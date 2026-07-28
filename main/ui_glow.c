/*
 * ui_glow — 见 ui_glow.h。（前身是 v6.6 的 scene_flash，抽成组件后新增
 * sustain 常驻模式，供掉线提示复用。）
 */

#include "ui_glow.h"
#include "ui_motion.h"
#include "ui_screen.h"
#include "theme.h"

#include "lvgl.h"

#define RING_N        4
#define RING_STEP     3      /* 每层内缩 px */
#define RING_WIDTH    3
/* 边缘最亮、向内递减。这条梯度就是"辉光"本身。 */
static const uint8_t RING_WEIGHT[RING_N] = { 255, 175, 110, 60 };

/* ping 的包络：(g, sp)，g=整体亮度比例，sp=扩散范围比例。
 * 3 步升 + 3 步保持 + 9 步"收+暗"，15 步 * 33ms ≈ 500ms。
 * 只降 g 会读成"灯泡变暗"；让 sp 一起收，才读成"辉光收回去"。 */
#define STEP_MS      33
static const uint8_t ENV_G[]  = {  85, 170, 255, 255, 255, 255,
                                  235, 205, 175, 145, 115,  85,  55,  25, 0 };
static const uint8_t ENV_SP[] = { 255, 255, 255, 255, 255, 255,
                                  235, 210, 180, 150, 120,  90,  60,  30, 0 };
#define ENV_N  (sizeof(ENV_G) / sizeof(ENV_G[0]))
/* sustain 淡入用 ping 包络的前几步，然后停住。 */
#define SUSTAIN_RISE_N  3

/* 带宽必须按【圆角】算，不能按直边算。直边处墨迹只深入
 * RING_N*RING_STEP + RING_WIDTH；但四个角是圆弧，所有层的弧心都落在距角
 * (UI_VIS_RADIUS, UI_VIS_RADIUS) 处，最内层墨迹半径只有 RING_INNER_R，
 * 于是它在 45° 方向距边 UI_VIS_RADIUS - RING_INNER_R/√2，比直边深得多。
 * 按直边算会让四个角永远刷不到，留下上一帧的残影——表现为一圈发光在拐角
 * 处断开（v6.7 实测过的缺陷）。 */
#define RING_INNER_R  (UI_VIS_RADIUS - (RING_N - 1) * RING_STEP - RING_WIDTH)
#define BAND          (UI_VIS_RADIUS - (RING_INNER_R * 707) / 1000 \
                       + 5 + UI_VIS_INSET)

static const lv_area_t BANDS[4] = {
    { 0,             0,             UI_VIS_W - 1, BAND - 1     },
    { 0,             UI_VIS_W-BAND, UI_VIS_W - 1, UI_VIS_W - 1 },
    { 0,             0,             BAND - 1,     UI_VIS_W - 1 },
    { UI_VIS_W-BAND, 0,             UI_VIS_W - 1, UI_VIS_W - 1 },
};

const ui_glow_style_t UI_GLOW_KEY     = { 0x000000, 190, 255 };  /* 色见 ping */
const ui_glow_style_t UI_GLOW_WAITING = { 0xE0A030, 150,  96 };
const ui_glow_style_t UI_GLOW_LOST    = { 0xE0503C, 150,  96 };

static lv_obj_t *s_ring[RING_N];
static lv_timer_t *s_timer;
static int   s_step;
static bool  s_holding;          /* 是否持有高刷档（必须成对） */

static bool  s_sustain_on;
static ui_glow_style_t s_sustain;
static ui_glow_style_t s_active;  /* 当前在播的样式 */

static void rings_color(uint32_t rgb)
{
    lv_color_t c = lv_color_hex(rgb);
    for (int i = 0; i < RING_N; ++i)
        lv_obj_set_style_border_color(s_ring[i], c, 0);
}

/* g/sp 为 0..255 的比例；亮度 = 层权重 × 样式峰值 × g，再按 sp 决定这层
 * 是否还在扩散范围内（由内向外熄灭，边界层线性过渡，否则熄灭是台阶）。 */
static void apply(const ui_glow_style_t *st, uint8_t g, uint8_t sp)
{
    uint32_t reach = (uint32_t)sp * st->spread / 255;
    ui_motion_batch_begin();
    for (int i = 0; i < RING_N; ++i) {
        int w = (int)reach * RING_N - i * 255;
        if (w < 0)   w = 0;
        if (w > 255) w = 255;
        int v = (int)RING_WEIGHT[i] * st->peak / 255 * g / 255 * w / 255;
        lv_obj_set_style_border_opa(s_ring[i], (lv_opa_t)v, 0);
    }
    ui_motion_batch_end(s_ring[0], BANDS, 4);
}

/* 直接失效边带。隐藏/清除时不能借 ui_motion_batch_end——它在没有
 * batch_begin 的情况下是空操作，那样辉光会留在屏上擦不掉。 */
static void invalidate_bands(void)
{
    for (int i = 0; i < 4; ++i) lv_obj_invalidate_area(s_ring[0], &BANDS[i]);
}

static void rings_show(bool on)
{
    for (int i = 0; i < RING_N; ++i) {
        if (on) lv_obj_clear_flag(s_ring[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_ring[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void drop_tier(void)
{
    if (s_holding) { ui_motion_release(); s_holding = false; }
}

/* 播完之后回落到 sustain 姿态（若有），否则收干净。 */
static void settle(void)
{
    lv_timer_pause(s_timer);
    drop_tier();
    if (s_sustain_on) {
        s_active = s_sustain;
        rings_color(s_sustain.color);
        apply(&s_sustain, 255, 255);
    } else {
        apply(&s_active, 0, 0);
        rings_show(false);
        invalidate_bands();
    }
}

static void step_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_ring[0]) return;
    if (s_step >= (int)ENV_N) { settle(); return; }
    apply(&s_active, ENV_G[s_step], ENV_SP[s_step]);
    s_step++;
}

/* sustain 的淡入：只走包络的前 SUSTAIN_RISE_N 步就停住。 */
static void sustain_step_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_ring[0]) return;
    if (s_step >= SUSTAIN_RISE_N) { settle(); return; }
    apply(&s_active, ENV_G[s_step], ENV_SP[s_step]);
    s_step++;
}

void ui_glow_init(void)
{
    if (s_ring[0]) return;
    for (int i = 0; i < RING_N; ++i) {
        int inset = i * RING_STEP;
        s_ring[i] = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_ring[i]);
        lv_obj_set_size(s_ring[i], UI_VIS_BOX - 2 * inset,
                                   UI_VIS_BOX - 2 * inset);
        /* 贴可见区而不是 LVGL 空间，并留内缩余量——见 ui_screen.h。 */
        lv_obj_set_pos(s_ring[i], UI_VIS_INSET + inset, UI_VIS_INSET + inset);
        /* 半径同步内缩，四角才保持同心，否则内层会显得更方。 */
        lv_obj_set_style_radius(s_ring[i], UI_VIS_RADIUS - inset, 0);
        lv_obj_set_style_bg_opa(s_ring[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_ring[i], RING_WIDTH, 0);
        lv_obj_set_style_border_opa(s_ring[i], LV_OPA_TRANSP, 0);
        /* 不可点击、不滚动：它只是一层视觉回应，绝不能吃掉触摸事件。 */
        lv_obj_clear_flag(s_ring[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_ring[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_ring[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_timer = lv_timer_create(step_cb, STEP_MS, NULL);
    lv_timer_pause(s_timer);
}

void ui_glow_ping(const ui_glow_style_t *s)
{
    if (!s_ring[0] || !s) return;
    s_active = *s;
    if (s_active.color == 0) {          /* UI_GLOW_KEY: 跟随主题强调色 */
        const theme_palette_t *pal = theme_current();
        s_active.color = pal ? pal->accent_claude : 0x2BB3B1;
    }
    rings_color(s_active.color);
    rings_show(true);
    /* 连按重启包络，但只持有一份档位——多 hold 一次就再也放不回低刷。 */
    if (!s_holding) { ui_motion_hold(); s_holding = true; }
    s_step = 0;
    lv_timer_set_cb(s_timer, step_cb);
    apply(&s_active, ENV_G[0], ENV_SP[0]);
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}

void ui_glow_sustain(const ui_glow_style_t *s)
{
    if (!s_ring[0] || !s) return;
    s_sustain    = *s;
    s_sustain_on = true;
    /* ping 正在播时不要打断它：它结束后 settle() 会自动落到 sustain。 */
    if (s_holding) return;
    s_active = s_sustain;
    rings_color(s_active.color);
    rings_show(true);
    ui_motion_hold(); s_holding = true;
    s_step = 0;
    lv_timer_set_cb(s_timer, sustain_step_cb);
    apply(&s_active, ENV_G[0], ENV_SP[0]);
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}

void ui_glow_sustain_clear(void)
{
    if (!s_ring[0] || !s_sustain_on) return;
    s_sustain_on = false;
    if (s_holding) return;              /* ping 在播，结束时自然收干净 */
    apply(&s_active, 0, 0);
    rings_show(false);
    invalidate_bands();
}
