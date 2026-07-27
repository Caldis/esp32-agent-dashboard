/*
 * perf_mon — 见 perf_mon.h。
 *
 * 所有事件回调都在 LVGL task 上串行触发，所以 per-cycle 的暂存变量不用
 * 保护；只有窗口累加量会被 console task 读走，那几个字段进临界区。
 */

#include "perf_mon.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "harness/console_protocol.h"
#include "scene_trans.h"
#include "scenes/scenes.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 窗口累加量（console task 会读） */
static struct {
    uint32_t refr_n;       /* 刷新周期数（含无重绘的空转） */
    uint32_t draw_n;       /* 真正渲染了的周期 = 真实帧 */
    uint32_t flush_n;      /* flush 次数（分块模式下每帧多次） */
    uint32_t overrun;      /* 单周期超过刷新周期 = 掉帧 */
    uint64_t refr_us, render_us, wait_us;
    uint32_t refr_max, render_max, wait_max;
    uint64_t inval_px;
    uint32_t inval_n;      /* 脏区个数 */
    uint32_t inval_max_px; /* 最大的单个脏矩形 */
} s_w;
static int64_t s_win_start_us;

/* per-cycle 暂存（仅 LVGL task 触碰） */
static int64_t  s_refr_t0, s_render_t0, s_wait_t0;
static uint32_t s_cyc_render_us, s_cyc_wait_us;
static bool     s_cyc_drew;

static uint32_t s_period_ms = 33;

static void win_reset_locked(void)
{
    s_w = (typeof(s_w)){0};
    s_win_start_us = esp_timer_get_time();
}

/* ── 事件钩子 ─────────────────────────────────────────────────────── */

static void on_disp_event(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        s_refr_t0 = now;
        s_cyc_render_us = 0;
        s_cyc_wait_us   = 0;
        s_cyc_drew      = false;
        break;

    case LV_EVENT_RENDER_START:
        s_render_t0 = now;
        s_cyc_drew  = true;
        break;
    case LV_EVENT_RENDER_READY:
        s_cyc_render_us += (uint32_t)(now - s_render_t0);
        break;

    case LV_EVENT_FLUSH_START:
        s_w.flush_n++;          /* 计数无需精确同步，容忍撕裂 */
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        s_wait_t0 = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        s_cyc_wait_us += (uint32_t)(now - s_wait_t0);
        break;

    case LV_EVENT_REFR_READY: {
        uint32_t total = (uint32_t)(now - s_refr_t0);
        portENTER_CRITICAL(&s_mux);
        s_w.refr_n++;
        /* 空转周期（没脏区）不计入帧率与耗时统计——否则 idle 时的 0us
         * 空转会把均值稀释成一个漂亮但无意义的数字。 */
        if (s_cyc_drew) {
            s_w.draw_n++;
            s_w.refr_us   += total;
            s_w.render_us += s_cyc_render_us;
            s_w.wait_us   += s_cyc_wait_us;
            if (total > s_w.refr_max)             s_w.refr_max   = total;
            if (s_cyc_render_us > s_w.render_max) s_w.render_max = s_cyc_render_us;
            if (s_cyc_wait_us > s_w.wait_max)     s_w.wait_max   = s_cyc_wait_us;
            if (total > s_period_ms * 1000u)      s_w.overrun++;
        }
        portEXIT_CRITICAL(&s_mux);
        break;
    }

    case LV_EVENT_INVALIDATE_AREA: {
        /* BSP 的 rounder 也挂在这个事件上并会改写 area；我们注册得更晚，
         * 读到的是对齐后的真实脏区。 */
        const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
        if (a) {
            uint32_t px = (uint32_t)lv_area_get_width(a) * lv_area_get_height(a);
            portENTER_CRITICAL(&s_mux);
            s_w.inval_px += px;
            s_w.inval_n++;
            if (px > s_w.inval_max_px) s_w.inval_max_px = px;
            portEXIT_CRITICAL(&s_mux);
        }
        break;
    }

    default:
        break;
    }
}

void perf_mon_set_period(uint32_t ms)
{
    if (ms) s_period_ms = ms;
}

/* ── ?perf ────────────────────────────────────────────────────────── */

static int cmd_perf(const console_args_t *args)
{
    (void)args;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    typeof(s_w) w = s_w;
    int64_t win_us = now - s_win_start_us;
    win_reset_locked();
    portEXIT_CRITICAL(&s_mux);

    if (win_us <= 0) win_us = 1;
    uint32_t dn = w.draw_n ? w.draw_n : 1;

    char buf[416];
    snprintf(buf, sizeof(buf),
             "{\"win_ms\":%" PRId64 ",\"period_ms\":%" PRIu32 ","
             /* frame_ms = 平均每帧间隔 = 真正的帧率倒数。别再报 window
              * 平均 fps：窗口里只要混进转场结束后的空闲段，帧数就被一段
              * 根本不该计入的时间除稀，读数会比实际慢一半（v6.3 的转场
              * 实测 refr_avg 21ms≈47fps，窗口 fps 却显示 15）。 */
             "\"frame_ms\":%.1f,\"fps_win\":%.1f,"
             "\"refr\":%" PRIu32 ",\"drawn\":%" PRIu32 ","
             "\"flush\":%" PRIu32 ",\"overrun\":%" PRIu32 ","
             "\"busy_pct\":%.1f,"
             "\"refr_avg_us\":%" PRIu32 ",\"refr_max_us\":%" PRIu32 ","
             "\"render_avg_us\":%" PRIu32 ",\"render_max_us\":%" PRIu32 ","
             "\"wait_avg_us\":%" PRIu32 ",\"wait_max_us\":%" PRIu32 ","
             "\"inval_px_per_frame\":%" PRIu32 ","
             "\"inval_n_per_frame\":%.1f,\"inval_max_px\":%" PRIu32 "}",
             win_us / 1000, s_period_ms,
             w.draw_n ? (float)w.refr_us / (float)w.draw_n / 1000.0f : 0.0f,
             (float)w.draw_n * 1000000.0f / (float)win_us,
             w.refr_n, w.draw_n, w.flush_n, w.overrun,
             (float)w.refr_us * 100.0f / (float)win_us,
             (uint32_t)(w.refr_us / dn), w.refr_max,
             (uint32_t)(w.render_us / dn), w.render_max,
             (uint32_t)(w.wait_us / dn), w.wait_max,
             (uint32_t)(w.inval_px / dn),
             (float)w.inval_n / (float)dn, w.inval_max_px);
    console_reply_ok("%s", buf);
    return 0;
}

static const console_cmd_t s_cmd_perf = { "?perf", cmd_perf,
    "JSON render perf for the window since the last ?perf" };

/* ?bake [0|1] — 精灵烘焙 A/B 开关。见 scene_trans.h。 */
static int cmd_bake(const console_args_t *args)
{
    if (args->argc >= 2) scene_trans_set_bake(args->argv[1][0] != '0');
    console_reply_ok("{\"bake\":%d}", scene_trans_get_bake() ? 1 : 0);
    return 0;
}

static const console_cmd_t s_cmd_bake = { "?bake", cmd_bake,
    "toggle transition sprite baking (A/B): ?bake 0|1" };

/* ?refr [ms] — 静止档刷新周期（动态刷新率的低刷那一档）。转场档固定
 * REFR_MS_ACTIVE，不开放：那一档的取向是"越快越好"，没有可调空间。 */
static int cmd_refr(const console_args_t *args)
{
    if (args->argc >= 2) scene_trans_set_idle_refr((uint32_t)atoi(args->argv[1]));
    console_reply_ok("{\"idle_refr_ms\":%" PRIu32 "}", scene_trans_get_idle_refr());
    return 0;
}

static const console_cmd_t s_cmd_refr = { "?refr", cmd_refr,
    "idle-tier refresh period ms: ?refr 66" };

/* ?wxbreath [0|1] — 天气插画呼吸波形开关。见 scenes.h。 */
static int cmd_wxbreath(const console_args_t *args)
{
    if (args->argc >= 2) scene_weather_set_breath(args->argv[1][0] != '0');
    console_reply_ok("{\"breath\":%d}", scene_weather_get_breath() ? 1 : 0);
    return 0;
}

static const console_cmd_t s_cmd_wxbreath = { "?wxbreath", cmd_wxbreath,
    "toggle the weather illustration accent breath (A/B): ?wxbreath 0|1" };

void perf_mon_init(lv_display_t *disp)
{
    if (!disp) return;

    /* LVGL 暴露不出刷新周期的 getter，直接读编译期常量——它就是
     * CONFIG_LV_DEF_REFR_PERIOD，也正是我们要抬的那个天花板。 */
    s_period_ms = LV_DEF_REFR_PERIOD;

    static const lv_event_code_t CODES[] = {
        LV_EVENT_REFR_START, LV_EVENT_REFR_READY,
        LV_EVENT_RENDER_START, LV_EVENT_RENDER_READY,
        LV_EVENT_FLUSH_START,
        LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH,
        LV_EVENT_INVALIDATE_AREA,
    };
    for (unsigned i = 0; i < sizeof(CODES) / sizeof(CODES[0]); ++i)
        lv_display_add_event_cb(disp, on_disp_event, CODES[i], NULL);

    portENTER_CRITICAL(&s_mux);
    win_reset_locked();
    portEXIT_CRITICAL(&s_mux);

    console_protocol_register(&s_cmd_perf);
    console_protocol_register(&s_cmd_bake);
    console_protocol_register(&s_cmd_refr);
    console_protocol_register(&s_cmd_wxbreath);
}
