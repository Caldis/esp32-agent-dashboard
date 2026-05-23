/*
 * scene_dashboard — v2.5.0 AMBIENT feed view.
 *
 * Replaces the v0.1.x multi-card grid with a vertically-stacked
 * completion feed. The dashboard is the device's resting state when
 * NO agent is awaiting input (when one IS, scene_auto_switch_cb
 * pushes scene_awaiting instead — see esp32_agent_dashboard_main.c).
 *
 * Layout (466 × 466 round AMOLED, noir):
 *
 *   ┌──────────────────────────────┐
 *   │           HH:MM              │  ← time, 48pt
 *   │           Clawd              │  ← device, 14pt dim
 *   │                              │
 *   │  HH:MM  ok  cc sx  Edit  …   │  ← feed row 1, biggest (verb 26pt)
 *   │  HH:MM  ok  cx sx  Grep  …   │  ← feed row 2
 *   │  HH:MM  ··  cc sx  Read  …   │  ← feed row 3
 *   │  HH:MM  ok  cc sx  Bash  …   │  ← feed row 4
 *   │  HH:MM  ok  cx sx  Read  …   │  ← feed row 5
 *   │  HH:MM  ok  cc sx  Write …   │  ← feed row 6 (oldest)
 *   │                              │
 *   │   N active   N.Nk tokens     │  ← footer summary, 18pt
 *   └──────────────────────────────┘
 *
 * Aggregates every slot's entries[] into one global recent-events feed
 * sorted by monotonic_ms descending. Newest at top. Oldest pages off
 * the bottom when feed exceeds 6 rows.
 *
 * Tick cadence 500ms. Display lock held by LVGL timer dispatch.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "tool_icons.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#define SCREEN_W       466
#define SCREEN_H       466
#define FEED_ROWS_MAX  6

typedef struct {
    lv_obj_t *time_lbl;          /* "HH:MM" 48pt */
    lv_obj_t *device_lbl;        /* "Clawd" 14pt dim */
    /* v2.5.1: ONE label per row instead of 5-column layout.
     * Multi-column absolute positioning gave us visual text bleed when
     * any column's text exceeded its width. Single-label rows are
     * structurally overlap-proof: the label is a fixed rect with
     * LV_LABEL_LONG_DOT, text simply truncates inside. */
    lv_obj_t *row_label[FEED_ROWS_MAX];
    lv_obj_t *row_verb[FEED_ROWS_MAX];     /* the verb stays as a separate emphasis label */
    lv_obj_t *footer_left;       /* "N active" */
    lv_obj_t *footer_right;      /* "N.Nk tokens" */
    lv_obj_t *footer_caption_l;
    lv_obj_t *footer_caption_r;
    lv_timer_t *timer;
} dash_t;

/* Flat entry for aggregation. */
typedef struct {
    char     ts[AGENT_ENTRY_TIME_MAX];
    char     role[16];
    char     tool[AGENT_ENTRY_TOOL_MAX];
    char     text[AGENT_ENTRY_TEXT_MAX];
    char     kind_short[3];       /* "cc"/"cx"/"ag" */
    char     sid_short[8];        /* last 4 chars */
    uint32_t monotonic_ms;
} feed_entry_t;

static const char *short_kind_of(const char *kind)
{
    if (strcmp(kind, "claude-code") == 0) return "cc";
    if (strcmp(kind, "codex")       == 0) return "cx";
    return "ag";
}

static void short_sid_of(char out[8], const char *sid)
{
    size_t n = strnlen(sid, 32);
    const char *start = (n > 4) ? sid + (n - 4) : sid;
    size_t i = 0;
    for (; i < 4 && start[i]; ++i) out[i] = start[i];
    out[i] = '\0';
}

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

/* Aggregate all entries from all slots, sort by recency desc, keep top N.
 * Lock held by caller. */
static int collect_feed(feed_entry_t out[], int cap)
{
    agent_state_t *s = agent_state_get();
    int n = 0;
    for (int si = 0; si < AGENT_SLOT_MAX; ++si) {
        agent_slot_t *slot = &s->slots[si];
        if (!slot->in_use) continue;
        for (int ei = 0; ei < slot->entry_count && n < cap * 2; ++ei) {
            const agent_entry_t *e = &slot->entries[ei];
            feed_entry_t *fe = &out[n++];
            memcpy(fe->ts,   e->ts,   sizeof(fe->ts));
            memcpy(fe->role, e->role, sizeof(fe->role));
            memcpy(fe->tool, e->tool, sizeof(fe->tool));
            memcpy(fe->text, e->text, sizeof(fe->text));
            memcpy(fe->kind_short, short_kind_of(slot->kind), 3);
            short_sid_of(fe->sid_short, slot->session_id);
            fe->monotonic_ms = e->monotonic_ms;
        }
    }
    /* Insertion sort by monotonic_ms desc — n ≤ 20, trivial cost. */
    for (int i = 1; i < n; ++i) {
        feed_entry_t k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].monotonic_ms < k.monotonic_ms) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return (n > cap) ? cap : n;
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

/* ── Layout constants ────────────────────────────────────────────── */

#define HEADER_Y        38
#define DEVICE_Y       102

#define FEED_TOP_Y     150
#define FEED_ROW_H      36
#define FEED_PAD_X      48

#define FOOTER_Y       420
#define FOOTER_CAP_Y   442

/* v2.5.1: single-label-per-row. Two labels actually — one for the
 * verb (emphasised, bigger font) and one for the rest of the row
 * ("HH:MM  ok  cc sx   src/auth.py +8 -2"). Verb stands out, the
 * rest is mono-style condensed. Both have fixed widths + DOT mode so
 * overflow is bounded inside each label, can't bleed sideways. */
#define COL_VERB_X      52   /* verb left edge */
#define COL_VERB_W      90
#define COL_REST_X     156   /* "HH:MM ok chip target" */
#define COL_REST_W     268

/* Colors mirror palette.md */
#define COL_BG         0x0B0A09
#define COL_TEXT       0xF3EEE2
#define COL_TEXT_DIM   0x8A807A
#define COL_INK_MUTE   0x5A514A
#define COL_TEAL       0x2BB3B1
#define COL_MOSS_OK    0x588A5C
#define COL_GOLD_WARN  0xB89020

/* ── Tick ────────────────────────────────────────────────────────── */

static void tick(lv_timer_t *t)
{
    dash_t *d = (dash_t *)lv_timer_get_user_data(t);
    if (!d) return;

    agent_state_lock();
    agent_state_t *st = agent_state_get();

    /* Header */
    char clock[16];
    format_clock(clock, sizeof(clock), st);
    lv_label_set_text(d->time_lbl, clock);
    lv_label_set_text(d->device_lbl,
                      st->device_name[0] ? st->device_name : "Clawd");

    /* Feed */
    feed_entry_t feed[FEED_ROWS_MAX];
    int n = collect_feed(feed, FEED_ROWS_MAX);

    for (int i = 0; i < FEED_ROWS_MAX; ++i) {
        if (i < n) {
            feed_entry_t *fe = &feed[i];

            /* Verb (emphasised, bigger) — Edit / Bash / Grep / Read / ... */
            const char *verb = fe->tool[0] ? fe->tool :
                               (fe->role[0] ? fe->role : "·");
            lv_label_set_text(d->row_verb[i], verb);

            /* "HH:MM  ok  cc sx   target/text" in one label.
             * Single label = no possible column overflow into neighbours. */
            const char *ts_str = fe->ts[0] ? fe->ts : "··:··";
            const char *status_glyph;
            if (strcmp(fe->role, "tool") == 0)            status_glyph = "ok";
            else if (strcmp(fe->role, "assistant") == 0)  status_glyph = "..";
            else                                          status_glyph = "·";
            /* Explicit max-widths on each %s so gcc's format-truncation
             * analyzer can compute a tight upper bound (otherwise it
             * assumes 815 bytes from unbounded const char* pointers). */
            char rest[160];
            snprintf(rest, sizeof(rest),
                     "%.6s  %.3s  %.3s %.6s   %.80s",
                     ts_str, status_glyph,
                     fe->kind_short, fe->sid_short,
                     fe->text);
            lv_label_set_text(d->row_label[i], rest);

            lv_obj_clear_flag(d->row_verb[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(d->row_label[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d->row_verb[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(d->row_label[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* If no entries at all yet, show a calm "no activity yet" line. */
    if (n == 0) {
        lv_label_set_text(d->row_verb[0], "");
        lv_label_set_text(d->row_label[0], "no activity yet");
        lv_obj_set_style_text_color(d->row_label[0],
                                     lv_color_hex(COL_INK_MUTE), 0);
        lv_obj_clear_flag(d->row_label[0], LV_OBJ_FLAG_HIDDEN);
    }

    /* Footer */
    char left[24], right[24], tok_str[16];
    int active = st->running + st->waiting;
    snprintf(left,  sizeof(left),  "%d", active);
    format_tokens(tok_str, sizeof(tok_str), st->tokens_today);
    snprintf(right, sizeof(right), "%s", tok_str);
    lv_label_set_text(d->footer_left,  left);
    lv_label_set_text(d->footer_right, right);

    agent_state_unlock();
}

/* ── Init / on_show / on_hide ────────────────────────────────────── */

static lv_obj_t *make_row_label(lv_obj_t *parent, int x, int y,
                                  const lv_font_t *font, uint32_t color,
                                  int width, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, "");
    lv_obj_set_width(l, width);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, x, y);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
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

    /* Header — big time */
    d->time_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(d->time_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(d->time_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->time_lbl, "--:--");
    lv_obj_align(d->time_lbl, LV_ALIGN_TOP_MID, 0, HEADER_Y);

    d->device_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(d->device_lbl, lv_color_hex(COL_INK_MUTE), 0);
    lv_obj_set_style_text_font(d->device_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(d->device_lbl, "Clawd");
    lv_obj_align(d->device_lbl, LV_ALIGN_TOP_MID, 0, DEVICE_Y);

    /* Feed rows — single label per row. Verb on the left (big), rest
     * of the row info (time/status/chip/target) packed into one
     * mono-style label on the right. Both labels are width-constrained
     * with LV_LABEL_LONG_DOT so neither can ever spill into the
     * adjacent row. */
    for (int i = 0; i < FEED_ROWS_MAX; ++i) {
        int y = FEED_TOP_Y + i * FEED_ROW_H;
        d->row_verb[i]  = make_row_label(parent, COL_VERB_X,  y - 4,
                                          &lv_font_montserrat_22,
                                          COL_TEXT, COL_VERB_W, LV_TEXT_ALIGN_LEFT);
        d->row_label[i] = make_row_label(parent, COL_REST_X,  y + 4,
                                          &lv_font_montserrat_14,
                                          COL_TEXT_DIM, COL_REST_W, LV_TEXT_ALIGN_LEFT);
    }

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
    .description  = "AMBIENT feed of recent completions across all agents.",
    .tags         = "dashboard,feed,ambient,home",
    .init         = init,
    .on_show      = on_show,
    .on_hide      = on_hide,
};
