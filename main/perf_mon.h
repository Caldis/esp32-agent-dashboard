#pragma once
#include "lvgl.h"

/*
 * perf_mon — 真实渲染性能计量 (v6.3)。
 *
 * 为什么需要它：`?stat` 里的 "fps" 是一个 33ms 自建 lv_timer 数自己的
 * 结果，按构造恒等于 ~30，跟屏幕真实重绘速率无关——它只是"LVGL 任务还
 * 活着"的活性信号。拿它做帧率优化的指标等于蒙眼调参。
 *
 * 这里挂的是 LVGL 的显示事件，量的是真东西：
 *   REFR_START/READY        一个完整刷新周期的墙钟时长
 *   RENDER_START/READY      软件渲染（画到 draw buffer）耗时
 *   FLUSH_WAIT_START/FINISH 阻塞等 DMA 把 buffer 推上屏的耗时
 *   INVALIDATE_AREA         每帧被弄脏的像素数（哪个动画最费屏）
 *
 * 关键区分：render 慢 = CPU/绘制瓶颈（字体、复杂图元、draw unit 数），
 * flush-wait 慢 = 总线瓶颈（QSPI 时钟、buffer 大小、DMA 源在 PSRAM）。
 * 两者的解药完全不同，混在一个 fps 数字里就分不出来。
 *
 * 统计窗口：每次 `?perf` 读取后清零，所以典型用法是
 *   ?perf            → 丢弃旧窗口
 *   （触发要测的动画）
 *   ?perf            → 这一段的真实数据
 */

void perf_mon_init(lv_display_t *disp);

/* overrun 的判定阈值 = 当前刷新周期。动态刷新率（scene_trans 的档位切换）
 * 一改周期就要同步过来，否则静止档的 66ms 会被按 16ms 判成满屏掉帧。 */
void perf_mon_set_period(uint32_t ms);
