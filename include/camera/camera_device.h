#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

namespace camera {

/** 视频帧：数据指针仅在回调期间有效，若需保留请拷贝。 */
struct Frame {
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixelformat = 0;      // V4L2_PIX_FMT_* (fourcc)
    uint64_t timestamp_us = 0;
};

struct VideoFormat {
    uint32_t pixelformat = 0;      // V4L2_PIX_FMT_* (fourcc)
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 0;
    uint32_t fps_den = 1;
    std::string name;              // 四字符可读名，如 "MJPG" / "YUYV"
};

class CameraDevice {
public:
    using FrameCallback = std::function<void(const Frame &)>;

    CameraDevice() = default;
    ~CameraDevice();
    CameraDevice(const CameraDevice &) = delete;
    CameraDevice &operator=(const CameraDevice &) = delete;

    bool open(const std::string &path);
    void close();

    bool isOpen() const { return fd_ >= 0; }
    const std::string &path() const { return path_; }

    std::vector<VideoFormat> listFormats();
    bool setFormat(uint32_t pixelformat, uint32_t width, uint32_t height);
    bool start(FrameCallback cb);
    void stop();
    bool isStreaming() const { return streaming_.load(); }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pixelformat() const { return pixelformat_; }
    std::string lastError() const { return lastError_; }

private:
    void captureLoop();

    int fd_ = -1;
    std::string path_;
    std::string lastError_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t pixelformat_ = 0;

    struct Buffer {
        uint8_t *start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> buffers_;

    std::atomic<bool> streaming_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;
    FrameCallback callback_;
};

}  // namespace camera
