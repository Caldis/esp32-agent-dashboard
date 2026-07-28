#pragma once

/*
 * ui_screen — 屏幕几何的唯一真相 (v6.9)。
 *
 * 这个文件存在，是因为"屏幕多宽"在这块板子上有三个不同的答案，而把它们
 * 混起来已经造成过一次真实缺陷（边框辉光右侧和底部被切）：
 *
 *   UI_LV_W    480  LVGL 的坐标空间（BSP_LCD_H_RES）。所有 LVGL 坐标、
 *                   lv_obj_center()、TOP_MID 对齐都以它为准。
 *   UI_VIS_W   466  物理面板真正点亮的范围。多出来的 14px 在屏外。
 *   UI_VIS_ORG   ?  可见区在 LVGL 空间里的起点。驱动的 x_gap/y_gap 从未
 *                   设置，MADCTL=0xA0 又做了镜像换轴，任何配置都没写明它。
 *                   实测夹逼出 a ∈ [1,6]（见 UI_VIS_INSET）。
 *
 * 后果：LV_ALIGN_TOP_MID 居中于 240，而可见中心是 233 + a —— 整个界面
 * 相对物理屏幕偏右约 7px。这是既有的全局约定（footer 的左右对称就是照
 * 它调的），不要顺手改，那是独立的一次改动。
 *
 * 贴边绘制（辉光、边框类）必须用 UI_VIS_* 而不是 UI_LV_W，并留出
 * UI_VIS_INSET 的安全余量。
 */

#define UI_LV_W        480   /* LVGL 坐标空间 */
#define UI_VIS_W       466   /* 物理可见范围 */

/* 面板原点偏移的上界。实测夹逼：
 *   辉光摆在 7..472 时右/下被切  -> a + 465 < 472 -> a < 7
 *   辉光摆在 0..465 时左上缺一点 -> a > 0
 * 取上界作为贴边绘制的内缩量，对 a 的整个可能区间都安全。
 * 若日后实测出确切的 a，把这里改成 a，贴边元素即可重新贴合物理边缘。 */
#define UI_VIS_INSET     6

/* 贴边元素的可用方框：(UI_VIS_INSET, UI_VIS_INSET) 起，边长 UI_VIS_BOX。 */
#define UI_VIS_BOX     (UI_VIS_W - 2 * UI_VIS_INSET)

/* 面板圆角（实测调定）。贴边元素的圆角应与它一致或按内缩量同步递减。 */
#define UI_VIS_RADIUS   60
