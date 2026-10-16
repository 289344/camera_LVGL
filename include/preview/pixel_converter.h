#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace preview {

/** 将 YUYV 4:2:2 转换为 RGB565。 */
void yuyvToRgb565(const uint8_t *src, uint16_t *dst, uint32_t width, uint32_t height);

/** 将 JPEG 解码为 RGB565。返回 true 表示成功，并通过 outW/outH 返回实际尺寸。 */
bool jpegToRgb565(const uint8_t *jpeg, size_t size, uint16_t *dst,
                  uint32_t dstWidth, uint32_t dstHeight,
                  uint32_t &outW, uint32_t &outH);

/** 将 YUYV 编码为 JPEG（供录像使用）。返回 true 表示成功。 */
bool yuyvToJpeg(const uint8_t *yuyv, uint32_t width, uint32_t height,
                std::vector<uint8_t> &outJpeg, int quality = 85);

}  // namespace preview
