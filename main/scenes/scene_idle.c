/*
 * scene_idle — default scene when no agent activity is happening.
 *
 * v1 changes:
 *   • Sequential fade-in animation on the three "z" letters: each comes
 *     in 200 ms apart, full cycle ≈ 2 s, loops.
 *   • Subtitle reads "agent just stopped" when the most-recent agent's
 *     `last_active_unix` was within the last 30 s of host clock. Otherwise
 *     "no agents".
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "status_bar.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#define PERIOD_MS   2400u

typedef struct {
    status_bar_t sb;              /* shared top time + bottom active/tokens */
    lv_obj_t   *dot;
    lv_obj_t   *zzz_a;
    lv_obj_t   *zzz_b;
    lv_obj_t   *zzz_c;
    lv_obj_t   *sub;
    lv_timer_t *timer;
    uint32_t    t0_ms;
    int         last_sub_state;   /* 0 = "no agents", 1 = "just stopped" */
} idle_state_t;

static lv_opa_t letter_opa(uint32_t phase_ms, int letter_idx)
{
    /* Each letter is offset by 200 ms; full cycle 2000 ms; each letter
     * is "on" for the latter 60% of its slot. */
    uint32_t local = (phase_ms + (uint32_t)letter_idx * 200u) % PERIOD_MS;
    if (local < PERIOD_MS * 30 / 100) {
        /* fade-in */
        return (lv_opa_t)((local * 255u) / (PERIOD_MS * 30 / 100));
    } else if (local < PERIOD_MS * 70 / 100) {
        return 255;
    } else {
        uint32_t remaining = PERIOD_MS - local;
        uint32_t span = PERIOD_MS * 30 / 100;
        if (span == 0) return 0;
        return (lv_opa_t)((remaining * 255u) / span);
    }
}

static bool recently_stopped(uint32_t *out_seconds)
{
    /* Look at the most-recent last_active_unix across agents. If we have
     * host clock and (now - last_active) < 30s, return true. */
    uint32_t epoch = 0;
    uint32_t clk_received_ms = 0;
    uint32_t latest = 0;
    int slot_count = 0;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    epoch = s->host_epoch_unix;
    clk_received_ms = s->host_clock_received_ms;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!s->slots[i].in_use) continue;
        slot_count++;
        if (s->slots[i].last_active_unix > latest)
            latest = s->slots[i].last_active_unix;
    }
    /* fallback: even if a slot was pruned, the last_snapshot_ms tells us
     * something happened "now-ish". We treat any snapshot inside 30s as
     * "active". */
    uint32_t last_snap = s->last_snapshot_ms;
    bool ever = s->ever_received;
    agent_state_unlock();

    if (slot_count > 0) return false;   /* not idle — has agents */

    if (epoch && latest) {
        uint32_t now_epoch = epoch + (lv_tick_get() - clk_received_ms) / 1000u;
        if (now_epoch >= latest && now_epoch - latest < 30) {
            if (out_seconds) *out_seconds = now_epoch - latest;
            return true;
        }
    }
    if (ever) {
        uint32_t age = (lv_tick_get() - last_snap) / 1000u;
        if (age < 30) {
            if (out_seconds) *out_seconds = age;
            return true;
        }
    }
    return false;
}

static void idle_tick(lv_timer_t *t)
{
    idle_state_t *st = (idle_state_t *)lv_timer_get_user_data(t);
    if (!st) return;
    agent_state_lock();
    status_bar_update(&st->sb, agent_state_get());
    agent_state_unlock();
    uint32_t now = lv_tick_get();
    uint32_t phase = now - st->t0_ms;

    const theme_palette_t *pal = theme_current();

    /* Breathe the dot with a triangle wave for a soft pulse. */
    uint32_t p = phase % PERIOD_MS;
    uint32_t half = PERIOD_MS / 2;
    uint32_t bright = (p < half) ? (p * 200) / half
                                 : ((PERIOD_MS - p) * 200) / half;
    if (bright < 50) bright = 50;
    lv_obj_set_style_bg_opa(st->dot, bright, 0);
    lv_obj_set_style_bg_color(st->dot, lv_color_hex(pal->text_dim), 0);

    lv_obj_set_style_text_opa(st->zzz_a, letter_opa(phase, 0), 0);
    lv_obj_set_style_text_opa(st->zzz_b, letter_opa(phase, 1), 0);
    lv_obj_set_style_text_opa(st->zzz_c, letter_opa(phase, 2), 0);
    lv_obj_set_style_text_color(st->zzz_a, lv_color_hex(pal->text), 0);
    lv_obj_set_style_text_color(st->zzz_b, lv_color_hex(pal->text), 0);
    lv_obj_set_style_text_color(st->zzz_c, lv_color_hex(pal->text), 0);

    /* Subtitle */
    int desired = recently_stopped(NULL) ? 1 : 0;
    if (desired != st->last_sub_state) {
        lv_label_set_text(st->sub, desired ? "agent just stopped" : "no agents");
        st->last_sub_state = desired;
    }
    lv_obj_set_style_text_color(st->sub, lv_color_hex(pal->text_dim), 0);
}

static void idle_init(scene_t *s, lv_obj_t *parent)
{
    idle_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->t0_ms = lv_tick_get();
    st->last_sub_state = -1;

    status_bar_create(parent, &st->sb);

    /* Soft dot */
    st->dot = lv_obj_create(parent);
    lv_obj_remove_style_all(st->dot);
    lv_obj_set_size(st->dot, 140, 140);
    lv_obj_center(st->dot);
    lv_obj_set_style_radius(st->dot, 70, 0);
    lv_obj_set_style_bg_color(st->dot, lv_color_hex(theme_current()->text_dim), 0);
    lv_obj_set_style_bg_opa(st->dot, LV_OPA_30, 0);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_CLICKABLE);

    /* Three independent z letters so we can opa them separately. */
    st->zzz_a = lv_label_create(parent);
    st->zzz_b = lv_label_create(parent);
    st->zzz_c = lv_label_create(parent);
    lv_obj_set_style_text_font(st->zzz_a, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_font(st->zzz_b, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_font(st->zzz_c, &lv_font_montserrat_22, 0);
    lv_label_set_text(st->zzz_a, "z");
    lv_label_set_text(st->zzz_b, "Z");
    lv_label_set_text(st->zzz_c, "z");
    lv_obj_align(st->zzz_a, LV_ALIGN_CENTER, -22, 0);
    lv_obj_align(st->zzz_b, LV_ALIGN_CENTER,   0, 0);
    lv_obj_align(st->zzz_c, LV_ALIGN_CENTER,  22, 0);

    /* Subtitle */
    st->sub = lv_label_create(parent);
    lv_obj_set_style_text_font(st->sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(st->sub, LV_OPA_70, 0);
    lv_label_set_text(st->sub, "no agents");
    lv_obj_align(st->sub, LV_ALIGN_CENTER, 0, 120);

    st->timer = lv_timer_create(idle_tick, 60, st);
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
    .display_name = "Idle",
    .accent       = LV_COLOR_MAKE(0x6B, 0x6F, 0x7A),
    .description  = "Default scene; sequential zZz fade when no agents are active.",
    .tags         = "agent,default,idle",
    .init         = idle_init,
    .on_show      = idle_on_show,
    .on_hide      = idle_on_hide,
};
