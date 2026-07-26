#pragma once
#include "lvgl.h"
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
    int16_t        rest_pos;    /* 内部：bind 时快照的 aligned y/x */
} trans_actor_t;

typedef struct {
    trans_actor_t *actors;      /* 场景持有的静态数组（bind 后勿动） */
    int            actor_n;
    /* 时间锚点变形；NULL = 本场景时间就停在共识姿态。ms 为期望时长。 */
    void         (*clock_to_consensus)(scene_t *s, uint32_t ms);
    void         (*clock_from_consensus)(scene_t *s, uint32_t ms);
} trans_profile_t;

/* 场景 init() 末尾调用（对象已 align，rest 位置在此快照）。 */
void scene_trans_bind(const char *scene_id, trans_profile_t *profile);

/* 统一切换入口——替代所有 scene_fw_show 调用点。异步：出场约 280ms
 * 后瞬切，再入场约 600ms。转场中重复调用会覆盖目标（快速连按安全）。
 * motion_reduced 时退化为瞬切。 */
void scene_trans_switch(int target_idx);

bool scene_trans_busy(void);

#ifdef __cplusplus
}
#endif
