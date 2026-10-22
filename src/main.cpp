#include "lvgl.h"

#include "ui/ui.h"
#include "ui/fonts.h"
#include "app/app_controller.h"
#include "board_display.h"

static Ui gUi;
static AppController gController;
#ifdef CAMERA_LVGL_BOARD
static uint32_t gDisplayAwakeCounter = 0;
#endif

static void refreshUi(lv_display_t *disp) {
    // 临时切到 FULL 模式，确保 LVGL 真正从根对象开始重绘整个屏幕。
    // 不能直接复制 DIRECT 后备缓冲：首次启动或局部刷新后，后备缓冲中
    // 可能仍有未初始化区域，直接复制会把按钮区域覆盖成黑色。
    const lv_display_render_mode_t oldMode = lv_display_get_render_mode(disp);
    if (oldMode != LV_DISPLAY_RENDER_MODE_FULL) {
        lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_FULL);
    }
    gUi.refreshAll();
    lv_refr_now(disp);
    if (oldMode != LV_DISPLAY_RENDER_MODE_FULL) {
        lv_display_set_render_mode(disp, oldMode);
    }
}

static void uiTick(lv_timer_t *timer) {
    (void)timer;
    gController.tick();
#ifdef CAMERA_LVGL_BOARD
    // 每 5 秒解除一次内核控制台 blank，适配 consoleblank=600 的板端系统。
    if (++gDisplayAwakeCounter % 150 == 0) board::keepDisplayAwake();
#endif
}

int main(void) {
    lv_init();

#ifdef CAMERA_LVGL_BOARD
    lv_display_t *disp = board::createDisplay();
    if (!disp) return 1;
    board::keepDisplayAwake();
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
    if (touch) {
        lv_indev_set_display(touch, disp);
        // 该板 goodix-ts 的 ABS_MT 实际已按屏幕像素输出，范围为 0..1023 / 0..599。
        // 不要用内核 absinfo 里的 0..800/0..480，否则触摸点会被错误缩放。
        lv_evdev_set_calibration(touch, 0, 0, 1023, 599);
    }
#else
    lv_display_t *disp = lv_sdl_window_create(1024, 600);
    if (!disp) return 1;
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();
#endif

    // 中文字体：FreeType 渲染
    // 所选 CJK 字体（如 Droid Sans Fallback）可能不含拉丁/数字字形，
    // 因此把内置 Montserrat 作为回退字体，避免英文和数字显示为方框。
    lv_freetype_init(512);
    lv_font_t *cjkNormal = ui::loadCjkFont(16);
    lv_font_t *cjkLarge = ui::loadCjkFont(20);
    if (cjkNormal) cjkNormal->fallback = &lv_font_montserrat_16;
    if (cjkLarge) cjkLarge->fallback = &lv_font_montserrat_20;

    const lv_font_t *themeFont = cjkNormal ? cjkNormal : LV_FONT_DEFAULT;
    const lv_font_t *titleFont = cjkLarge ? cjkLarge : LV_FONT_DEFAULT;
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                          lv_palette_main(LV_PALETTE_RED), true, themeFont);

    gUi.init(disp, titleFont);
    gController.init(&gUi);

    gUi.onCameraSelect = [](int i) { gController.selectCamera(i); };
    gUi.onStartPreview = []() { gController.startPreview(); };
    gUi.onStopPreview = []() { gController.stopPreview(); };
    gUi.onNetworkToggle = []() { gController.toggleNetworkPreview(); };
    gUi.onRecordToggle = []() { gController.toggleRecording(); };
    gUi.onAlbumOpen = []() { gController.openAlbum(); };
    gUi.onAlbumClose = []() { gController.closeAlbum(); };
    gUi.onTransfer = []() { gController.transferSelectedFile(); };
    gUi.onFileSelect = [](int row) { gController.selectFile(row); };
    gUi.onFileActivate = [](int) { gController.playSelectedFile(); };

    // 首帧强制完整绘制。自定义 fbdev 驱动不会在进程启动时自动清空整个画面。
    refreshUi(disp);

    lv_timer_create(uiTick, 33, nullptr);  // ~30fps 的 UI 刷新

    while (1) {
        const bool wasPlaying = gController.isPlayingVideo();
        gController.pollPlayback();
        if (wasPlaying && !gController.isPlayingVideo()) {
            // 外部播放器直接写 /dev/fb0，结束后强制完整重绘相机界面。
            refreshUi(disp);
        }
        if (gController.isPlayingVideo()) {
            // 播放器占用同一 framebuffer 时暂停 LVGL 刷新和触摸处理，避免画面互相覆盖。
            lv_delay_ms(30);
            continue;
        }

        uint32_t delay = lv_timer_handler();
        if (delay > 100) delay = 100;
        if (delay < 1) delay = 1;
        lv_delay_ms(delay);
    }
    return 0;
}
