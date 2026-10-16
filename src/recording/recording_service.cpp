#include "recording/recording_service.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace recording {

RecordingService::~RecordingService() {
    stop();
}

std::string RecordingService::generateFilename() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rec_%04d%02d%02d_%02d%02d%02d.avi",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

bool RecordingService::start(const Config &cfg) {
    stop();
    lastError_.clear();

    std::error_code ec;
    std::filesystem::create_directories(cfg.dir, ec);
    if (ec) {
        lastError_ = "创建目录失败: " + cfg.dir;
        return false;
    }

    filePath_ = cfg.dir + "/" + generateFilename();
    if (!muxer_.open(filePath_, cfg.width, cfg.height, cfg.fpsNum, cfg.fpsDen)) {
        lastError_ = muxer_.lastError();
        return false;
    }

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
        return;
    }
    stopRequested_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    recording_.store(false);
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

        if (muxer_.writeFrame(frame.data(), frame.size())) {
            frameCount_.store(muxer_.frameCount());
            bytesWritten_.store(muxer_.bytesWritten());
        } else {
            lastError_ = muxer_.lastError();
        }
    }
    muxer_.close();
    if (!muxer_.lastError().empty()) lastError_ = muxer_.lastError();
}

double RecordingService::durationSec() const {
    if (fpsNum_ == 0) return 0.0;
    return static_cast<double>(frameCount_.load()) * fpsDen_ / fpsNum_;
}

}  // namespace recording
