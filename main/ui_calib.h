#pragma once

/*
 * ui_calib — 面板可见区原点 (a) 的标定尺。实现与用法见 ui_calib.c。
 *
 * `?vis 1` 画四个候选原点的方框（0/3/6/9），画出【完整一圈】的那个
 * 颜色就是 a；`?vis 0` 收起。量准后写进 ui_screen.h。
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_calib_init(void);        /* 注册 ?vis（app_main 的 register 阶段） */
void ui_calib_show(bool on);     /* 需在 LVGL task / 持显示锁时调用 */

#ifdef __cplusplus
}
#endif
