#pragma once

#include <cstdint>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace preview {

/** RGB565 帧缓冲池，用于采集线程解码与 GUI 线程显示之间的无撕裂交换。 */
class FramePool {
public:
    explicit FramePool(uint32_t width, uint32_t height, uint32_t count = 3);
    ~FramePool();

    FramePool(const FramePool &) = delete;
    FramePool &operator=(const FramePool &) = delete;

    bool valid() const { return !buffers_.empty() && width_ > 0; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    /** 采集线程：获取一个可写入的缓冲区索引（无空闲时阻塞）。 */
    int acquireWrite();
    /** 采集线程：发布已填好的缓冲区为最新帧。 */
    void publish(int index);

    /** GUI 线程：获取最新已发布帧的索引（无新帧返回 -1）。 */
    int tryGetLatest();
    /** GUI 线程：归还已显示的缓冲区。 */
    void release(int index);

    uint8_t *data(int index) { return buffers_[static_cast<size_t>(index)]; }
    const uint8_t *data(int index) const { return buffers_[static_cast<size_t>(index)]; }

private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t *> buffers_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<int> free_;
    int latest_ = -1;   // 等待 GUI 取走的最新帧
};

}  // namespace preview
