#pragma once

#include "lvgl.h"

namespace board {

/** 创建并初始化 i.MX6ULL 的 /dev/fb0 显示设备。失败返回 nullptr。 */
lv_display_t *createDisplay();

/** 解除 framebuffer 控制台的自动 blank 状态，供 UI 定时保活。 */
void keepDisplayAwake();

/** 将 LVGL 的完整后备画面重新写入 /dev/fb0。
 *  用于外部播放器直接占用 framebuffer 后恢复界面。 */
void restoreDisplayFromBackBuffer();

}  // namespace board
