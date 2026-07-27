/*
 * scene_flash — 见 scene_flash.h。
 */

#include "scene_flash.h"
#include "theme.h"
#include "scene_trans.h"

#include "lvgl.h"

#define SCREEN_W      466
#define RING_WIDTH      7      /* 够醒目，又不侵占内容区 */
#define RING_PEAK     190      /* 峰值 border_opa（不到全白，免得像报错） */

/* 低频步进而不是逐帧动画。
 *
 * 这个环的包围盒是整块 466x466，LVGL 会按包围盒失效，于是每一步都要连带
 * 重画环下方那一圈里的场景内容——实测整屏级重绘在这块板子上约 30ms/帧
 * （见 CLAUDE.md 的"整屏合并"反例）。逐帧跑 16ms 周期只会掉帧，反而不
 * 流畅。
 *
 * 所以用 STEP_MS 的定时器步进一条预算好的包络：升 2 步、保持 2 步、落 5
 * 步，共 9 步 * 55ms ≈ 500ms。9 次重绘换一次高光，代价是可预期的，而且
 * 因为每步之间有 55ms，渲染器有充足时间画完——观感上是平滑的淡入淡出，
 * 不是台阶。这与天气插画呼吸波形用的是同一套取舍。 */
#define STEP_MS        33
static const uint8_t RING_ENVELOPE[] = {
    /* 升 3 步、保持 2 步、落 10 步 —— 快起慢落，读起来像"点亮后余韵"，
     * 而不是对称的呼吸。15 步 * 33ms ≈ 500ms。 */
     RING_PEAK / 3,        (RING_PEAK * 2) / 3,   RING_PEAK,
     RING_PEAK,            RING_PEAK,
    (RING_PEAK * 9) / 10, (RING_PEAK * 8) / 10,  (RING_PEAK * 7) / 10,
    (RING_PEAK * 6) / 10, (RING_PEAK * 5) / 10,  (RING_PEAK * 4) / 10,
    (RING_PEAK * 3) / 10, (RING_PEAK * 2) / 10,  (RING_PEAK * 1) / 10,
     0,
};
#define ENVELOPE_N  (sizeof(RING_ENVELOPE) / sizeof(RING_ENVELOPE[0]))

static lv_obj_t   *s_ring;
static lv_timer_t *s_timer;
static int         s_step;

/* 只失效环带本身，而不是环的包围盒。
 *
 * 环的包围盒是整块 466x466，所以一次普通的 border_opa 改动会让 LVGL 重画
 * 屏幕上的一切——实测 53.4ms/帧、每次高光 overrun 7 次，正是用户要避免的
 * 不流畅。但环的墨迹只占最外面十几像素，中间那一大块根本没变。
 *
 * 于是关掉自动失效（lv_obj_enable_style_refresh(false)，对 border_opa 是
 * 安全的：refresh 在关闭时只跳过 invalidate，而 opa 不带 LAYOUT/EXT_DRAW/
 * LAYER 标志），改为手动失效上下左右四条窄带。脏区从 21.7 万像素降到约
 * 2.5 万——同一招在天气插画呼吸波形上已经验证过。 */
#define BAND  16   /* 环宽 7 + 抗锯齿与半径过渡的余量 */

static void invalidate_ring_band(void)
{
    static const lv_area_t BANDS[4] = {
        { 0,             0,             SCREEN_W - 1,  BAND - 1        },
        { 0,             SCREEN_W-BAND, SCREEN_W - 1,  SCREEN_W - 1    },
        { 0,             0,             BAND - 1,      SCREEN_W - 1    },
        { SCREEN_W-BAND, 0,             SCREEN_W - 1,  SCREEN_W - 1    },
    };
    for (int i = 0; i < 4; ++i) lv_obj_invalidate_area(s_ring, &BANDS[i]);
}

/* 高光期间也要高刷。静止档是 66ms/帧，而包络步进是 33ms —— 不抬档的话
 * 步比帧还快，动画会被直接丢帧，实测就是"闪一下但不顺"。转场已经有这套
 * 分档机制，高光是同一类需求：短暂的运动窗口需要运力。 */
#define FLASH_REFR_MS  16

static void scene_flash_refr(uint32_t ms)
{
    lv_display_t *d = lv_display_get_default();
    if (!d) return;
    lv_timer_t *t = lv_display_get_refr_timer(d);
    if (t) lv_timer_set_period(t, ms);
}

static void ring_set_opa(lv_opa_t v)
{
    lv_obj_enable_style_refresh(false);
    lv_obj_set_style_border_opa(s_ring, v, 0);
    lv_obj_enable_style_refresh(true);
    invalidate_ring_band();
}

static void step_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_ring) return;
    if (s_step >= (int)ENVELOPE_N) {
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        invalidate_ring_band();     /* 隐藏也要自己失效——同样被关掉了 */
        lv_timer_pause(s_timer);
        /* 交还低刷档。转场正在跑时不要抢——它自己会在结束时收回。 */
        if (!scene_trans_busy()) scene_flash_refr(scene_trans_get_idle_refr());
        return;
    }
    ring_set_opa(RING_ENVELOPE[s_step]);
    s_step++;
}

void scene_flash_init(void)
{
    if (s_ring) return;

    s_ring = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, SCREEN_W, SCREEN_W);
    lv_obj_center(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring, RING_WIDTH, 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_TRANSP, 0);
    /* 不可点击、不滚动：它只是一层视觉回应，绝不能吃掉触摸事件。 */
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(step_cb, STEP_MS, NULL);
    lv_timer_pause(s_timer);
}

void scene_flash_ping(void)
{
    if (!s_ring || !s_timer) return;

    /* 颜色跟随主题强调色，和设备其它反馈保持同一套语言。 */
    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_border_color(s_ring,
        lv_color_hex(pal ? pal->accent_claude : 0x2BB3B1), 0);

    /* 重复按键：从头开始，不叠加。上一轮如果还在落，直接接回峰值——
     * 连按的手感应该是"更亮"，不是"排队等前一次放完"。 */
    s_step = 0;
    scene_flash_refr(FLASH_REFR_MS);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    ring_set_opa(RING_ENVELOPE[0]);
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}
