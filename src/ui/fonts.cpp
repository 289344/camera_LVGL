#include "ui/fonts.h"

#include "fs_compat.h"

namespace ui {

namespace {
const char *kCandidates[] = {
    // 优先使用随工程打包的字体
    "assets/fonts/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
};
}  // namespace

lv_font_t *loadCjkFont(uint32_t size) {
    for (const char *path : kCandidates) {
        if (!fscompat::exists(path)) continue;
        lv_font_t *font = lv_freetype_font_create(
            path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, LV_FREETYPE_FONT_STYLE_NORMAL);
        if (font) return font;
    }
    return nullptr;
}

}  // namespace ui
