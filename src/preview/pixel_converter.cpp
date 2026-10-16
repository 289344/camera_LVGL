#include "preview/pixel_converter.h"

#include <vector>
#include <cstdio>
#include <cstring>
#include <jpeglib.h>

namespace preview {

namespace {
inline uint16_t rgb888To565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

inline int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}
}  // namespace

void yuyvToRgb565(const uint8_t *src, uint16_t *dst, uint32_t width, uint32_t height) {
    // YUYV 4:2:2：每两个像素占用 4 字节 [Y0 U Y1 V]
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = src + static_cast<size_t>(y) * width * 2;
        uint16_t *drow = dst + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x + 1 < width; x += 2) {
            int y0 = row[x * 2 + 0];
            int u = row[x * 2 + 1] - 128;
            int y1 = row[x * 2 + 2];
            int v = row[x * 2 + 3] - 128;

            int c = y0 - 16;
            int r0 = clamp255((298 * c + 409 * v + 128) >> 8);
            int g0 = clamp255((298 * c - 100 * u - 208 * v + 128) >> 8);
            int b0 = clamp255((298 * c + 516 * u + 128) >> 8);

            c = y1 - 16;
            int r1 = clamp255((298 * c + 409 * v + 128) >> 8);
            int g1 = clamp255((298 * c - 100 * u - 208 * v + 128) >> 8);
            int b1 = clamp255((298 * c + 516 * u + 128) >> 8);

            drow[x] = rgb888To565(static_cast<uint8_t>(r0), static_cast<uint8_t>(g0),
                                  static_cast<uint8_t>(b0));
            drow[x + 1] = rgb888To565(static_cast<uint8_t>(r1), static_cast<uint8_t>(g1),
                                      static_cast<uint8_t>(b1));
        }
    }
}

bool jpegToRgb565(const uint8_t *jpeg, size_t size, uint16_t *dst,
                  uint32_t dstWidth, uint32_t dstHeight,
                  uint32_t &outW, uint32_t &outH) {
    jpeg_decompress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<uint8_t *>(jpeg), size);

    bool ok = false;
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const uint32_t w = cinfo.output_width;
    const uint32_t h = cinfo.output_height;
    outW = w;
    outH = h;

    if (w == dstWidth && h == dstHeight) {
        // 尺寸一致：逐行直接转换
        std::vector<uint8_t> row(w * 3);
        uint8_t *rowptr = row.data();
        for (uint32_t y = 0; y < h; ++y) {
            jpeg_read_scanlines(&cinfo, &rowptr, 1);
            uint16_t *drow = dst + static_cast<size_t>(y) * dstWidth;
            for (uint32_t x = 0; x < w; ++x) {
                drow[x] = rgb888To565(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
            }
        }
        ok = true;
    } else {
        // 尺寸不一致：先整体解码为 RGB888，再最近邻缩放
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        std::vector<uint8_t> row(w * 3);
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t *rowptr = row.data();
            jpeg_read_scanlines(&cinfo, &rowptr, 1);
            std::memcpy(rgb.data() + static_cast<size_t>(y) * w * 3, row.data(), row.size());
        }
        for (uint32_t dy = 0; dy < dstHeight; ++dy) {
            uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(dy) * h / dstHeight);
            if (sy >= h) sy = h - 1;
            uint16_t *drow = dst + static_cast<size_t>(dy) * dstWidth;
            const uint8_t *srow = rgb.data() + static_cast<size_t>(sy) * w * 3;
            for (uint32_t dx = 0; dx < dstWidth; ++dx) {
                uint32_t sx = static_cast<uint32_t>(static_cast<uint64_t>(dx) * w / dstWidth);
                if (sx >= w) sx = w - 1;
                drow[dx] = rgb888To565(srow[sx * 3], srow[sx * 3 + 1], srow[sx * 3 + 2]);
            }
        }
        ok = true;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return ok;
}

}  // namespace preview
