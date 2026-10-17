#pragma once

#include "lvgl.h"

namespace ui {

/** 加载中文字体（FreeType）。失败时返回 nullptr。 */
lv_font_t *loadCjkFont(uint32_t size);

}  // namespace ui
