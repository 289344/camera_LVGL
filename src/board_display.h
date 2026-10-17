#pragma once

#include "lvgl.h"

namespace board {

/** 创建并初始化 i.MX6ULL 的 /dev/fb0 显示设备。失败返回 nullptr。 */
lv_display_t *createDisplay();

}  // namespace board
