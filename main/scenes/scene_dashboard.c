/*
 * scene_dashboard — ambient "thinking" pulse + shared status bar.
 *
 * The resting/working screen when no agent is awaiting. Center: a calm breathing
 * pulse + status word (thinking / N working / your turn / idle). Top time and
 * bottom active/tokens come from the shared status_bar so every scene's header/
 * footer is identical. Tool/thought detail lives in the web dev panel.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "status_bar.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define SCREEN_W   466
#define COL_BG     0x0B0A09
#define COL_TEXT   0xF3EEE2
#define COL_TEAL   0x2BB3B1

typedef struct {
    status_bar_t sb;             /* shared top time + bottom active/tokens */
    lv_obj_t *ambient_grp;
    lv_obj_t *ambient_ring;
    lv_obj_t *ambient_dot;       /* breathing inner dot */
    lv_obj_t *ambient_lbl;       /* status word */
    bool      breath_armed;
    lv_timer_t *timer;
} dash_t;

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

static void tick(lv_timer_t *t)
{
    dash_t *d = (dash_t *)lv_timer_get_user_data(t);
    if (!d) return;

    agent_state_lock();
    agent_state_t *st = agent_state_get();

    status_bar_update(&d->sb, st);

    int active_now = st->running + st->waiting;
    const char *verb = (st->running > 0) ? "thinking" :
                       (active_now > 0)  ? "your turn" : "idle";
    char amb[32];
    if (st->running > 1) snprintf(amb, sizeof(amb), "%d agents working", st->running);
    else                 snprintf(amb, sizeof(amb), "%s", verb);
    lv_label_set_text(d->ambient_lbl, amb);

    agent_state_unlock();
}

static void init(scene_t *s, lv_obj_t *parent)
{
    s->container = parent;
    dash_t *d = lv_malloc(sizeof(dash_t));
    memset(d, 0, sizeof(dash_t));
    s->user_data = d;

    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(parent, lv_color_hex(pal ? pal->bg : COL_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    status_bar_create(parent, &d->sb);

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
    .description  = "Ambient thinking pulse + status bar.",
    .tags         = "dashboard,ambient,home",
    .init         = init,
    .on_show      = on_show,
    .on_hide      = on_hide,
};
