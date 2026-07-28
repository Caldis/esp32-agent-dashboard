/*
 * scene_flash — 见 scene_flash.h。
 */

#include "scene_flash.h"
#include "theme.h"
#include "scene_trans.h"

#include "lvgl.h"

#define SCREEN_W      466
/* 面板圆角，实测调定（v6.7：72 -> 60）。挑大了四角会与黑边露缝，挑小了
 * 高光会切进内容区。 */
#define RING_RADIUS    60

/* ── 内阴影发光 ─────────────────────────────────────────────────────
 * LVGL 9 没有 inset shadow，所以用【同心环叠加】近似：N 个圆角矩形描边
 * 由屏幕边缘向内层层内缩，亮度逐层递减，合起来读作一圈由边缘向内渗的
 * 辉光。层窄而少，靠亮度梯度而不是模糊来造柔和感。
 *
 * 动画分两段，正好对应"淡入出现、扩散范围收缩的同时淡出"：
 *   fade in   —— 整体亮度 g 拉起，扩散范围 sp 保持满值（辉光点亮）
 *   fade out  —— g 回落的同时 sp 收缩，内层先熄，辉光向边缘退回
 * 只降 opa 会读成"灯泡变暗"；让扩散范围一起收，才读成"辉光收回去"。
 *
 * 渲染成本：每个环的包围盒都是整块屏，N 个环各自失效就是 N 次全屏重绘。
 * 所以所有环的 opa 改动都在 lv_obj_enable_style_refresh(false) 下批量
 * 写入，最后只手动失效边缘那四条窄带——代价与环的数量无关。 */
#define RING_N          4
#define RING_STEP       3      /* 每层内缩 px */
#define RING_WIDTH      3
/* 边缘最亮、向内递减。这条梯度就是"辉光"本身。 */
static const uint8_t RING_BASE[RING_N] = { 235, 160, 100, 55 };

/* 包络：(g, sp)，g=整体亮度，sp=扩散范围。
 * 4 步升 + 2 步保持 + 9 步"收+暗"，15 步 * 33ms ≈ 500ms。 */
#define STEP_MS        33
static const uint8_t ENV_G[]  = {  64, 128, 192, 255, 255, 255,
                                  235, 205, 175, 145, 115,  85,  55,  25, 0 };
static const uint8_t ENV_SP[] = { 255, 255, 255, 255, 255, 255,
                                  235, 210, 180, 150, 120,  90,  60,  30, 0 };
#define ENVELOPE_N  (sizeof(ENV_G) / sizeof(ENV_G[0]))

/* 高光期间也要高刷。静止档是 66ms/帧，而包络步进 33ms —— 不抬档的话步比
 * 帧还快，动画会被直接丢帧，实测就是"闪一下但不顺"。 */
#define FLASH_REFR_MS  16

/* 只失效边缘环带，而不是环的包围盒（整屏）。对 border_opa 关闭自动刷新
 * 是安全的：lv_obj_refresh_style 在关闭时只跳过 invalidate，而 opa 不带
 * LAYOUT/EXT_DRAW/LAYER 标志。实测：不批量 53.4ms/帧（19fps），批量后
 * 18.5ms/帧（53fps）。 */
#define BAND  (RING_N * RING_STEP + RING_WIDTH + 6)

static lv_obj_t   *s_ring[RING_N];
static lv_timer_t *s_timer;
static int         s_step;

static void scene_flash_refr(uint32_t ms)
{
    lv_display_t *d = lv_display_get_default();
    if (!d) return;
    lv_timer_t *t = lv_display_get_refr_timer(d);
    if (t) lv_timer_set_period(t, ms);
}

static void invalidate_ring_band(void)
{
    static const lv_area_t BANDS[4] = {
        { 0,             0,             SCREEN_W - 1, BAND - 1     },
        { 0,             SCREEN_W-BAND, SCREEN_W - 1, SCREEN_W - 1 },
        { 0,             0,             BAND - 1,     SCREEN_W - 1 },
        { SCREEN_W-BAND, 0,             SCREEN_W - 1, SCREEN_W - 1 },
    };
    for (int i = 0; i < 4; ++i) lv_obj_invalidate_area(s_ring[0], &BANDS[i]);
}

/* sp 决定还有几层亮着，由内向外熄灭。边界那一层用线性权重过渡，否则
 * 层的熄灭是可见的台阶。 */
static void glow_apply(uint8_t g, uint8_t sp)
{
    lv_obj_enable_style_refresh(false);
    for (int i = 0; i < RING_N; ++i) {
        int w = (int)sp * RING_N - i * 255;   /* 该层落在扩散范围内的比例 */
        if (w < 0)   w = 0;
        if (w > 255) w = 255;
        int v = (int)RING_BASE[i] * g / 255 * w / 255;
        lv_obj_set_style_border_opa(s_ring[i], (lv_opa_t)v, 0);
    }
    lv_obj_enable_style_refresh(true);
    invalidate_ring_band();
}

static void step_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_ring[0]) return;
    if (s_step >= (int)ENVELOPE_N) {
        for (int i = 0; i < RING_N; ++i)
            lv_obj_add_flag(s_ring[i], LV_OBJ_FLAG_HIDDEN);
        invalidate_ring_band();
        lv_timer_pause(s_timer);
        /* 交还低刷档。转场正在跑时不要抢——它自己会在结束时收回。 */
        if (!scene_trans_busy()) scene_flash_refr(scene_trans_get_idle_refr());
        return;
    }
    glow_apply(ENV_G[s_step], ENV_SP[s_step]);
    s_step++;
}

void scene_flash_init(void)
{
    if (s_ring[0]) return;

    for (int i = 0; i < RING_N; ++i) {
        int inset = i * RING_STEP;
        s_ring[i] = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_ring[i]);
        lv_obj_set_size(s_ring[i], SCREEN_W - 2 * inset, SCREEN_W - 2 * inset);
        lv_obj_center(s_ring[i]);
        /* 半径同步内缩，四角才保持同心，否则内层会显得更方。 */
        lv_obj_set_style_radius(s_ring[i], RING_RADIUS - inset, 0);
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

void scene_flash_ping(void)
{
    if (!s_ring[0] || !s_timer) return;

    const theme_palette_t *pal = theme_current();
    lv_color_t c = lv_color_hex(pal ? pal->accent_claude : 0x2BB3B1);
    for (int i = 0; i < RING_N; ++i) {
        lv_obj_set_style_border_color(s_ring[i], c, 0);
        lv_obj_clear_flag(s_ring[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 重复按键：从头开始，不叠加。上一轮还在收就直接接回峰值——连按的
     * 手感应该是"更亮"，不是"排队等前一次放完"。 */
    s_step = 0;
    scene_flash_refr(FLASH_REFR_MS);
    glow_apply(ENV_G[0], ENV_SP[0]);
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}
