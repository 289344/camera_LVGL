#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace recording {

/** 后台录像服务：将 MJPEG 帧和 ALSA 麦克风音频封装为 MP4。 */
class RecordingService {
public:
    struct Config {
        std::string dir;         // 保存目录
        uint32_t width = 640;
        uint32_t height = 480;
        uint32_t fpsNum = 30;
        uint32_t fpsDen = 1;
        std::string audioDevice = "plughw:1,0";
        uint32_t audioChannels = 1;
        uint32_t audioSampleRate = 16000;
        uint32_t audioBitrate = 64000;
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
    std::string lastError() const;
    uint32_t frameCount() const { return frameCount_.load(); }
    uint64_t bytesWritten() const { return bytesWritten_.load(); }
    uint32_t droppedFrames() const { return dropped_.load(); }
    double durationSec() const;

private:
    void writerLoop();
    void waitForFfmpeg();
    void setError(const std::string &error);
    static std::string generateFilename();

    std::thread thread_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> stopRequested_{false};
    std::FILE *ffmpegIn_ = nullptr;
    int ffmpegPid_ = -1;

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
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace recording
