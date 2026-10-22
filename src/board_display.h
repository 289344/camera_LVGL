#pragma once

#include "lvgl.h"

namespace board {

/** 创建并初始化 i.MX6ULL 的 /dev/fb0 显示设备。失败返回 nullptr。 */
lv_display_t *createDisplay();

/** 解除 framebuffer 控制台的自动 blank 状态，供 UI 定时保活。 */
void keepDisplayAwake();

}  // namespace board
