#include "preview/frame_pool.h"

#include <cstring>

namespace preview {

FramePool::FramePool(uint32_t width, uint32_t height, uint32_t count)
    : width_(width), height_(height) {
    if (width == 0 || height == 0 || count == 0) return;
    const size_t bytes = static_cast<size_t>(width) * height * 2;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *buf = new uint8_t[bytes];
        std::memset(buf, 0, bytes);
        buffers_.push_back(buf);
        free_.push_back(static_cast<int>(i));
    }
}

FramePool::~FramePool() {
    for (auto *b : buffers_) delete[] b;
    buffers_.clear();
}

int FramePool::acquireWrite() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !free_.empty(); });
    int idx = free_.front();
    free_.pop_front();
    return idx;
}

void FramePool::publish(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 若上一帧尚未被 GUI 取走，则归还为可用
    if (latest_ >= 0) {
        free_.push_back(latest_);
    }
    latest_ = index;
    cv_.notify_one();
}

int FramePool::tryGetLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_ < 0) return -1;
    int idx = latest_;
    latest_ = -1;
    return idx;
}

void FramePool::release(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_.push_back(index);
    cv_.notify_one();
}

}  // namespace preview
