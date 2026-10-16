#include "lvgl.h"

#include "ui/ui.h"
#include "app/app_controller.h"

static Ui ui;
static AppController controller;

static void uiTick(lv_timer_t *timer) {
    (void)timer;
    controller.tick();
}

int main(void) {
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(1024, 600);
    if (!disp) return 1;
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();

    ui.init(disp);
    controller.init(&ui);

    ui.onCameraSelect = [](int i) { controller.selectCamera(i); };
    ui.onStartPreview = []() { controller.startPreview(); };
    ui.onStopPreview = []() { controller.stopPreview(); };
    ui.onRecordToggle = []() { controller.toggleRecording(); };
    ui.onTransfer = []() { controller.transferSelectedFile(); };
    ui.onFileSelect = [](int row) { controller.selectFile(row); };

    lv_timer_create(uiTick, 33, nullptr);  // ~30fps 的 UI 刷新

    while (1) {
        uint32_t delay = lv_timer_handler();
        if (delay > 100) delay = 100;
        if (delay < 1) delay = 1;
        lv_delay_ms(delay);
    }
    return 0;
}
