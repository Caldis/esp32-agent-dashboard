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
 * SimHei subset (digits + ':' are in GB2312). SimHei digits are on the
 * thin side for true StandBy weight — an ExtraBold digits-only subset
 * is a planned enhancement, not a blocker (see SCENE_V4_DESIGN §5).
 *
 * Entrance (v4 M4): on every show the face starts where the top small
 * clock lives — its y offset and a transform_scale matching the 48pt
 * size — and glides to the screen center while scaling up to 1.0, both
 * tracks on apple_ease_out (iOS standard cubic-bezier). Visually the
 * top clock "becomes" the big clock instead of the face popping in.
 * motion_reduced skips straight to the resting pose.
 */

#include "scenes.h"
#include "agent_state.h"
#include "status_bar.h"
#include "cjk_font.h"
#include "anim/apple_ease.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define CLOCK_PX    150
#define COL_TEXT    0xF3EEE2

/* Entrance geometry. The face label is CENTER-aligned, so y is an
 * offset from the vertical middle (233 on this 466px panel). The top
 * clock renders at TOP_MID y=56 with a 48pt font — its visual center
 * sits at ≈56+29=85, i.e. offset 85-233 = -148 from screen center.
 * Start scale ≈ 256 * 48px/150px ≈ 82 so the face appears top-clock
 * sized before growing to full size. */
#define ENTRY_Y      (-148)
#define ENTRY_SCALE  82
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

static void anim_face_scale(void *obj, int32_t v)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)obj, v, 0);
}

/* Play (or skip) the top-to-center entrance. Called from on_show on the
 * LVGL task; always resets to a deterministic start pose first so a
 * re-entry mid-animation can't compound. */
static void clock_entrance(clock_state_t *st, bool motion_ok)
{
    lv_anim_delete(st->face, anim_face_y);
    lv_anim_delete(st->face, anim_face_scale);

    if (!motion_ok) {
        lv_obj_set_y(st->face, 0);
        lv_obj_set_style_transform_scale(st->face, 256, 0);
        return;
    }

    lv_obj_set_style_transform_pivot_x(st->face, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(st->face, lv_pct(50), 0);
    lv_obj_set_y(st->face, ENTRY_Y);
    lv_obj_set_style_transform_scale(st->face, ENTRY_SCALE, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->face);
    lv_anim_set_time(&a, ENTRY_MS);
    lv_anim_set_path_cb(&a, apple_ease_out);

    lv_anim_set_values(&a, ENTRY_Y, 0);
    lv_anim_set_exec_cb(&a, anim_face_y);
    lv_anim_start(&a);

    lv_anim_set_values(&a, ENTRY_SCALE, 256);
    lv_anim_set_exec_cb(&a, anim_face_scale);
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
    { const lv_font_t *bf = cjk_font(CLOCK_PX);
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
     * scene-framework crossfade never snapshots a shrunken face. */
    lv_anim_delete(st->face, anim_face_y);
    lv_anim_delete(st->face, anim_face_scale);
    lv_obj_set_y(st->face, 0);
    lv_obj_set_style_transform_scale(st->face, 256, 0);
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
