#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "recording/avi_muxer.h"

namespace recording {

/** 后台录像服务：将 MJPEG 帧封装为 AVI 文件。 */
class RecordingService {
public:
    struct Config {
        std::string dir;         // 保存目录
        uint32_t width = 640;
        uint32_t height = 480;
        uint32_t fpsNum = 30;
        uint32_t fpsDen = 1;
    };

    RecordingService() = default;
    ~RecordingService();

    RecordingService(const RecordingService &) = delete;
    RecordingService &operator=(const RecordingService &) = delete;

    bool start(const Config &cfg);
    void pushFrame(const uint8_t *data, size_t size);  // 拷贝入队，供采集线程调用
    void stop();                                       // 停止并最终化文件

    bool isRecording() const { return recording_.load(); }
    const std::string &filePath() const { return filePath_; }
    std::string lastError() const { return lastError_; }
    uint32_t frameCount() const { return frameCount_.load(); }
    uint64_t bytesWritten() const { return bytesWritten_.load(); }
    uint32_t droppedFrames() const { return dropped_.load(); }
    double durationSec() const;

private:
    void writerLoop();
    static std::string generateFilename();

    AviMuxer muxer_;
    std::thread thread_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> stopRequested_{false};

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t maxQueue_ = 16;

    std::atomic<uint32_t> dropped_{0};
    std::atomic<uint32_t> frameCount_{0};
    std::atomic<uint64_t> bytesWritten_{0};
    uint32_t fpsNum_ = 30;
    uint32_t fpsDen_ = 1;

    std::string filePath_;
    std::string lastError_;
};

}  // namespace recording
