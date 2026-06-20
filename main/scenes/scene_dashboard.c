/*
 * scene_dashboard — v2.9.0 AMBIENT-only.
 *
 * The device's resting/working screen when no agent is awaiting input. It shows
 * a calm "thinking" pulse + a status word + a footer (active count, tokens
 * today). That's it.
 *
 * History: earlier versions packed a per-tool/thought feed here, then a
 * tap-to-detail toggle, then auto-rotation between the pulse and the feed. The
 * auto-rotation flip-flopped with the pulse during normal work (a new tool
 * every few seconds vs the REVEAL window), so it was dropped — the device is a
 * one-way ambient mirror; the full tool/thought detail lives in the web dev
 * panel. The device-name line was also removed (meaningless on this screen).
 *
 * Tick cadence 500ms. Display lock held by LVGL timer dispatch.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#define SCREEN_W       466
#define SCREEN_H       466

#define HEADER_Y        56   /* big time, vertically a touch lower without the name line */
#define FOOTER_Y       420
#define FOOTER_CAP_Y   442

#define COL_BG         0x0B0A09
#define COL_TEXT       0xF3EEE2
#define COL_TEXT_DIM   0x8A807A
#define COL_TEAL       0x2BB3B1

typedef struct {
    lv_obj_t *time_lbl;          /* "HH:MM" 48pt */
    lv_obj_t *ambient_grp;       /* container: ring + dot + label */
    lv_obj_t *ambient_ring;
    lv_obj_t *ambient_dot;       /* breathing inner dot (animated) */
    lv_obj_t *ambient_lbl;       /* "thinking" / "N agents working" / "your turn" / "idle" */
    lv_obj_t *footer_left;       /* active count */
    lv_obj_t *footer_right;      /* tokens today */
    lv_obj_t *footer_caption_l;
    lv_obj_t *footer_caption_r;
    bool      breath_armed;
    lv_timer_t *timer;
} dash_t;

/* ── Formatting helpers ──────────────────────────────────────────── */

static void format_clock(char *buf, size_t cap, const agent_state_t *st)
{
    if (st->host_epoch_unix > 0) {
        uint32_t now = st->host_epoch_unix
                     + (lv_tick_get() - st->host_clock_received_ms) / 1000;
        int32_t tz_now = (int32_t)now + st->host_tz_offset_seconds;
        time_t tt = (time_t)tz_now;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(buf, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(buf, cap, "--:--");
    }
}

static void format_tokens(char *buf, size_t cap, uint64_t tok)
{
    if (tok < 1000) {
        snprintf(buf, cap, "%u", (unsigned)tok);
    } else if (tok < 100000) {
        snprintf(buf, cap, "%.1fk", (double)tok / 1000.0);
    } else {
        snprintf(buf, cap, "%uk", (unsigned)(tok / 1000));
    }
}

/* ── Breathing pulse animation ───────────────────────────────────── */

static void anim_breath_size(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
    lv_obj_center((lv_obj_t *)obj);
}

static void arm_breath(dash_t *d)
{
    if (d->breath_armed || !d->ambient_dot) return;
    d->breath_armed = 1;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d->ambient_dot);
    lv_anim_set_values(&a, 16, 34);
    lv_anim_set_time(&a, 1500);
    lv_anim_set_playback_time(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_breath_size);
    lv_anim_start(&a);
}

/* ── Tick ────────────────────────────────────────────────────────── */

static void tick(lv_timer_t *t)
{
    dash_t *d = (dash_t *)lv_timer_get_user_data(t);
    if (!d) return;

    agent_state_lock();
    agent_state_t *st = agent_state_get();

    char clock[16];
    format_clock(clock, sizeof(clock), st);
    lv_label_set_text(d->time_lbl, clock);

    int active_now = st->running + st->waiting;
    const char *verb = (st->running > 0) ? "thinking" :
                       (active_now > 0)  ? "your turn" : "idle";
    char amb[32];
    if (st->running > 1) snprintf(amb, sizeof(amb), "%d agents working", st->running);
    else                 snprintf(amb, sizeof(amb), "%s", verb);
    lv_label_set_text(d->ambient_lbl, amb);

    char left[24], tok_str[16];
    snprintf(left, sizeof(left), "%d", active_now);
    format_tokens(tok_str, sizeof(tok_str), st->tokens_today);
    lv_label_set_text(d->footer_left,  left);
    lv_label_set_text(d->footer_right, tok_str);

    agent_state_unlock();
}

/* ── Init / on_show / on_hide ────────────────────────────────────── */

static void init(scene_t *s, lv_obj_t *parent)
{
    s->container = parent;
    dash_t *d = lv_malloc(sizeof(dash_t));
    memset(d, 0, sizeof(dash_t));
    s->user_data = d;

    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(parent, lv_color_hex(pal ? pal->bg : COL_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* Header — big time (no device-name line) */
    d->time_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(d->time_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(d->time_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->time_lbl, "--:--");
    lv_obj_align(d->time_lbl, LV_ALIGN_TOP_MID, 0, HEADER_Y);

    /* AMBIENT group — calm thinking pulse (ring + breathing dot + status word) */
    d->ambient_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(d->ambient_grp);
    lv_obj_set_size(d->ambient_grp, SCREEN_W, 200);
    lv_obj_align(d->ambient_grp, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_clear_flag(d->ambient_grp, LV_OBJ_FLAG_SCROLLABLE);

    d->ambient_ring = lv_obj_create(d->ambient_grp);
    lv_obj_set_size(d->ambient_ring, 96, 96);
    lv_obj_align(d->ambient_ring, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_opa(d->ambient_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(d->ambient_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(d->ambient_ring, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_border_width(d->ambient_ring, 2, 0);
    lv_obj_set_style_border_opa(d->ambient_ring, LV_OPA_40, 0);
    lv_obj_clear_flag(d->ambient_ring, LV_OBJ_FLAG_SCROLLABLE);

    d->ambient_dot = lv_obj_create(d->ambient_ring);
    lv_obj_set_size(d->ambient_dot, 18, 18);
    lv_obj_center(d->ambient_dot);
    lv_obj_set_style_bg_color(d->ambient_dot, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_bg_opa(d->ambient_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d->ambient_dot, 0, 0);
    lv_obj_set_style_radius(d->ambient_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(d->ambient_dot, LV_OBJ_FLAG_SCROLLABLE);

    d->ambient_lbl = lv_label_create(d->ambient_grp);
    lv_obj_set_style_text_color(d->ambient_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(d->ambient_lbl, &lv_font_montserrat_28, 0);
    lv_label_set_text(d->ambient_lbl, "thinking");
    lv_obj_align(d->ambient_lbl, LV_ALIGN_TOP_MID, 0, 122);

    /* Footer — two big numbers + tiny captions */
    d->footer_left = lv_label_create(parent);
    lv_obj_set_style_text_color(d->footer_left, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_text_font(d->footer_left, &lv_font_montserrat_28, 0);
    lv_label_set_text(d->footer_left, "0");
    lv_obj_set_pos(d->footer_left, 124, FOOTER_Y - 12);

    d->footer_caption_l = lv_label_create(parent);
    lv_obj_set_style_text_color(d->footer_caption_l, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(d->footer_caption_l, &lv_font_montserrat_12, 0);
    lv_label_set_text(d->footer_caption_l, "active");
    lv_obj_set_pos(d->footer_caption_l, 124, FOOTER_CAP_Y);

    d->footer_right = lv_label_create(parent);
    lv_obj_set_style_text_color(d->footer_right, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(d->footer_right, &lv_font_montserrat_28, 0);
    lv_label_set_text(d->footer_right, "0");
    lv_obj_set_pos(d->footer_right, 284, FOOTER_Y - 12);

    d->footer_caption_r = lv_label_create(parent);
    lv_obj_set_style_text_color(d->footer_caption_r, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(d->footer_caption_r, &lv_font_montserrat_12, 0);
    lv_label_set_text(d->footer_caption_r, "tokens today");
    lv_obj_set_pos(d->footer_caption_r, 284, FOOTER_CAP_Y);

    arm_breath(d);

    d->timer = lv_timer_create(tick, 500, d);
    lv_timer_pause(d->timer);
}

static void on_show(scene_t *s)
{
    dash_t *d = (dash_t *)s->user_data;
    if (!d) return;
    if (d->timer) {
        lv_timer_resume(d->timer);
        tick(d->timer);
    }
}

static void on_hide(scene_t *s)
{
    dash_t *d = (dash_t *)s->user_data;
    if (d && d->timer) lv_timer_pause(d->timer);
}

scene_t scene_dashboard = {
    .id           = "dashboard",
    .display_name = "Dashboard",
    .accent       = LV_COLOR_MAKE(0x2B, 0xB3, 0xB1),
    .description  = "Ambient thinking pulse + active/token summary.",
    .tags         = "dashboard,ambient,home",
    .init         = init,
    .on_show      = on_show,
    .on_hide      = on_hide,
};
