#include "recording/avi_muxer.h"

#include <cstring>
#include <cerrno>

namespace recording {

namespace {

void putU16(std::FILE *f, uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff)};
    std::fwrite(b, 1, 2, f);
}
void putU32(std::FILE *f, uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff),
                    static_cast<uint8_t>((v >> 16) & 0xff), static_cast<uint8_t>((v >> 24) & 0xff)};
    std::fwrite(b, 1, 4, f);
}
void putFourcc(std::FILE *f, const char *s) {
    std::fwrite(s, 1, 4, f);
}
void putZeros(std::FILE *f, size_t n) {
    static const uint8_t zeros[64] = {0};
    while (n > 0) {
        size_t chunk = n > sizeof(zeros) ? sizeof(zeros) : n;
        std::fwrite(zeros, 1, chunk, f);
        n -= chunk;
    }
}

long tell(std::FILE *f) { return std::ftell(f); }

void patchU32(std::FILE *f, long pos, uint32_t v) {
    long cur = std::ftell(f);
    std::fseek(f, pos, SEEK_SET);
    putU32(f, v);
    std::fseek(f, cur, SEEK_SET);
}

}  // namespace

AviMuxer::~AviMuxer() {
    close();
}

bool AviMuxer::open(const std::string &path, uint32_t width, uint32_t height,
                    uint32_t fpsNum, uint32_t fpsDen) {
    close();
    lastError_.clear();

    fp_ = std::fopen(path.c_str(), "wb");
    if (!fp_) {
        lastError_ = "无法创建文件: " + std::string(std::strerror(errno));
        return false;
    }
    fpsNum_ = fpsNum;
    fpsDen_ = fpsDen;
    bytesWritten_ = 0;
    frameCount_ = 0;
    index_.clear();

    if (!writeHeader(width, height, fpsNum, fpsDen)) {
        std::fclose(fp_);
        fp_ = nullptr;
        return false;
    }
    return true;
}

bool AviMuxer::writeHeader(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen) {
    // RIFF 头
    putFourcc(fp_, "RIFF");
    riffSizePos_ = tell(fp_);
    putU32(fp_, 0);  // 占位
    putFourcc(fp_, "AVI ");

    // hdrl LIST
    putFourcc(fp_, "LIST");
    hdrlSizePos_ = tell(fp_);
    putU32(fp_, 0);  // 占位（hdrl 大小）
    putFourcc(fp_, "hdrl");

    // avih
    putFourcc(fp_, "avih");
    putU32(fp_, 56);
    putU32(fp_, fpsDen_ * 1000000 / fpsNum_);  // dwMicroSecPerFrame
    putU32(fp_, 0);                             // dwMaxBytesPerSec（稍后可回填，也可留 0）
    putU32(fp_, 0);                             // dwPaddingGranularity
    putU32(fp_, 0x10);                          // dwFlags = AVIF_HASINDEX
    avihFramesPos_ = tell(fp_);
    putU32(fp_, 0);                             // dwTotalFrames
    putU32(fp_, 0);                             // dwInitialFrames
    putU32(fp_, 1);                             // dwStreams
    putU32(fp_, width * height * 2);            // dwSuggestedBufferSize
    putU32(fp_, width);
    putU32(fp_, height);
    putZeros(fp_, 16);                          // dwReserved[4]

    // strl LIST
    putFourcc(fp_, "LIST");
    strlSizePos_ = tell(fp_);
    putU32(fp_, 0);  // 占位（strl 大小）
    putFourcc(fp_, "strl");

    // strh
    putFourcc(fp_, "strh");
    putU32(fp_, 56);
    putFourcc(fp_, "vids");
    putFourcc(fp_, "MJPG");
    putU32(fp_, 0);              // dwFlags
    putU16(fp_, 0);              // wPriority
    putU16(fp_, 0);              // wLanguage
    putU32(fp_, 0);              // dwInitialFrames
    putU32(fp_, fpsDen_);        // dwScale
    putU32(fp_, fpsNum_);        // dwRate
    putU32(fp_, 0);              // dwStart
    strhLengthPos_ = tell(fp_);
    putU32(fp_, 0);              // dwLength（回填总帧数）
    putU32(fp_, width * height * 2);  // dwSuggestedBufferSize
    putU32(fp_, 0xFFFFFFFF);     // dwQuality
    putU32(fp_, 0);              // dwSampleSize
    putU16(fp_, 0); putU16(fp_, 0);  // rcFrame.left/top
    putU16(fp_, static_cast<uint16_t>(width));
    putU16(fp_, static_cast<uint16_t>(height));  // rcFrame.right/bottom

    // strf (BITMAPINFOHEADER)
    putFourcc(fp_, "strf");
    putU32(fp_, 40);
    putU32(fp_, 40);             // biSize
    putU32(fp_, width);          // biWidth
    putU32(fp_, height);         // biHeight
    putU16(fp_, 1);              // biPlanes
    putU16(fp_, 24);             // biBitCount
    putFourcc(fp_, "MJPG");      // biCompression
    putU32(fp_, width * height * 3);  // biSizeImage
    putU32(fp_, 0);              // biXPelsPerMeter
    putU32(fp_, 0);              // biYPelsPerMeter
    putU32(fp_, 0);              // biClrUsed
    putU32(fp_, 0);              // biClrImportant

    // movi LIST
    putFourcc(fp_, "LIST");
    moviSizePos_ = tell(fp_);
    putU32(fp_, 0);  // 占位
    putFourcc(fp_, "movi");

    return std::ferror(fp_) == 0;
}

bool AviMuxer::writeFrame(const uint8_t *jpeg, size_t size) {
    if (!fp_ || !jpeg || size == 0) return false;

    long chunkOffset = tell(fp_);
    putFourcc(fp_, "00dc");
    putU32(fp_, static_cast<uint32_t>(size));
    std::fwrite(jpeg, 1, size, fp_);
    if (size & 1) putZeros(fp_, 1);  // AVI 块 2 字节对齐

    index_.push_back({chunkOffset, static_cast<uint32_t>(size)});
    frameCount_++;
    bytesWritten_ += size + 8 + (size & 1);
    return std::ferror(fp_) == 0;
}

bool AviMuxer::close() {
    if (!fp_) return true;

    // 写 idx1 索引
    long idxPos = tell(fp_);
    if (!index_.empty()) {
        putFourcc(fp_, "idx1");
        putU32(fp_, static_cast<uint32_t>(index_.size() * 16));
        for (const auto &e : index_) {
            putFourcc(fp_, "00dc");
            putU32(fp_, 0x10);  // AVIIF_KEYFRAME
            putU32(fp_, static_cast<uint32_t>(e.offset));
            putU32(fp_, e.size);
        }
    }

    long fileEnd = tell(fp_);

    // 回填各大小字段
    long moviStart = moviSizePos_ - 4;   // movi LIST 的 "LIST" 标记位置
    long moviDataEnd = index_.empty() ? (moviSizePos_ + 4) : idxPos;

    patchU32(fp_, riffSizePos_, static_cast<uint32_t>(fileEnd - 8));
    // hdrl LIST 大小 = 从 "hdrl" 四字符到 movi LIST 之前
    patchU32(fp_, hdrlSizePos_, static_cast<uint32_t>(moviStart - (hdrlSizePos_ + 4)));
    // strl LIST 大小 = 从 "strl" 四字符到 hdrl 数据结束
    patchU32(fp_, strlSizePos_, static_cast<uint32_t>(moviStart - (strlSizePos_ + 4)));
    patchU32(fp_, avihFramesPos_, frameCount_);
    patchU32(fp_, strhLengthPos_, frameCount_);
    // movi LIST 大小 = 从 "movi" 四字符到 idx1 之前
    patchU32(fp_, moviSizePos_, static_cast<uint32_t>(moviDataEnd - (moviStart + 8)));

    std::fflush(fp_);
    bool ok = std::ferror(fp_) == 0;
    std::fclose(fp_);
    fp_ = nullptr;
    if (!ok) lastError_ = "写入文件失败";
    return ok;
}

double AviMuxer::durationSec() const {
    if (fpsNum_ == 0) return 0.0;
    return static_cast<double>(frameCount_) * fpsDen_ / fpsNum_;
}

}  // namespace recording
