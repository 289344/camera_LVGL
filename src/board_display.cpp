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

void delayCb(uint32_t ms) {
    // LV_OS_NONE 下 LVGL 默认忙等；播放时即使暂停刷新也会抢占单核 CPU，
    // 挤占解码和 ALSA 供数时间。Linux 上用可让出 CPU 的休眠。
    struct timespec remaining;
    remaining.tv_sec = ms / 1000U;
    remaining.tv_nsec = static_cast<long>(ms % 1000U) * 1000000L;
    while (::nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
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
    bool playbackActive = false;
};

FbContext *gActiveFb = nullptr;

void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *color) {
    FbContext *ctx = static_cast<FbContext *>(lv_display_get_driver_data(disp));
    if (!ctx || !ctx->map || ctx->playbackActive) {
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
        if (gActiveFb == ctx) gActiveFb = nullptr;
        ::close(ctx->fd);
    }
    std::free(ctx->drawBuf);
    delete ctx;
    lv_display_set_driver_data(disp, nullptr);
}

}  // namespace

lv_display_t *createDisplay() {
    lv_tick_set_cb(tickGetCb);
    lv_delay_set_cb(delayCb);

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
    gActiveFb = ctx;

    return disp;
}

void keepDisplayAwake() {
    if (!gActiveFb) return;
    // 板端 fbcon 默认 consoleblank=600，且该内核参数只读。
    // 周期性解除 blank，避免相册空闲或播放结束后 LCD 一直黑屏。
    ::ioctl(gActiveFb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
}

void setPlaybackActive(bool active) {
    if (!gActiveFb) return;
    gActiveFb->playbackActive = active;
    if (active) {
        // fbdevsink 会居中显示原尺寸画面；黑边只需清一次，不必逐帧补边。
        std::memset(gActiveFb->map, 0, gActiveFb->mapSize);
        keepDisplayAwake();
    }
}

}  // namespace board

#else

namespace board {

lv_display_t *createDisplay() {
    return nullptr;
}

void keepDisplayAwake() {}
void setPlaybackActive(bool) {}

}  // namespace board

#endif
