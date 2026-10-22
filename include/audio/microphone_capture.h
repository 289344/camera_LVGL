#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace audio {

/** 使用 arecord 采集原始 PCM，并把同一路麦克风数据分发给录像和网络预览。 */
class MicrophoneCapture {
public:
    using DataCallback = std::function<void(const uint8_t *data, size_t size)>;

    struct Config {
        std::string device;
        uint32_t sampleRate = 16000;
        uint32_t channels = 1;
        uint32_t periodFrames = 640;
        uint32_t bufferFrames = 1920;
    };

    MicrophoneCapture() = default;
    ~MicrophoneCapture();

    MicrophoneCapture(const MicrophoneCapture &) = delete;
    MicrophoneCapture &operator=(const MicrophoneCapture &) = delete;

    bool start(const Config &config, DataCallback callback);
    void stop();

    bool isActive() const { return running_.load(); }
    uint32_t sampleRate() const { return config_.sampleRate; }
    uint32_t channels() const { return config_.channels; }
    std::string lastError() const;

private:
    void captureLoop();
    void setError(const std::string &error);

    Config config_;
    DataCallback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int pipeFd_ = -1;
    int childPid_ = -1;

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace audio
