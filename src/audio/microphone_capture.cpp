#include "audio/microphone_capture.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace audio {

MicrophoneCapture::~MicrophoneCapture() {
    stop();
}

bool MicrophoneCapture::start(const Config &config, DataCallback callback) {
    stop();
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_.clear();
    }

    if (config.device.empty() || config.sampleRate == 0 || config.channels == 0 ||
        config.periodFrames == 0 || config.bufferFrames < config.periodFrames) {
        setError("麦克风采集配置无效");
        return false;
    }
    if (!callback) {
        setError("麦克风数据回调为空");
        return false;
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        setError("创建麦克风管道失败: " + std::string(std::strerror(errno)));
        return false;
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipeFds[0]);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        if (pipeFds[1] > STDERR_FILENO) ::close(pipeFds[1]);

        const int logFd = ::open("/tmp/camera_microphone.log",
                                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd >= 0) {
            ::dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) ::close(logFd);
        }

        const std::string rate = std::to_string(config.sampleRate);
        const std::string channels = std::to_string(config.channels);
        const std::string period = std::to_string(config.periodFrames);
        const std::string buffer = std::to_string(config.bufferFrames);
        ::execl("/usr/bin/arecord", "arecord", "-q", "-D", config.device.c_str(),
                "-t", "raw", "-f", "S16_LE", "-c", channels.c_str(),
                "-r", rate.c_str(), "--period-size", period.c_str(),
                "--buffer-size", buffer.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        setError("启动 arecord 失败: " + std::string(std::strerror(errno)));
        return false;
    }

    ::close(pipeFds[1]);
    config_ = config;
    callback_ = std::move(callback);
    pipeFd_ = pipeFds[0];
    childPid_ = static_cast<int>(pid);
    running_.store(true);
    thread_ = std::thread(&MicrophoneCapture::captureLoop, this);
    return true;
}

void MicrophoneCapture::stop() {
    running_.store(false);
    if (childPid_ > 0) {
        ::kill(static_cast<pid_t>(childPid_), SIGTERM);
    }
    if (thread_.joinable()) thread_.join();
    if (pipeFd_ >= 0) {
        ::close(pipeFd_);
        pipeFd_ = -1;
    }
    if (childPid_ > 0) {
        int status = 0;
        ::waitpid(static_cast<pid_t>(childPid_), &status, 0);
        childPid_ = -1;
    }
    callback_ = nullptr;
}

void MicrophoneCapture::captureLoop() {
    const size_t bytesPerFrame = sizeof(int16_t) * config_.channels;
    const size_t chunkSize = bytesPerFrame * config_.periodFrames;
    std::vector<uint8_t> buffer(chunkSize);

    while (running_.load()) {
        size_t offset = 0;
        while (offset < chunkSize && running_.load()) {
            const ssize_t n = ::read(pipeFd_, buffer.data() + offset, chunkSize - offset);
            if (n > 0) {
                offset += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (running_.load()) setError("arecord 已退出，请检查 /tmp/camera_microphone.log");
            running_.store(false);
            break;
        }
        if (offset == chunkSize && callback_) callback_(buffer.data(), buffer.size());
    }
    running_.store(false);
}

std::string MicrophoneCapture::lastError() const {
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

void MicrophoneCapture::setError(const std::string &error) {
    std::lock_guard<std::mutex> lk(errorMutex_);
    if (lastError_.empty()) lastError_ = error;
}

}  // namespace audio
