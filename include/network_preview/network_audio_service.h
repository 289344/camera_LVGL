#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace network_preview {

/** 将麦克风 PCM 帧通过独立低延迟 TCP 连接推送到 PC 预览网关。 */
class NetworkAudioService {
public:
    struct Config {
        std::string host;
        uint16_t port = 9003;
        uint16_t sampleRate = 16000;
        uint8_t channels = 1;
    };

    NetworkAudioService() = default;
    ~NetworkAudioService();

    NetworkAudioService(const NetworkAudioService &) = delete;
    NetworkAudioService &operator=(const NetworkAudioService &) = delete;

    bool start(const Config &config);
    void stop();
    void pushPcm(const uint8_t *data, size_t size);

    bool isActive() const { return running_.load(); }
    bool isConnected() const { return connected_.load(); }
    uint64_t sentChunks() const { return sentChunks_.load(); }
    uint64_t droppedChunks() const { return droppedChunks_.load(); }
    std::string lastError() const;

private:
    void workerLoop();
    int connectServer();
    bool sendAll(int fd, const uint8_t *data, size_t size);
    bool waitRetry(uint32_t milliseconds);
    void setError(const std::string &error);

    Config config_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> socketFd_{-1};
    std::atomic<uint64_t> sentChunks_{0};
    std::atomic<uint64_t> droppedChunks_{0};

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t maxQueue_ = 12;

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace network_preview
