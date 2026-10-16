#include "camera/camera_device.h"

#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

namespace camera {

namespace {
std::string fourccName(uint32_t f) {
    char s[5] = {
        static_cast<char>(f & 0xff),
        static_cast<char>((f >> 8) & 0xff),
        static_cast<char>((f >> 16) & 0xff),
        static_cast<char>((f >> 24) & 0xff),
        0,
    };
    return std::string(s);
}
}  // namespace

CameraDevice::~CameraDevice() {
    close();
}

bool CameraDevice::open(const std::string &path) {
    close();
    lastError_.clear();

    fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        lastError_ = "打开设备失败: " + std::string(std::strerror(errno));
        fd_ = -1;
        return false;
    }

    v4l2_capability cap{};
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        lastError_ = "VIDIOC_QUERYCAP 失败: " + std::string(std::strerror(errno));
        close();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        lastError_ = "设备不是视频采集设备";
        close();
        return false;
    }
    path_ = path;
    return true;
}

void CameraDevice::close() {
    stop();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();
    width_ = height_ = 0;
    pixelformat_ = 0;
}

std::vector<VideoFormat> CameraDevice::listFormats() {
    std::vector<VideoFormat> result;
    if (fd_ < 0) return result;

    for (uint32_t i = 0;; ++i) {
        v4l2_fmtdesc f{};
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.index = i;
        if (::ioctl(fd_, VIDIOC_ENUM_FMT, &f) < 0) break;

        // 枚举该格式支持的分辨率
        for (uint32_t j = 0;; ++j) {
            v4l2_frmsizeenum fs{};
            fs.pixel_format = f.pixelformat;
            fs.index = j;
            if (::ioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) break;

            uint32_t w = 0, h = 0;
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                w = fs.discrete.width;
                h = fs.discrete.height;
            } else {
                // 连续尺寸：取最大值
                w = fs.stepwise.max_width;
                h = fs.stepwise.max_height;
            }
            if (w == 0 || h == 0) continue;

            // 枚举该分辨率下的帧率（取第一个离散值）
            uint32_t fps_num = 0, fps_den = 1;
            v4l2_frmivalenum fi{};
            fi.pixel_format = f.pixelformat;
            fi.width = w;
            fi.height = h;
            if (::ioctl(fd_, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0) {
                if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                    fps_num = fi.discrete.numerator;
                    fps_den = fi.discrete.denominator;
                } else if (fi.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
                    fps_num = fi.stepwise.max.numerator;
                    fps_den = fi.stepwise.max.denominator;
                }
            }

            VideoFormat vf;
            vf.pixelformat = f.pixelformat;
            vf.width = w;
            vf.height = h;
            vf.fps_num = fps_num;
            vf.fps_den = fps_den;
            vf.name = fourccName(f.pixelformat);
            result.push_back(vf);
        }
    }
    return result;
}

bool CameraDevice::setFormat(uint32_t pixelformat, uint32_t width, uint32_t height) {
    if (fd_ < 0) return false;
    lastError_.clear();

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pixelformat;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        lastError_ = "VIDIOC_S_FMT 失败: " + std::string(std::strerror(errno));
        return false;
    }
    width_ = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    pixelformat_ = fmt.fmt.pix.pixelformat;
    return true;
}

bool CameraDevice::start(FrameCallback cb) {
    if (fd_ < 0 || !cb) {
        lastError_ = "设备未打开或回调为空";
        return false;
    }
    stop();

    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        lastError_ = "VIDIOC_REQBUFS 失败: " + std::string(std::strerror(errno));
        return false;
    }
    if (req.count < 2) {
        lastError_ = "设备提供的缓冲区不足";
        return false;
    }

    buffers_.clear();
    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (::ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            lastError_ = "VIDIOC_QUERYBUF 失败: " + std::string(std::strerror(errno));
            return false;
        }
        buffers_[i].length = buf.length;
        buffers_[i].start = static_cast<uint8_t *>(
            ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset));
        if (buffers_[i].start == MAP_FAILED) {
            buffers_[i].start = nullptr;
            lastError_ = "mmap 失败: " + std::string(std::strerror(errno));
            return false;
        }
        if (::ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            lastError_ = "VIDIOC_QBUF 失败: " + std::string(std::strerror(errno));
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        lastError_ = "VIDIOC_STREAMON 失败: " + std::string(std::strerror(errno));
        return false;
    }

    callback_ = std::move(cb);
    stopRequested_.store(false);
    streaming_.store(true);
    thread_ = std::thread(&CameraDevice::captureLoop, this);
    return true;
}

void CameraDevice::stop() {
    if (!streaming_.load() && !thread_.joinable()) {
        // 尚未开始采集
    }
    stopRequested_.store(true);
    if (fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ::ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    streaming_.store(false);

    for (auto &b : buffers_) {
        if (b.start) ::munmap(b.start, b.length);
    }
    buffers_.clear();
    callback_ = nullptr;
}

void CameraDevice::captureLoop() {
    while (!stopRequested_.load()) {
        pollfd p{};
        p.fd = fd_;
        p.events = POLLIN;
        int pr = ::poll(&p, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }

        Frame frame;
        frame.data = buffers_[buf.index].start;
        frame.size = buf.bytesused;
        frame.width = width_;
        frame.height = height_;
        frame.pixelformat = pixelformat_;
        frame.timestamp_us = static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000ULL +
                             static_cast<uint64_t>(buf.timestamp.tv_usec);
        if (callback_) callback_(frame);

        ::ioctl(fd_, VIDIOC_QBUF, &buf);
    }
}

}  // namespace camera
