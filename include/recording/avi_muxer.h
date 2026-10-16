#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace recording {

/** 将 MJPEG 帧封装为 AVI（MJPG fourcc）文件。 */
class AviMuxer {
public:
    AviMuxer() = default;
    ~AviMuxer();

    AviMuxer(const AviMuxer &) = delete;
    AviMuxer &operator=(const AviMuxer &) = delete;

    bool open(const std::string &path, uint32_t width, uint32_t height,
              uint32_t fpsNum, uint32_t fpsDen);
    bool writeFrame(const uint8_t *jpeg, size_t size);
    bool close();

    bool isOpen() const { return fp_ != nullptr; }
    uint64_t bytesWritten() const { return bytesWritten_; }
    uint32_t frameCount() const { return frameCount_; }
    double durationSec() const;
    std::string lastError() const { return lastError_; }

private:
    bool writeHeader(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen);

    std::FILE *fp_ = nullptr;
    std::string lastError_;
    uint64_t bytesWritten_ = 0;
    uint32_t frameCount_ = 0;
    uint32_t fpsNum_ = 30;
    uint32_t fpsDen_ = 1;

    // 需要回填的位置
    long riffSizePos_ = 0;
    long hdrlSizePos_ = 0;
    long strlSizePos_ = 0;
    long avihFramesPos_ = 0;
    long strhLengthPos_ = 0;
    long moviSizePos_ = 0;

    struct IndexEntry {
        long offset;
        uint32_t size;
    };
    std::vector<IndexEntry> index_;
};

}  // namespace recording
