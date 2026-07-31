#pragma once
#include "lvgl.h"
#include <stddef.h>
#include "harness/scene_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * scene_trans — 全场景统一的转场协议 (v5.0)。
 *
 * 协议：出场 → 黑幕瞬切 → 入场。
 *
 *   出场  当前场景的"演员"以其入场方式的反向加速离屏（后进先出），
 *         屏幕归于纯黑背景。时间锚点例外（见下）。
 *   瞬切  scene_fw_show_instant() 在全黑帧上切换场景容器——黑对黑，
 *         全屏重绘不可见。这同时根治了旧 crossfade 的"扫描线"感：
 *         整容器 widget-opa 淡化每帧做两次全屏层合成，慢到能看见
 *         SPI 分块 flush 的推进线。本协议下永远只有元素级小动画。
 *   入场  新场景演员从屏幕外以真弹簧（spring_disp/spring_opa，
 *         欠阻尼谐振子）滑入 + 淡入，可错峰。
 *
 * ── 时间锚点（用户契约：时间永不离屏） ─────────────────────────────
 * 时间是唯一跨转场存续的元素。约定"共识姿态" = status_bar 顶部中央
 * 48px 小钟。出场时各场景负责把自己的时间实体归位到共识姿态
 * （clock_to_consensus 回调；普通场景时间本来就在共识位 → NULL）；
 * 瞬切前后两个场景的 time_lbl 同字体、同文本（status_bar_format_time
 * 同源）、同位置，切换帧时间纹丝不动；入场时新场景再把时间从共识
 * 姿态变形到自己的 rest 姿态（clock_from_consensus 回调——clock 场景
 * 的"小钟滑下长成大钟"、weather 的"滑去右上角"都挂在这）。
 *
 * ── 共享元素（v6.2：连续性层） ─────────────────────────────────────
 * 时间锚点是"跨转场存续的元素"的第一例，但它需要变形回调，因为两侧
 * 姿态不同。更常见的情况是两侧姿态**完全一样**（dashboard 与 clock 的
 * footer 就是同一个 status_bar 组件、同一套坐标）——这时元素没有任何
 * 理由飞出去再飞回来，它应该原地待着。
 *
 * 做法：给演员一个 key（跨场景的身份标识）。转场开始时框架为
 * (from, to) 这一对场景求交集：key 相同 **且姿态完全一致** 的演员，
 * 双方都跳过进/出场，只被瞬间钉在 rest 姿态上。黑幕瞬切帧上两个对象
 * 像素级重合，交接不可见——与时间锚点同一个原理。
 *
 * 姿态一致 = 同 dir/同 opa 通道/同 base_opa/同 align/两轴坐标全等，且
 * 两侧都不是 HIDDEN。key 是开发者对"这是同一个东西"的断言，姿态检查
 * 是机器可验证的护栏：断言错了也只会退化成正常进出场，不会错位。
 * 反例（必须正常进出场）：weather 砍掉了 footer 层级，它压根不声明
 * 这些 key → 交集为空 → dashboard 的 footer 照常飞出。
 *
 * 动态 rest 姿态：有些元素的静止位置随内容变（dashboard 的 ambient
 * 簇在 chip/无 chip 两个 pose 之间滑动，fleet 卡片的 y 随行数重排）。
 * profile 可挂 sync_rest 回调，框架在出场前/入场前各调一次，让场景把
 * 当前的静止姿态写回 actor->rest_pos。
 *
 * 未 bind 的场景出/入场为瞬时（零演员），行为退化为黑幕瞬切——
 * 迁移期语义，各场景逐个接入演员表。
 *
 * 线程契约：scene_trans_switch 必须在持有 display 锁或 LVGL task 上
 * 调用（与 scene_fw_show 相同）。内部推进由 LVGL timer 完成。
 */

/* 入场方向（出场 = 反向） */
typedef enum {
    TRANS_FROM_TOP = 0,
    TRANS_FROM_BOTTOM,
    TRANS_FROM_LEFT,
    TRANS_FROM_RIGHT,
    TRANS_FADE_ONLY,        /* 不位移，只淡入淡出 */
} trans_dir_t;

/* 透明度写入通道（对象类型决定） */
typedef enum {
    TROPA_TEXT = 0,         /* label: text_opa */
    TROPA_GROUP_TEXT,       /* 容器: 全部子对象的 text_opa */
    TROPA_BG,               /* 实心块: bg_opa */
    TROPA_BORDER,           /* 描边环: border_opa */
    TROPA_NONE,             /* 只位移不淡化 */
} trans_opa_ch_t;

typedef struct {
    lv_obj_t      *obj;
    trans_dir_t    dir;
    trans_opa_ch_t ch;
    uint8_t        base_opa;    /* rest 透明度（入场终点） */
    int16_t        out_dist;    /* 离屏距离 px（正数；符号由 dir 决定） */
    uint16_t       delay_ms;    /* 入场错峰延迟；出场自动反转（后进先出） */
    /* 跨场景身份。非 NULL 且对侧场景有同 key、同姿态的演员时，本演员
     * 在这次转场里"原地待命"（不出、不进）。用 TRANS_KEY_* 常量，别
     * 手写字面量——比较是字符串比较，拼错=静默退化。 */
    const char    *key;
    /* 1 = 转场期间烘焙成位图精灵（见 scene_trans.c 的 ghost_*）。给内容
     * 昂贵的演员开：抗锯齿线段、圆环、圆角卡片、大字。只对 TROPA_NONE
     * 的演员生效（纯位移；带淡化的演员留在常规路径上）。 */
    uint8_t        bake;
    int16_t        rest_pos;    /* 内部：bind 时快照的 aligned y/x（sync_rest 可改写） */
    uint8_t        held;        /* 内部：本次转场判定为共享 */
    lv_obj_t      *ghost;       /* 内部：烘焙出的替身 image */
    int16_t        ghost_dx;    /* 内部：替身 style 坐标相对本体的固定偏移 */
    int16_t        ghost_dy;    /* （快照含 ext_draw 边距，见 ghost_begin） */
    lv_draw_buf_t *ghost_buf;   /* 内部：替身的像素 */
} trans_actor_t;

typedef struct {
    trans_actor_t *actors;      /* 场景持有的静态数组（bind 后勿动） */
    int            actor_n;
    /* 时间锚点变形；NULL = 本场景时间就停在共识姿态。ms 为期望时长。 */
    void         (*clock_to_consensus)(scene_t *s, uint32_t ms);
    void         (*clock_from_consensus)(scene_t *s, uint32_t ms);
    /* 可选：把随内容变化的静止姿态写回 actor->rest_pos。出场前 / 入场前
     * 各调用一次（此时对象可能停在场外，别读屏幕坐标反推）。 */
    void         (*sync_rest)(scene_t *s);
    /* 可选：非演员的图层参与转场演出（v7.6）。
     * 演员协议的前提是"一个 lv_obj + 位移/淡化"，而自绘图层（ui_deco 的
     * 装饰层、将来的任何 DRAW_MAIN 画家）没有可动的对象，也不该被塞进
     * 演员表——它一个对象里装着几十个形状，位移它等于位移全屏。
     * 这两个钩子把"该出场了/该入场了"这个【时机】交给场景，具体怎么演
     * 由图层自己决定。ms = 期望时长（0 = 立即到位，motion_reduced）。 */
    void         (*on_outro)(scene_t *s, uint32_t ms);
    void         (*on_intro)(scene_t *s, uint32_t ms);
} trans_profile_t;

/* ── 共享元素 key 表 ────────────────────────────────────────────────
 * 一个 key = 一个"跨场景同一实体"。footer 四件套由 status_bar 组件统一
 * 生成演员（status_bar_trans_actors），姿态一致由构造保证。 */
#define TRANS_KEY_SB_ACTIVE_NUM  "sb.active.num"
#define TRANS_KEY_SB_ACTIVE_CAP  "sb.active.cap"
#define TRANS_KEY_SB_TOKEN_NUM   "sb.token.num"
#define TRANS_KEY_SB_TOKEN_CAP   "sb.token.cap"

/* 场景 init() 末尾调用（对象已 align，rest 位置在此快照）。 */
void scene_trans_bind(const char *scene_id, trans_profile_t *profile);

/* 统一切换入口——替代所有 scene_fw_show 调用点。异步：出场约 280ms
 * 后瞬切，再入场约 600ms。转场中重复调用会覆盖目标（快速连按安全）。
 * motion_reduced 时退化为瞬切。 */
void scene_trans_switch(int target_idx);

bool scene_trans_busy(void);

/* 设备【将要】停在哪个场景。转场是异步的——scene_fw_current_index() 在
 * 黑幕瞬切之前一直返回旧场景，所以任何“我现在在哪、于是下一步去哪”
 * 的判断都必须用这个，否则在转场窗口内连按会基于过时的现状做决策。
 * 空闲时等价于 scene_fw_current_index()。 */
int scene_trans_target(void);

/* 精灵烘焙总开关（默认开）。存在的理由是 A/B：单次转场的渲染耗时噪声
 * 能到 60%，靠反复烧录两个固件比不出可信差异。`?bake 0|1` 在同一块板子
 * 上来回切，跑够重复次数再下结论。 */
void scene_trans_set_bake(bool on);
bool scene_trans_get_bake(void);

/* 替身几何自检结果。转场结束换回本体时元素“跳一下”就是这里失配的
 * 症状；日志行会被串口会话轮换吃掉，所以做成可查询的。`?ghost`。 */
void scene_trans_ghost_stats(uint32_t *made, uint32_t *bad, char *last, size_t cap);

/* 动态刷新率：静止档周期（转场档固定为 REFR_MS_ACTIVE）。`?refr <ms>`。 */
void     scene_trans_set_idle_refr(uint32_t ms);
uint32_t scene_trans_get_idle_refr(void);

#ifdef __cplusplus
}
#endif
