/*
 * scene_sessions — three counters (total / running / waiting) at the top
 * and a rolling list of the most-recent transcript entries below.
 *
 * Counters and entries come from agent_state. We re-read everything at
 * 5 Hz, skipping the LVGL work when nothing changed (poor-man's diff via
 * entry_seq + a per-field cache).
 */

#include "scenes.h"
#include "agent_state.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define ACCENT_HEX     0x4DD4FF       /* ice cyan */
#define DIM_HEX        0x365B66
#define ROW_FONT       (&lv_font_montserrat_14)
#define ROW_HEIGHT     24
#define ROW_PADDING_X  18

typedef struct {
    /* Header */
    lv_obj_t *roman;
    lv_obj_t *total;
    lv_obj_t *total_lbl;
    lv_obj_t *running;
    lv_obj_t *running_lbl;
    lv_obj_t *waiting;
    lv_obj_t *waiting_lbl;

    /* Body */
    lv_obj_t *rows[AGENT_ENTRY_COUNT];

    /* Caches */
    int       cached_total, cached_running, cached_waiting;
    uint32_t  cached_seq;

    lv_timer_t *timer;
} sessions_state_t;

static lv_obj_t *make_num(lv_obj_t *parent, int x_offset, uint32_t colour)
{
    lv_obj_t *n = lv_label_create(parent);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(colour), 0);
    lv_obj_set_style_text_opa(n, LV_OPA_COVER, 0);
    lv_label_set_text(n, "0");
    lv_obj_align(n, LV_ALIGN_CENTER, x_offset, -130);
    return n;
}

static lv_obj_t *make_caption(lv_obj_t *parent, int x_offset, const char *txt)
{
    lv_obj_t *c = lv_label_create(parent);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(DIM_HEX), 0);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_label_set_text(c, txt);
    lv_obj_align(c, LV_ALIGN_CENTER, x_offset, -100);
    return c;
}

static lv_obj_t *make_row(lv_obj_t *parent, int y_offset)
{
    lv_obj_t *r = lv_label_create(parent);
    lv_obj_set_style_text_font(r, ROW_FONT, 0);
    lv_obj_set_style_text_color(r, lv_color_white(), 0);
    lv_obj_set_style_text_opa(r, LV_OPA_70, 0);
    lv_obj_set_width(r, 380);
    lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
    lv_label_set_text(r, "");
    lv_obj_align(r, LV_ALIGN_CENTER, 0, y_offset);
    return r;
}

static uint32_t role_colour(const char *role)
{
    if (role == NULL) return 0xCCCCCC;
    if (strcmp(role, "user") == 0)       return 0x9EE493;  /* fresh mint */
    if (strcmp(role, "assistant") == 0)  return 0x8FD9FF;  /* sky */
    if (strcmp(role, "tool") == 0)       return 0xF0E0A8;  /* warm sand */
    return 0xCCCCCC;
}

static void sessions_tick(lv_timer_t *t)
{
    sessions_state_t *st = (sessions_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    /* Snapshot under lock. Copy only what we need so the lock is short. */
    int total, running, waiting;
    uint32_t seq;
    agent_entry_t snapshot[AGENT_ENTRY_COUNT];
    int snapshot_n;

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    total   = s->total;
    running = s->running;
    waiting = s->waiting;
    seq     = s->entry_seq;
    snapshot_n = s->entry_count;
    for (int i = 0; i < snapshot_n; ++i) {
        snapshot[i] = s->entries[i];
    }
    agent_state_unlock();

    if (total != st->cached_total) {
        char buf[16]; snprintf(buf, sizeof(buf), "%d", total);
        lv_label_set_text(st->total, buf);
        st->cached_total = total;
    }
    if (running != st->cached_running) {
        char buf[16]; snprintf(buf, sizeof(buf), "%d", running);
        lv_label_set_text(st->running, buf);
        st->cached_running = running;
    }
    if (waiting != st->cached_waiting) {
        char buf[16]; snprintf(buf, sizeof(buf), "%d", waiting);
        lv_label_set_text(st->waiting, buf);
        st->cached_waiting = waiting;
    }

    if (seq != st->cached_seq) {
        for (int i = 0; i < AGENT_ENTRY_COUNT; ++i) {
            lv_obj_t *row = st->rows[i];
            if (i < snapshot_n) {
                /* "[role] text" — fold into a single label for simplicity.
                 * Use precision specifiers to bound the format-truncation
                 * warning that fires on the unrestricted %s pair. */
                char buf[AGENT_ENTRY_TEXT_MAX + 24];
                snprintf(buf, sizeof(buf), "%.15s  %.95s",
                         snapshot[i].role[0] ? snapshot[i].role : "?",
                         snapshot[i].text);
                lv_label_set_text(row, buf);
                lv_obj_set_style_text_color(row,
                    lv_color_hex(role_colour(snapshot[i].role)), 0);
                /* Newer entries opaque; older fades a bit. */
                lv_opa_t opa = (lv_opa_t)(220 - (i * 30));
                if (opa < 80) opa = 80;
                lv_obj_set_style_text_opa(row, opa, 0);
            } else {
                lv_label_set_text(row, "");
            }
        }
        st->cached_seq = seq;
    }
}

static void sessions_init(scene_t *s, lv_obj_t *parent)
{
    sessions_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->cached_seq = (uint32_t)-1;
    st->cached_total = -1;
    st->cached_running = -1;
    st->cached_waiting = -1;

    /* Roman / title */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "SESSIONS");
    lv_obj_align(st->roman, LV_ALIGN_CENTER, 0, -170);

    /* Three numbers across the top. */
    st->total       = make_num(parent, -90, 0x4DD4FF);
    st->running     = make_num(parent,   0, 0x9EE493);
    st->waiting     = make_num(parent,  90, 0xF0E0A8);
    st->total_lbl   = make_caption(parent, -90, "TOTAL");
    st->running_lbl = make_caption(parent,   0, "RUN");
    st->waiting_lbl = make_caption(parent,  90, "WAIT");

    /* Five rows below, vertically stacked. */
    int y0 = -55;
    for (int i = 0; i < AGENT_ENTRY_COUNT; ++i) {
        st->rows[i] = make_row(parent, y0 + i * (ROW_HEIGHT + 4));
    }

    st->timer = lv_timer_create(sessions_tick, 200, st);
    lv_timer_pause(st->timer);
    sessions_tick(st->timer);
}

static void sessions_on_show(scene_t *s)
{
    sessions_state_t *st = (sessions_state_t *)s->user_data;
    if (st && st->timer) {
        /* Force a refresh by invalidating caches. */
        st->cached_seq = (uint32_t)-1;
        st->cached_total = -1;
        st->cached_running = -1;
        st->cached_waiting = -1;
        lv_timer_resume(st->timer);
        sessions_tick(st->timer);
    }
}

static void sessions_on_hide(scene_t *s)
{
    sessions_state_t *st = (sessions_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_sessions = {
    .id           = "sessions",
    .display_name = "II. Sessions",
    .accent       = LV_COLOR_MAKE(0x4D, 0xD4, 0xFF),
    .description  = "Active session counters and rolling transcript window.",
    .tags         = "agent,sessions",
    .init         = sessions_init,
    .on_show      = sessions_on_show,
    .on_hide      = sessions_on_hide,
};
