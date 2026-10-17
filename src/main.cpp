#include "lvgl.h"

#include "ui/ui.h"
#include "ui/fonts.h"
#include "app/app_controller.h"

static Ui gUi;
static AppController gController;

static void uiTick(lv_timer_t *timer) {
    (void)timer;
    gController.tick();
}

int main(void) {
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(1024, 600);
    if (!disp) return 1;
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();

    // 中文字体：FreeType 渲染
    lv_freetype_init(512);
    const lv_font_t *cjkNormal = ui::loadCjkFont(16);
    const lv_font_t *cjkLarge = ui::loadCjkFont(20);
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                          lv_palette_main(LV_PALETTE_RED), true,
                          cjkNormal ? cjkNormal : LV_FONT_DEFAULT);

    gUi.init(disp, cjkLarge ? cjkLarge : LV_FONT_DEFAULT);
    gController.init(&gUi);

    gUi.onCameraSelect = [](int i) { gController.selectCamera(i); };
    gUi.onStartPreview = []() { gController.startPreview(); };
    gUi.onStopPreview = []() { gController.stopPreview(); };
    gUi.onRecordToggle = []() { gController.toggleRecording(); };
    gUi.onTransfer = []() { gController.transferSelectedFile(); };
    gUi.onFileSelect = [](int row) { gController.selectFile(row); };

    lv_timer_create(uiTick, 33, nullptr);  // ~30fps 的 UI 刷新

    while (1) {
        uint32_t delay = lv_timer_handler();
        if (delay > 100) delay = 100;
        if (delay < 1) delay = 1;
        lv_delay_ms(delay);
    }
    return 0;
}
