#include "recording/recording_service.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs_compat.h"

namespace recording {
namespace {

std::string ratioString(uint32_t numerator, uint32_t denominator) {
    if (denominator == 0) denominator = 1;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u/%u", numerator, denominator);
    return std::string(buf);
}

void runFfmpeg(const std::string &path, const RecordingService::Config &cfg, int inputFd) {
    const std::string fps = ratioString(cfg.fpsNum, cfg.fpsDen);
    const std::string channels = std::to_string(cfg.audioChannels);
    const std::string sampleRate = std::to_string(cfg.audioSampleRate);
    const std::string bitrate = std::to_string(cfg.audioBitrate);

    ::dup2(inputFd, STDIN_FILENO);
    if (inputFd != STDIN_FILENO) ::close(inputFd);

    int logFd = ::open("/tmp/camera_recording.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logFd >= 0) {
        ::dup2(logFd, STDOUT_FILENO);
        ::dup2(logFd, STDERR_FILENO);
        if (logFd > STDERR_FILENO) ::close(logFd);
    }

    ::execl("/usr/bin/ffmpeg", "ffmpeg",
            "-nostdin", "-hide_banner", "-loglevel", "warning", "-y",
            "-use_wallclock_as_timestamps", "1",
            "-thread_queue_size", "512",
            "-f", "mjpeg", "-framerate", fps.c_str(), "-i", "pipe:0",
            "-thread_queue_size", "512",
            "-f", "alsa", "-channels", channels.c_str(),
            "-sample_rate", sampleRate.c_str(), "-i", cfg.audioDevice.c_str(),
            "-map", "0:v:0", "-map", "1:a:0",
            "-c:v", "copy", "-c:a", "aac", "-b:a", bitrate.c_str(),
            "-vsync", "vfr", "-shortest", "-movflags", "+faststart", path.c_str(),
            static_cast<char *>(nullptr));
    ::_exit(127);
}

}  // namespace

RecordingService::~RecordingService() {
    stop();
}

std::string RecordingService::generateFilename() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rec_%04d%02d%02d_%02d%02d%02d.mp4",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

std::string RecordingService::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

void RecordingService::setError(const std::string &error) {
    std::lock_guard<std::mutex> lk(errorMutex_);
    if (lastError_.empty()) lastError_ = error;
}

bool RecordingService::start(const Config &cfg) {
    stop();
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_.clear();
    }

    if (!fscompat::createDirectories(cfg.dir)) {
        setError("创建目录失败: " + cfg.dir);
        return false;
    }
    if (cfg.audioDevice.empty()) {
        setError("未配置麦克风设备");
        return false;
    }

    filePath_ = cfg.dir + "/" + generateFilename();
    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        setError("创建录像管道失败: " + std::string(std::strerror(errno)));
        return false;
    }

    // ALSA 设备异常或 FFmpeg 提前退出时，写入管道应返回 EPIPE，而不是终止主程序。
    std::signal(SIGPIPE, SIG_IGN);

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipeFds[1]);
        runFfmpeg(filePath_, cfg, pipeFds[0]);
    }
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        setError("启动 FFmpeg 失败: " + std::string(std::strerror(errno)));
        return false;
    }

    ::close(pipeFds[0]);
    std::FILE *input = ::fdopen(pipeFds[1], "wb");
    if (!input) {
        ::close(pipeFds[1]);
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        setError("打开 FFmpeg 输入管道失败");
        return false;
    }
    std::setvbuf(input, nullptr, _IONBF, 0);

    ffmpegIn_ = input;
    ffmpegPid_ = static_cast<int>(pid);
    fpsNum_ = cfg.fpsNum;
    fpsDen_ = cfg.fpsDen;
    dropped_.store(0);
    frameCount_.store(0);
    bytesWritten_.store(0);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }

    stopRequested_.store(false);
    recording_.store(true);
    thread_ = std::thread(&RecordingService::writerLoop, this);
    return true;
}

void RecordingService::pushFrame(const uint8_t *data, size_t size) {
    if (!recording_.load() || !data || size == 0) return;

    std::vector<uint8_t> buf(data, data + size);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.size() >= maxQueue_) {
            queue_.pop_front();  // 丢弃最旧帧，避免阻塞采集线程
            dropped_.fetch_add(1);
        }
        queue_.push_back(std::move(buf));
    }
    cv_.notify_one();
}

void RecordingService::stop() {
    if (!recording_.load() && !thread_.joinable()) {
        waitForFfmpeg();
        return;
    }

    // 先拒绝新帧，再让写线程排空已有队列。
    recording_.store(false);
    stopRequested_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    waitForFfmpeg();
}

void RecordingService::writerLoop() {
    while (true) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopRequested_.load() || !queue_.empty(); });
            if (queue_.empty() && stopRequested_.load()) break;
            frame = std::move(queue_.front());
            queue_.pop_front();
        }

        if (!ffmpegIn_) break;
        const size_t written = std::fwrite(frame.data(), 1, frame.size(), ffmpegIn_);
        if (written != frame.size()) {
            setError("写入 FFmpeg 失败，麦克风或编码器可能不可用");
            break;
        }
        frameCount_.fetch_add(1);
        bytesWritten_.fetch_add(frame.size());
    }

    if (ffmpegIn_) {
        std::fclose(ffmpegIn_);
        ffmpegIn_ = nullptr;
    }
}

void RecordingService::waitForFfmpeg() {
    if (ffmpegPid_ <= 0) return;

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (true) {
        const pid_t result = ::waitpid(static_cast<pid_t>(ffmpegPid_), &status, WNOHANG);
        if (result == static_cast<pid_t>(ffmpegPid_)) break;
        if (result < 0 && errno == ECHILD) {
            status = 0;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(static_cast<pid_t>(ffmpegPid_), SIGTERM);
            ::waitpid(static_cast<pid_t>(ffmpegPid_), &status, 0);
            setError("FFmpeg 停止超时");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ffmpegPid_ = -1;

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        setError("音视频封装失败，详见 /tmp/camera_recording.log");
    }
    if (fscompat::fileSize(filePath_) == 0) {
        setError("录像文件为空，请检查麦克风设备");
    }
}

double RecordingService::durationSec() const {
    if (fpsNum_ == 0) return 0.0;
    return static_cast<double>(frameCount_.load()) * fpsDen_ / fpsNum_;
}

}  // namespace recording
