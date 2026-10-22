#include "lvgl.h"

#include "ui/ui.h"
#include "ui/fonts.h"
#include "app/app_controller.h"
#include "board_display.h"

static Ui gUi;
static AppController gController;

static void uiTick(lv_timer_t *timer) {
    (void)timer;
    gController.tick();
}

int main(void) {
    lv_init();

#ifdef CAMERA_LVGL_BOARD
    lv_display_t *disp = board::createDisplay();
    if (!disp) return 1;
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
    gUi.onRecordToggle = []() { gController.toggleRecording(); };
    gUi.onAlbumOpen = []() { gController.openAlbum(); };
    gUi.onAlbumClose = []() { gController.closeAlbum(); };
    gUi.onTransfer = []() { gController.transferSelectedFile(); };
    gUi.onFileSelect = [](int row) { gController.selectFile(row); };
    gUi.onFileActivate = [](int) { gController.playSelectedFile(); };

    // 首帧强制完整绘制。自定义 fbdev 驱动不会在进程启动时自动清空整个画面。
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);

    lv_timer_create(uiTick, 33, nullptr);  // ~30fps 的 UI 刷新

    while (1) {
        const bool wasPlaying = gController.isPlayingVideo();
        gController.pollPlayback();
        if (wasPlaying && !gController.isPlayingVideo()) {
            // 外部播放器直接写 /dev/fb0，结束后强制完整重绘相机界面。
            lv_obj_invalidate(lv_screen_active());
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
