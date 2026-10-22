#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace network_preview {

/** 将摄像头 JPEG 帧通过低延迟 TCP 帧流推送到 PC 预览服务器。 */
class NetworkPreviewService {
public:
    struct Config {
        std::string host;
        uint16_t port = 9002;
        uint32_t width = 640;
        uint32_t height = 480;
        uint32_t maxFps = 30;
    };

    NetworkPreviewService() = default;
    ~NetworkPreviewService();

    NetworkPreviewService(const NetworkPreviewService &) = delete;
    NetworkPreviewService &operator=(const NetworkPreviewService &) = delete;

    bool start(const Config &config);
    void stop();
    void pushFrame(const uint8_t *data, size_t size);

    bool isActive() const { return running_.load(); }
    bool isConnected() const { return connected_.load(); }
    uint64_t sentFrames() const { return sentFrames_.load(); }
    uint64_t droppedFrames() const { return droppedFrames_.load(); }
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
    std::atomic<uint64_t> sentFrames_{0};
    std::atomic<uint64_t> droppedFrames_{0};

    std::mutex frameMutex_;
    std::condition_variable frameCv_;
    std::vector<uint8_t> latestFrame_;
    uint64_t latestGeneration_ = 0;
    uint64_t latestTimestampMs_ = 0;
    uint64_t lastAcceptedMs_ = 0;

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace network_preview
