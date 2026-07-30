#pragma once

/*
 * ui_calib — 面板可见区几何的标定尺。实现与用法见 ui_calib.c。
 *
 * 三样东西全是"从来没被验证过的假设"，且都只有一双眼睛能读——帧缓冲
 * 里没有"面板点亮了哪些像素"这个信息（`?dump` 拿到的是整个 480×480
 * 缩放到 466，不是可见区裁剪）：
 *
 *   ?vis 1  原点 a    —— 顶/左各 4 条 1px 刻度，最外可见的那条即 a
 *   ?vis 2  延伸      —— 右/下各 4 条，最外可见的那条即最后一列/行
 *   ?vis 3  圆角半径  —— 四角各一个候选半径，贴合边缘的那个即真值
 *   ?vis 0  收起
 *
 * 量准后写进 ui_screen.h。换面板批次要复量。
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_calib_init(void);        /* 注册 ?vis（app_main 的 register 阶段） */
void ui_calib_show(int mode);    /* 需在 LVGL task / 持显示锁时调用 */

#ifdef __cplusplus
}
#endif
