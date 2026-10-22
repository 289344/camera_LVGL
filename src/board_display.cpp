#include "board_display.h"

#ifdef CAMERA_LVGL_BOARD

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace board {

namespace {

uint32_t tickGetCb() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<uint32_t>(t.tv_sec * 1000U + t.tv_nsec / 1000000U);
}

constexpr int kHorRes = 1024;
constexpr int kVerRes = 600;
constexpr int kBytesPerPixel = 4;

struct FbContext {
    int fd = -1;
    uint8_t *map = nullptr;
    uint8_t *drawBuf = nullptr;
    uint32_t lineLength = 0;
    uint32_t mapSize = 0;
};

int gActiveFbFd = -1;
FbContext *gActiveContext = nullptr;

void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *color) {
    FbContext *ctx = static_cast<FbContext *>(lv_display_get_driver_data(disp));
    if (!ctx || !ctx->map) {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    const uint32_t srcStride = static_cast<uint32_t>(kHorRes) * kBytesPerPixel;
    const uint32_t rowSize = static_cast<uint32_t>(w) * kBytesPerPixel;

    uint8_t *src = color + static_cast<uint32_t>(area->y1) * srcStride +
                   static_cast<uint32_t>(area->x1) * kBytesPerPixel;
    uint8_t *dst = ctx->map + static_cast<uint32_t>(area->x1) * kBytesPerPixel +
                   static_cast<uint32_t>(area->y1) * ctx->lineLength;

    for (int32_t y = 0; y < h; ++y) {
        lv_memcpy(dst, src, rowSize);
        src += srcStride;
        dst += ctx->lineLength;
    }

    lv_display_flush_ready(disp);
}

void deleteCb(lv_event_t *e) {
    lv_display_t *disp = static_cast<lv_display_t *>(lv_event_get_target(e));
    FbContext *ctx = static_cast<FbContext *>(lv_display_get_driver_data(disp));
    if (!ctx) return;

    if (ctx->map) ::munmap(ctx->map, ctx->mapSize);
    if (ctx->fd >= 0) {
        if (gActiveFbFd == ctx->fd) gActiveFbFd = -1;
        if (gActiveContext == ctx) gActiveContext = nullptr;
        ::close(ctx->fd);
    }
    std::free(ctx->drawBuf);
    delete ctx;
    lv_display_set_driver_data(disp, nullptr);
}

}  // namespace

lv_display_t *createDisplay() {
    lv_tick_set_cb(tickGetCb);

    lv_display_t *disp = lv_display_create(kHorRes, kVerRes);
    if (!disp) return nullptr;

    int fd = ::open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        lv_display_delete(disp);
        return nullptr;
    }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if (::ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0 ||
        ::ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        ::close(fd);
        lv_display_delete(disp);
        return nullptr;
    }

    if (vinfo.xres < kHorRes || vinfo.yres < kVerRes || vinfo.bits_per_pixel != 32) {
        ::close(fd);
        lv_display_delete(disp);
        return nullptr;
    }

    uint8_t *map = static_cast<uint8_t *>(
        ::mmap(nullptr, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (map == MAP_FAILED) {
        ::close(fd);
        lv_display_delete(disp);
        return nullptr;
    }
    // 进程重启时 framebuffer 会保留上一进程的画面；先清屏，避免旧 UI 残留。
    std::memset(map, 0, finfo.smem_len);

    FbContext *ctx = new FbContext();
    ctx->fd = fd;
    ctx->map = map;
    ctx->lineLength = finfo.line_length;
    ctx->mapSize = finfo.smem_len;

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);

    const uint32_t bufSize = static_cast<uint32_t>(kHorRes) * kVerRes * kBytesPerPixel;
    uint8_t *drawBuf = static_cast<uint8_t *>(std::malloc(bufSize));
    if (!drawBuf) {
        ::munmap(map, finfo.smem_len);
        ::close(fd);
        delete ctx;
        lv_display_delete(disp);
        return nullptr;
    }
    ctx->drawBuf = drawBuf;

    lv_display_set_driver_data(disp, ctx);
    lv_display_set_flush_cb(disp, flushCb);
    lv_display_add_event_cb(disp, deleteCb, LV_EVENT_DELETE, nullptr);
    lv_display_set_buffers(disp, drawBuf, nullptr, bufSize, LV_DISPLAY_RENDER_MODE_DIRECT);
    gActiveFbFd = fd;
    gActiveContext = ctx;

    return disp;
}

void keepDisplayAwake() {
    if (gActiveFbFd < 0) return;
    // 板端 fbcon 默认 consoleblank=600，且该内核参数只读。
    // 周期性解除 blank，避免相册空闲或播放结束后 LCD 一直黑屏。
    ::ioctl(gActiveFbFd, FBIOBLANK, FB_BLANK_UNBLANK);
}

void restoreDisplayFromBackBuffer() {
    FbContext *ctx = gActiveContext;
    if (!ctx || !ctx->map || !ctx->drawBuf) return;

    // 外部播放器（如 GStreamer fbdevsink）会直接覆盖 /dev/fb0，
    // 但不会更新 LVGL 的 DIRECT 模式后备缓冲。这里一次性恢复完整界面，
    // 避免播放结束后只有状态变化的控件被局部重绘。
    const uint32_t rowSize = static_cast<uint32_t>(kHorRes) * kBytesPerPixel;
    for (int y = 0; y < kVerRes; ++y) {
        std::memcpy(ctx->map + static_cast<uint32_t>(y) * ctx->lineLength,
                    ctx->drawBuf + static_cast<uint32_t>(y) * rowSize,
                    rowSize);
    }
    keepDisplayAwake();
}

}  // namespace board

#else

namespace board {

lv_display_t *createDisplay() {
    return nullptr;
}

void keepDisplayAwake() {}

void restoreDisplayFromBackBuffer() {}

}  // namespace board

#endif
