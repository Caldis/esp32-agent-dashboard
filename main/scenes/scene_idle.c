/*
 * scene_idle — default scene shown when no sessions are active.
 *
 * Soft breathing dot at the centre with a "zZz" label and a faint subtitle.
 * Sized to look gentle on a 466×466 round AMOLED. Cheap to render
 * (only re-styles two existing objects each tick) — fine for the default
 * always-on view.
 */

#include "scenes.h"

#include <stdio.h>

#include "lvgl.h"

#define ACCENT_HEX  0x6B7AA8       /* dusk indigo */
#define PERIOD_MS   2400u

typedef struct {
    lv_obj_t   *dot;
    lv_obj_t   *zzz;
    lv_obj_t   *sub;
    lv_timer_t *timer;
    uint32_t    t0_ms;
} idle_state_t;

static void idle_tick(lv_timer_t *t)
{
    idle_state_t *st = (idle_state_t *)lv_timer_get_user_data(t);
    if (!st) return;
    uint32_t now = lv_tick_get();
    /* Triangle-wave 0..1 over PERIOD_MS so we don't need sinf(). */
    uint32_t phase = (now - st->t0_ms) % PERIOD_MS;
    uint32_t half = PERIOD_MS / 2;
    uint32_t bright;        /* 0..255 ish */
    if (phase < half) {
        bright = (phase * 220) / half;
    } else {
        bright = ((PERIOD_MS - phase) * 220) / half;
    }
    /* Floor a bit so the dot never fully disappears. */
    if (bright < 60) bright = 60;

    lv_obj_set_style_bg_opa(st->dot, bright, 0);

    /* Zzz alpha follows but lags / inverts slightly for a subtle dual-cue. */
    lv_obj_set_style_text_opa(st->zzz, 80 + (bright / 3), 0);
}

static void idle_init(scene_t *s, lv_obj_t *parent)
{
    idle_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->t0_ms = lv_tick_get();

    /* Soft breathing dot, centered. */
    st->dot = lv_obj_create(parent);
    lv_obj_remove_style_all(st->dot);
    lv_obj_set_size(st->dot, 120, 120);
    lv_obj_center(st->dot);
    lv_obj_set_style_radius(st->dot, 60, 0);
    lv_obj_set_style_bg_color(st->dot, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_bg_opa(st->dot, LV_OPA_60, 0);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_CLICKABLE);

    /* "zZz" inside the dot. */
    st->zzz = lv_label_create(parent);
    lv_obj_set_style_text_font(st->zzz, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(st->zzz, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(st->zzz, 3, 0);
    lv_obj_set_style_text_opa(st->zzz, LV_OPA_90, 0);
    lv_label_set_text(st->zzz, "z Z z");
    lv_obj_center(st->zzz);

    /* Subtitle below the dot. */
    st->sub = lv_label_create(parent);
    lv_obj_set_style_text_font(st->sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->sub, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_text_opa(st->sub, LV_OPA_70, 0);
    lv_label_set_text(st->sub, "no sessions");
    lv_obj_align(st->sub, LV_ALIGN_CENTER, 0, 110);

    st->timer = lv_timer_create(idle_tick, 50, st);
    lv_timer_pause(st->timer);
    idle_tick(st->timer);
}

static void idle_on_show(scene_t *s)
{
    idle_state_t *st = (idle_state_t *)s->user_data;
    if (!st) return;
    st->t0_ms = lv_tick_get();
    if (st->timer) {
        lv_timer_resume(st->timer);
        idle_tick(st->timer);
    }
}

static void idle_on_hide(scene_t *s)
{
    idle_state_t *st = (idle_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_idle = {
    .id           = "idle",
    .display_name = "I. Idle",
    .accent       = LV_COLOR_MAKE(0x6B, 0x7A, 0xA8),
    .description  = "Default scene; breathing dot when no sessions are active.",
    .tags         = "agent,default",
    .init         = idle_init,
    .on_show      = idle_on_show,
    .on_hide      = idle_on_hide,
};
