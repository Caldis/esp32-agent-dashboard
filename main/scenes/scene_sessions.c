/*
 * scene_sessions — dual-pane per-agent view.
 *
 * Vertically splits the screen. Left pane = slot[0] (typically claude-code),
 * right pane = slot[1] (typically codex). Per pane:
 *   • kind label at top, tinted in palette accent
 *   • status pill (running/waiting/idle)
 *   • last 2 transcript entries, each with a tool icon
 *   • tokens count at bottom
 *
 * If only one agent is present, that agent renders full-width.
 *
 * All widgets are pre-allocated in `init` and reused across snapshots.
 * Per-tick path: take agent_state lock briefly to copy what we need,
 * release, then mutate widgets.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "tool_icons.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define SCREEN_W       466
#define SCREEN_H       466

typedef struct {
    lv_obj_t *pane;
    lv_obj_t *accent_bar;
    lv_obj_t *kind_lbl;
    lv_obj_t *status_pill;
    lv_obj_t *status_lbl;
    lv_obj_t *entry_rows[2];
    lv_obj_t *entry_icons[2];
    lv_obj_t *entry_texts[2];
    lv_obj_t *tokens_lbl;
    lv_obj_t *tokens_caption;
} pane_widgets_t;

typedef struct {
    lv_obj_t       *roman;
    pane_widgets_t  panes[2];
    lv_obj_t       *empty_lbl;
    lv_timer_t     *timer;
} sessions_state_t;

static const char *status_text(agent_status_t st)
{
    switch (st) {
        case AGENT_STATUS_RUNNING: return "RUNNING";
        case AGENT_STATUS_WAITING: return "WAITING";
        default:                   return "IDLE";
    }
}

static uint32_t status_colour(agent_status_t st, const theme_palette_t *pal)
{
    switch (st) {
        case AGENT_STATUS_RUNNING: return pal->success;
        case AGENT_STATUS_WAITING: return pal->warning;
        default:                   return pal->text_dim;
    }
}

static void format_tokens(uint64_t v, char *buf, size_t cap)
{
    if (v < 1000) snprintf(buf, cap, "%llu", (unsigned long long)v);
    else if (v < 1000000ULL) snprintf(buf, cap, "%.1fk", (double)v / 1000.0);
    else snprintf(buf, cap, "%.1fM", (double)v / 1000000.0);
}

static void build_pane(lv_obj_t *parent, pane_widgets_t *pw)
{
    pw->pane = lv_obj_create(parent);
    lv_obj_remove_style_all(pw->pane);
    lv_obj_clear_flag(pw->pane, LV_OBJ_FLAG_SCROLLABLE);

    pw->accent_bar = lv_obj_create(pw->pane);
    lv_obj_remove_style_all(pw->accent_bar);
    lv_obj_set_size(pw->accent_bar, 3, 200);
    lv_obj_set_style_radius(pw->accent_bar, 2, 0);
    lv_obj_set_style_bg_opa(pw->accent_bar, LV_OPA_80, 0);

    pw->kind_lbl = lv_label_create(pw->pane);
    lv_obj_set_style_text_font(pw->kind_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(pw->kind_lbl, 1, 0);
    lv_label_set_text(pw->kind_lbl, "—");

    pw->status_pill = lv_obj_create(pw->pane);
    lv_obj_remove_style_all(pw->status_pill);
    lv_obj_clear_flag(pw->status_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(pw->status_pill, 80, 18);
    lv_obj_set_style_radius(pw->status_pill, 9, 0);
    lv_obj_set_style_bg_opa(pw->status_pill, LV_OPA_30, 0);
    lv_obj_set_style_border_width(pw->status_pill, 1, 0);
    lv_obj_set_style_border_opa(pw->status_pill, LV_OPA_70, 0);

    pw->status_lbl = lv_label_create(pw->status_pill);
    lv_obj_set_style_text_font(pw->status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(pw->status_lbl, 1, 0);
    lv_label_set_text(pw->status_lbl, "IDLE");
    lv_obj_center(pw->status_lbl);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *row = lv_obj_create(pw->pane);
        lv_obj_remove_style_all(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        pw->entry_icons[i] = lv_label_create(row);
        lv_obj_set_style_text_font(pw->entry_icons[i], &lv_font_montserrat_14, 0);
        lv_label_set_text(pw->entry_icons[i], "");
        lv_obj_align(pw->entry_icons[i], LV_ALIGN_LEFT_MID, 0, 0);

        pw->entry_texts[i] = lv_label_create(row);
        lv_obj_set_style_text_font(pw->entry_texts[i], &lv_font_montserrat_12, 0);
        lv_label_set_long_mode(pw->entry_texts[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(pw->entry_texts[i], "");
        lv_obj_align(pw->entry_texts[i], LV_ALIGN_LEFT_MID, 22, 0);

        pw->entry_rows[i] = row;
    }

    pw->tokens_lbl = lv_label_create(pw->pane);
    lv_obj_set_style_text_font(pw->tokens_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(pw->tokens_lbl, "0");

    pw->tokens_caption = lv_label_create(pw->pane);
    lv_obj_set_style_text_font(pw->tokens_caption, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(pw->tokens_caption, 2, 0);
    lv_label_set_text(pw->tokens_caption, "TOKENS");
}

static void layout_pane(pane_widgets_t *pw, int x, int y, int w, int h)
{
    lv_obj_set_pos(pw->pane, x, y);
    lv_obj_set_size(pw->pane, w, h);

    lv_obj_align(pw->accent_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_height(pw->accent_bar, h - 30);

    lv_obj_align(pw->kind_lbl,    LV_ALIGN_TOP_LEFT, 14, 8);
    lv_obj_align(pw->status_pill, LV_ALIGN_TOP_LEFT, 14, 32);

    for (int i = 0; i < 2; ++i) {
        lv_obj_set_size(pw->entry_rows[i], w - 24, 22);
        lv_obj_set_pos(pw->entry_rows[i], 14, 62 + i * 26);
        lv_obj_set_width(pw->entry_texts[i], w - 50);
    }

    lv_obj_align(pw->tokens_lbl,     LV_ALIGN_BOTTOM_LEFT, 14, -32);
    lv_obj_align(pw->tokens_caption, LV_ALIGN_BOTTOM_LEFT, 14, -12);
}

static void paint_pane(pane_widgets_t *pw, const agent_slot_t *slot,
                       const theme_palette_t *pal, bool active)
{
    if (!active) {
        lv_obj_add_flag(pw->pane, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(pw->pane, LV_OBJ_FLAG_HIDDEN);

    uint32_t accent = theme_accent_for_kind(slot->kind);
    lv_obj_set_style_bg_color(pw->accent_bar, lv_color_hex(accent), 0);
    lv_obj_set_style_text_color(pw->kind_lbl, lv_color_hex(accent), 0);
    lv_label_set_text(pw->kind_lbl, slot->kind);

    uint32_t scol = status_colour(slot->status, pal);
    lv_obj_set_style_bg_color(pw->status_pill, lv_color_hex(scol), 0);
    lv_obj_set_style_border_color(pw->status_pill, lv_color_hex(scol), 0);
    lv_obj_set_style_text_color(pw->status_lbl, lv_color_hex(pal->text), 0);
    lv_label_set_text(pw->status_lbl, status_text(slot->status));

    int n = slot->entry_count;
    if (n > 2) n = 2;
    for (int i = 0; i < 2; ++i) {
        if (i < n) {
            const agent_entry_t *e = &slot->entries[i];
            const char *icon = tool_icon_for(e->tool);
            lv_label_set_text(pw->entry_icons[i], icon);
            lv_obj_set_style_text_color(pw->entry_icons[i],
                lv_color_hex(pal->text_dim), 0);

            const char *body = e->text[0] ? e->text : e->tool;
            if (!body || !body[0]) body = "—";
            lv_label_set_text(pw->entry_texts[i], body);
            lv_obj_set_style_text_color(pw->entry_texts[i],
                lv_color_hex(pal->text), 0);
            lv_obj_set_style_text_opa(pw->entry_texts[i],
                i == 0 ? LV_OPA_90 : LV_OPA_60, 0);
        } else {
            lv_label_set_text(pw->entry_icons[i], "");
            lv_label_set_text(pw->entry_texts[i], "");
        }
    }

    char tk[24]; format_tokens(slot->tokens_cumulative, tk, sizeof(tk));
    lv_label_set_text(pw->tokens_lbl, tk);
    lv_obj_set_style_text_color(pw->tokens_lbl, lv_color_hex(pal->text), 0);
    lv_obj_set_style_text_color(pw->tokens_caption, lv_color_hex(pal->text_dim), 0);
}

static void sessions_tick(lv_timer_t *t)
{
    sessions_state_t *st = (sessions_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    agent_slot_t snap[AGENT_SLOT_MAX];
    int slot_count = 0;

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        snap[i] = s->slots[i];
        if (snap[i].in_use) slot_count++;
    }
    agent_state_unlock();

    const theme_palette_t *pal = theme_current();

    agent_slot_t *left = NULL, *right = NULL;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!snap[i].in_use) continue;
        if (!left)  { left  = &snap[i]; continue; }
        if (!right) { right = &snap[i]; break; }
    }

    int top_y  = 44;
    int pane_h = SCREEN_H - 88;

    if (slot_count == 0) {
        lv_obj_add_flag(st->panes[0].pane, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(st->panes[1].pane, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(st->empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(st->empty_lbl, lv_color_hex(pal->text_dim), 0);
        return;
    }
    lv_obj_add_flag(st->empty_lbl, LV_OBJ_FLAG_HIDDEN);

    if (slot_count == 1 && left) {
        layout_pane(&st->panes[0], 30, top_y, SCREEN_W - 60, pane_h);
        paint_pane(&st->panes[0], left, pal, true);
        paint_pane(&st->panes[1], NULL, pal, false);
    } else {
        int half_w = (SCREEN_W - 20) / 2;
        layout_pane(&st->panes[0], 10,            top_y, half_w, pane_h);
        layout_pane(&st->panes[1], 10 + half_w,   top_y, half_w, pane_h);
        paint_pane(&st->panes[0], left,  pal, true);
        paint_pane(&st->panes[1], right, pal, true);
    }
}

static void sessions_init(scene_t *s, lv_obj_t *parent)
{
    sessions_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_obj_set_style_text_color(st->roman,
        lv_color_hex(theme_current()->text_dim), 0);
    lv_label_set_text(st->roman, "SESSIONS");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 16);

    build_pane(parent, &st->panes[0]);
    build_pane(parent, &st->panes[1]);

    st->empty_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->empty_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(st->empty_lbl, "no active sessions");
    lv_obj_center(st->empty_lbl);

    st->timer = lv_timer_create(sessions_tick, 200, st);
    lv_timer_pause(st->timer);
    /* Tick once on first on_show, not now — calling tick here would put
     * AGENT_SLOT_MAX × sizeof(agent_slot_t) on the main task stack which
     * blows the 4 KB default. */
}

static void sessions_on_show(scene_t *s)
{
    sessions_state_t *st = (sessions_state_t *)s->user_data;
    if (st && st->timer) {
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
    .display_name = "Sessions",
    .accent       = LV_COLOR_MAKE(0xFF, 0x8B, 0x5C),
    .description  = "Per-agent dual-pane view with status, recent tool entries, and tokens.",
    .tags         = "agent,sessions,multiagent",
    .init         = sessions_init,
    .on_show      = sessions_on_show,
    .on_hide      = sessions_on_hide,
};
