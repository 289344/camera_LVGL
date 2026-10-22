#include "network_preview/network_audio_service.h"

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

constexpr uint32_t kProtocolMagic = 0x43414D41;  // "CAMA"
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kAudioFrameType = 1;
constexpr uint16_t kPcmS16Le = 1;
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

NetworkAudioService::~NetworkAudioService() {
    stop();
}

bool NetworkAudioService::start(const Config &config) {
    stop();
    if (config.host.empty() || config.sampleRate == 0 || config.channels == 0) {
        setError("网络音频配置无效");
        return false;
    }

    config_ = config;
    connected_.store(false);
    socketFd_.store(-1);
    sentChunks_.store(0);
    droppedChunks_.store(0);
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_.clear();
    }

    running_.store(true);
    thread_ = std::thread(&NetworkAudioService::workerLoop, this);
    return true;
}

void NetworkAudioService::stop() {
    running_.store(false);
    connected_.store(false);
    queueCv_.notify_all();

    const int fd = socketFd_.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();

    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.clear();
}

void NetworkAudioService::pushPcm(const uint8_t *data, size_t size) {
    if (!running_.load() || !connected_.load() || !data || size == 0) return;

    std::vector<uint8_t> chunk(data, data + size);
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        if (queue_.size() >= maxQueue_) {
            queue_.pop_front();
            droppedChunks_.fetch_add(1);
        }
        queue_.push_back(std::move(chunk));
    }
    queueCv_.notify_one();
}

std::string NetworkAudioService::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

void NetworkAudioService::setError(const std::string &error) {
    std::lock_guard<std::mutex> lk(errorMutex_);
    lastError_ = error;
}

bool NetworkAudioService::waitRetry(uint32_t milliseconds) {
    std::unique_lock<std::mutex> lk(queueMutex_);
    queueCv_.wait_for(lk, std::chrono::milliseconds(milliseconds),
                      [this] { return !running_.load(); });
    return running_.load();
}

int NetworkAudioService::connectServer() {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    const std::string port = std::to_string(config_.port);
    const int gai = ::getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &result);
    if (gai != 0) {
        setError("解析音频服务器失败: " + std::string(::gai_strerror(gai)));
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
        setError("连接音频服务器失败 " + config_.host + ":" + port);
        return -1;
    }

    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.clear();
    }
    socketFd_.store(fd);
    connected_.store(true);
    setError("");
    return fd;
}

bool NetworkAudioService::sendAll(int fd, const uint8_t *data, size_t size) {
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

void NetworkAudioService::workerLoop() {
    int fd = -1;
    uint32_t sequence = 0;

    while (running_.load()) {
        if (fd < 0) {
            fd = connectServer();
            if (fd < 0) {
                if (!waitRetry(1000)) break;
                continue;
            }
        }

        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load()) break;
            chunk = std::move(queue_.front());
            queue_.pop_front();
        }

        uint8_t header[kHeaderSize] = {};
        putU32(header, kProtocolMagic);
        header[4] = kProtocolVersion;
        header[5] = kAudioFrameType;
        putU16(header + 6, 0);
        putU32(header + 8, ++sequence);
        putU64(header + 12, realtimeMs());
        putU16(header + 20, config_.sampleRate);
        putU16(header + 22, config_.channels);
        putU16(header + 24, kPcmS16Le);
        putU16(header + 26, 0);
        putU32(header + 28, static_cast<uint32_t>(chunk.size()));

        if (!sendAll(fd, header, sizeof(header)) ||
            !sendAll(fd, chunk.data(), chunk.size())) {
            setError("网络音频发送失败，正在重连");
            connected_.store(false);
            socketFd_.store(-1);
            ::close(fd);
            fd = -1;
            if (!waitRetry(500)) break;
            continue;
        }
        sentChunks_.fetch_add(1);
    }

    connected_.store(false);
    socketFd_.store(-1);
    if (fd >= 0) ::close(fd);
}

}  // namespace network_preview
