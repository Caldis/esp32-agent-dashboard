#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * ui_motion — 动效框架层 (v6.8)。
 *
 * 这里放的不是"动画"，而是所有动画都要共享、且各自抄写就会出错的两样
 * 东西。它们都是从实际踩过的坑里长出来的：
 *
 * ── 1. 刷新档位（动态刷新率）──────────────────────────────────────
 * 设备静止时用低刷省电，有运动时才拉到高刷。v6.3 起 scene_trans 有一套，
 * v6.6 scene_flash 又抄了一套，两个主人共同改同一个 LVGL 刷新定时器，靠
 * `if (!scene_trans_busy())` 这种手工判断协调——再加第三个动画就必然打架
 * （谁最后结束谁说了算，而"最后"是时序问题）。
 *
 * 改成【引用计数】：任何要动的东西 acquire，结束时 release，最后一个
 * release 才落回低刷。谁都不需要知道别人是否还在跑。
 *
 *     ui_motion_hold();     // 开始动
 *     ...
 *     ui_motion_release();  // 结束
 *
 * 必须成对。计数不会降到 0 以下（防止某条路径多 release 一次就把设备锁在
 * 高刷上）。
 *
 * ── 2. 批量样式写入 ───────────────────────────────────────────────
 * 反复出现的模式：一次改很多对象的 opa，但不想让每个对象各自触发一次
 * 失效（N 次全屏级重绘），而是全部写完后手动失效一小块。
 * 实测收益很大（天气插画呼吸 115k->27k 脏像素；边框辉光 53.4ms->18.5ms
 * 每帧），但手写这个模式有个致命坑：中间任何一次 early return 都会让
 * lv_obj_enable_style_refresh(false) 永久留着，整个 UI 从此不再刷新。
 *
 * 所以封成一对，并且要求调用方把"要失效哪些区域"一起交出来——恢复和
 * 失效在同一个函数里完成，写不出只做一半的代码。
 *
 *     ui_motion_batch_begin();
 *     ...写一堆样式...
 *     ui_motion_batch_end(anchor_obj, areas, n);
 *
 * 只对【不带 LAYOUT/EXT_DRAW/LAYER 标志】的属性安全——opa 系列都满足。
 * 位置、尺寸、字体这类会改变布局的属性不能走这条路。
 */

/* ── 刷新档位 ─────────────────────────────────────────────────────── */

/* app_main 里调一次。之后静止档生效。 */
void ui_motion_init(uint32_t idle_period_ms);

/* 引用计数式的高刷请求。必须成对。 */
void ui_motion_hold(void);
void ui_motion_release(void);

/* 当前是否有人持有高刷（诊断/测试用）。 */
int  ui_motion_holders(void);

/* 静止档周期。改动立即生效（若此刻没人持有高刷）。`?refr` 用。 */
void     ui_motion_set_idle_period(uint32_t ms);
uint32_t ui_motion_get_idle_period(void);

/* ── 批量样式写入 ─────────────────────────────────────────────────── */

void ui_motion_batch_begin(void);
/* 恢复自动失效，并对 anchor 失效 areas 里的每一块。areas 为 NULL 时
 * 退化为失效 anchor 自身（等价于普通行为，但仍保证 refresh 被恢复）。 */
void ui_motion_batch_end(lv_obj_t *anchor, const lv_area_t *areas, int n);
