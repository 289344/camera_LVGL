#include "network_preview/network_preview_service.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace network_preview {
namespace {

constexpr uint32_t kProtocolMagic = 0x43414D4C;  // "CAML"
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kFrameTypeJpeg = 1;
constexpr uint16_t kPixelFormatJpeg = 1;
constexpr size_t kHeaderSize = 32;

uint64_t monotonicMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint64_t realtimeMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void putU16(uint8_t *dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value >> 8);
    dst[1] = static_cast<uint8_t>(value);
}

void putU32(uint8_t *dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value >> 24);
    dst[1] = static_cast<uint8_t>(value >> 16);
    dst[2] = static_cast<uint8_t>(value >> 8);
    dst[3] = static_cast<uint8_t>(value);
}

void putU64(uint8_t *dst, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>(value >> (56 - i * 8));
    }
}

}  // namespace

NetworkPreviewService::~NetworkPreviewService() {
    stop();
}

bool NetworkPreviewService::start(const Config &config) {
    stop();
    if (config.host.empty() || config.width == 0 || config.height == 0) {
        setError("网络预览配置无效");
        return false;
    }

    config_ = config;
    if (config_.maxFps == 0) config_.maxFps = 30;
    sentFrames_.store(0);
    droppedFrames_.store(0);
    connected_.store(false);
    socketFd_.store(-1);
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        latestFrame_.clear();
        latestGeneration_ = 0;
        latestTimestampMs_ = 0;
        lastAcceptedMs_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_.clear();
    }

    running_.store(true);
    thread_ = std::thread(&NetworkPreviewService::workerLoop, this);
    return true;
}

void NetworkPreviewService::stop() {
    running_.store(false);
    connected_.store(false);
    frameCv_.notify_all();

    const int fd = socketFd_.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();

    std::lock_guard<std::mutex> lk(frameMutex_);
    latestFrame_.clear();
}

void NetworkPreviewService::pushFrame(const uint8_t *data, size_t size) {
    if (!running_.load() || !data || size == 0) return;

    const uint64_t now = monotonicMs();
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        const uint64_t minInterval = 1000U / config_.maxFps;
        if (lastAcceptedMs_ != 0 && now - lastAcceptedMs_ < minInterval) return;
        lastAcceptedMs_ = now;

        if (!latestFrame_.empty()) droppedFrames_.fetch_add(1);
        latestFrame_.assign(data, data + size);
        latestTimestampMs_ = realtimeMs();
        ++latestGeneration_;
    }
    frameCv_.notify_one();
}

std::string NetworkPreviewService::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

void NetworkPreviewService::setError(const std::string &error) {
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_ = error;
}

bool NetworkPreviewService::waitRetry(uint32_t milliseconds) {
    std::unique_lock<std::mutex> lk(frameMutex_);
    frameCv_.wait_for(lk, std::chrono::milliseconds(milliseconds),
                      [this] { return !running_.load(); });
    return running_.load();
}

int NetworkPreviewService::connectServer() {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    const std::string port = std::to_string(config_.port);
    const int gai = ::getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &result);
    if (gai != 0) {
        setError("解析服务器地址失败: " + std::string(::gai_strerror(gai)));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai && running_.load(); ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;
        }

        if (rc != 0) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            const uint64_t deadline = monotonicMs() + 3000;
            bool connected = false;
            while (running_.load() && monotonicMs() < deadline) {
                const int pr = ::poll(&pfd, 1, 100);
                if (pr > 0) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 &&
                        error == 0) {
                        connected = true;
                    }
                    break;
                }
                if (pr < 0 && errno != EINTR) break;
            }
            if (!connected) {
                ::close(fd);
                fd = -1;
                continue;
            }
        }

        ::fcntl(fd, F_SETFL, flags);
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        break;
    }
    ::freeaddrinfo(result);

    if (fd < 0) {
        setError("连接预览服务器失败 " + config_.host + ":" + port);
        return -1;
    }

    socketFd_.store(fd);
    connected_.store(true);
    setError("");
    return fd;
}

bool NetworkPreviewService::sendAll(int fd, const uint8_t *data, size_t size) {
    size_t offset = 0;
    while (offset < size && running_.load()) {
        const ssize_t n = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (n > 0) {
            offset += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return offset == size;
}

void NetworkPreviewService::workerLoop() {
    int fd = -1;
    uint64_t consumedGeneration = 0;

    while (running_.load()) {
        if (fd < 0) {
            fd = connectServer();
            if (fd < 0) {
                if (!waitRetry(1000)) break;
                continue;
            }
        }

        std::vector<uint8_t> frame;
        uint64_t generation = 0;
        uint64_t timestamp = 0;
        {
            std::unique_lock<std::mutex> lk(frameMutex_);
            frameCv_.wait(lk, [this, consumedGeneration] {
                return !running_.load() || latestGeneration_ != consumedGeneration;
            });
            if (!running_.load()) break;
            frame.swap(latestFrame_);
            generation = latestGeneration_;
            timestamp = latestTimestampMs_;
            latestFrame_.clear();
        }

        uint8_t header[kHeaderSize] = {};
        putU32(header, kProtocolMagic);
        header[4] = kProtocolVersion;
        header[5] = kFrameTypeJpeg;
        putU16(header + 6, 0);
        putU32(header + 8, static_cast<uint32_t>(generation));
        putU64(header + 12, timestamp);
        putU16(header + 20, static_cast<uint16_t>(config_.width));
        putU16(header + 22, static_cast<uint16_t>(config_.height));
        putU16(header + 24, kPixelFormatJpeg);
        putU16(header + 26, 0);
        putU32(header + 28, static_cast<uint32_t>(frame.size()));

        if (!sendAll(fd, header, sizeof(header)) ||
            !sendAll(fd, frame.data(), frame.size())) {
            setError("网络预览发送失败，正在重连");
            connected_.store(false);
            socketFd_.store(-1);
            ::close(fd);
            fd = -1;
            if (!waitRetry(500)) break;
            continue;
        }

        consumedGeneration = generation;
        sentFrames_.fetch_add(1);
    }

    connected_.store(false);
    socketFd_.store(-1);
    if (fd >= 0) ::close(fd);
}

}  // namespace network_preview
