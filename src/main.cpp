#include "lvgl.h"

int main(void) {
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(1024, 600);
    if (!disp) {
        return 1;
    }
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1e1e2e), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Camera LVGL - build OK");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);

    while (1) {
        uint32_t delay = lv_timer_handler();
        if (delay > 100) delay = 100;
        if (delay < 1) delay = 1;
        lv_delay_ms(delay);
    }
    return 0;
}
