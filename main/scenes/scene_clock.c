/*
 * scene_clock — StandBy-style big clock (v4).
 *
 * The third stop of the BOOT view cycle (dashboard → overview → clock).
 * iPhone-landscape-charging vibe: nothing on screen but a large
 * centered HH:MM plus the shared active/tokens footer. The status
 * bar's own top clock is hidden — the face IS the clock — while its
 * connection-health pill keeps working (a stale link must be visible
 * on every environment scene).
 *
 * Time source is status_bar_format_time(), the exact math the top
 * clock uses (host epoch + tick delta + tz), so the two can never
 * disagree; "--:--" until the host pushes `dash time`.
 *
 * The face is rendered by tiny_ttf at CLOCK_PX from the embedded
 * M PLUS Rounded 1c Black digit subset (clock_font() — rounded, heavy,
 * colon side bearings pre-tightened; the SF-Rounded look StandBy has).
 * Falls back to the SimHei subset, then Montserrat.
 *
 * Entrance (v4 M4, plan B): on every show the face starts at the top
 * small-clock position, fully transparent, and glides down to center
 * while fading in — apple_ease_out on both tracks. Plan A additionally
 * animated transform_scale (small→full size), but scaling a 150px
 * tiny_ttf label re-renders it through an intermediate layer every
 * frame and measured 12.5-13.4 fps against the panel's 30 (visible
 * stutter); y+fade keeps the "top clock becomes the big clock" story
 * at full frame rate. motion_reduced skips straight to the resting
 * pose.
 */

#include "scenes.h"
#include "agent_state.h"
#include "status_bar.h"
#include "cjk_font.h"
#include "anim/apple_ease.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define CLOCK_PX    135   /* was 150; pulled to ~90% on user feedback */
#define COL_TEXT    0xF3EEE2

/* Entrance geometry. The face label is CENTER-aligned, so y is an
 * offset from the vertical middle (233 on this 466px panel). The top
 * clock renders at TOP_MID y=56 with a 48pt font — its visual center
 * sits at ≈56+29=85, i.e. offset 85-233 = -148 from screen center. */
#define ENTRY_Y      (-148)
#define ENTRY_MS     550

typedef struct {
    status_bar_t sb;          /* footer + conn pill; top clock hidden */
    lv_obj_t   *face;         /* big centered HH:MM */
    lv_timer_t *timer;
    char        cached[16];   /* last rendered time string */
} clock_state_t;

static void anim_face_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/* text_opa, NOT style_opa: widget-level opa (like transform_scale)
 * composites the label through an intermediate layer every frame —
 * measured as bad as the scale plan (9-15 fps). text_opa is a plain
 * per-pixel alpha applied while blitting the glyphs; no layer. */
static void anim_face_opa(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* Play (or skip) the top-to-center entrance. Called from on_show on the
 * LVGL task; always resets to a deterministic start pose first so a
 * re-entry mid-animation can't compound. */
static void clock_entrance(clock_state_t *st, bool motion_ok)
{
    lv_anim_delete(st->face, anim_face_y);
    lv_anim_delete(st->face, anim_face_opa);

    if (!motion_ok) {
        lv_obj_set_y(st->face, 0);
        lv_obj_set_style_text_opa(st->face, LV_OPA_COVER, 0);
        return;
    }

    lv_obj_set_y(st->face, ENTRY_Y);
    lv_obj_set_style_text_opa(st->face, LV_OPA_TRANSP, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->face);
    lv_anim_set_time(&a, ENTRY_MS);
    lv_anim_set_path_cb(&a, apple_ease_out);

    lv_anim_set_values(&a, ENTRY_Y, 0);
    lv_anim_set_exec_cb(&a, anim_face_y);
    lv_anim_start(&a);

    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, anim_face_opa);
    lv_anim_start(&a);
}

static void clock_tick(lv_timer_t *t)
{
    clock_state_t *st = (clock_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    char buf[16];
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    status_bar_update(&st->sb, s);
    status_bar_format_time(buf, sizeof(buf), s);
    agent_state_unlock();

    /* Rewrite the 150px face only on minute change — every set_text
     * re-rasterises five big tiny_ttf glyphs. */
    if (strcmp(buf, st->cached) != 0) {
        snprintf(st->cached, sizeof(st->cached), "%s", buf);
        lv_label_set_text(st->face, buf);
    }
}

static void clock_init(scene_t *s, lv_obj_t *parent)
{
    clock_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    status_bar_create(parent, &st->sb);
    /* The big face replaces the 48pt top clock. status_bar_update keeps
     * set_text-ing the hidden label; harmless. */
    lv_obj_add_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);

    st->face = lv_label_create(parent);
    { const lv_font_t *bf = clock_font(CLOCK_PX);
      if (!bf) bf = cjk_font(CLOCK_PX);
      lv_obj_set_style_text_font(st->face, bf ? bf : &lv_font_montserrat_48, 0); }
    lv_obj_set_style_text_color(st->face, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(st->face, "--:--");
    lv_obj_align(st->face, LV_ALIGN_CENTER, 0, 0);

    st->timer = lv_timer_create(clock_tick, 1000, st);
    lv_timer_pause(st->timer);
    clock_tick(st->timer);
}

static void clock_on_show(scene_t *s)
{
    clock_state_t *st = (clock_state_t *)s->user_data;
    if (!st) return;

    bool motion_reduced;
    agent_state_lock();
    motion_reduced = agent_state_get()->motion_reduced;
    agent_state_unlock();
    clock_entrance(st, !motion_reduced);

    if (st->timer) {
        lv_timer_resume(st->timer);
        clock_tick(st->timer);
    }
}

static void clock_on_hide(scene_t *s)
{
    clock_state_t *st = (clock_state_t *)s->user_data;
    if (!st) return;
    /* Kill an in-flight entrance and park at the resting pose so the
     * scene-framework crossfade never snapshots a mid-flight face. */
    lv_anim_delete(st->face, anim_face_y);
    lv_anim_delete(st->face, anim_face_opa);
    lv_obj_set_y(st->face, 0);
    lv_obj_set_style_text_opa(st->face, LV_OPA_COVER, 0);
    if (st->timer) lv_timer_pause(st->timer);
}

scene_t scene_clock = {
    .id           = "clock",
    .display_name = "Clock",
    .accent       = LV_COLOR_MAKE(0xF3, 0xEE, 0xE2),
    .description  = "StandBy-style big centered clock with the shared "
                    "active/tokens footer.",
    .tags         = "clock,standby,time",
    .init         = clock_init,
    .on_show      = clock_on_show,
    .on_hide      = clock_on_hide,
};
