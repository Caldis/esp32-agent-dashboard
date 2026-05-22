/*
 * scene_dashboard — single-screen overview, the device's home scene.
 *
 * Layout (466 × 466 round AMOLED):
 *   • header strip: device name + "(stale)" hint if connection is old
 *   • top half: condensed dual-agent summary — kind label, status dot,
 *     msg or top entry text. No tokens, no entries list (that's
 *     scene_sessions territory).
 *   • bottom half: aggregate token sparkline (sum across agents) +
 *     running / waiting counters.
 *
 * Reads agent_state every 250 ms. All work happens under bsp_display_lock
 * inside the tick (LVGL timer thread is always on the LVGL task, so the
 * lock is held implicitly by the timer dispatch).
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
#define STALE_MS       30000

typedef struct {
    lv_obj_t *card;
    lv_obj_t *kind_lbl;
    lv_obj_t *dot;
    lv_obj_t *msg_lbl;
    lv_obj_t *tokens_lbl;
} dash_card_t;

typedef struct {
    lv_obj_t   *header;
    lv_obj_t   *stale;        /* "(stale)" hint */
    dash_card_t cards[2];
    lv_obj_t   *running_lbl;
    lv_obj_t   *running_cap;
    lv_obj_t   *waiting_lbl;
    lv_obj_t   *waiting_cap;
    lv_obj_t   *spark_line;
    lv_point_precise_t spark_pts[AGENT_SPARK_SAMPLES * AGENT_SLOT_MAX];
    lv_timer_t *timer;
} dash_state_t;

static const char *status_text(agent_status_t st)
{
    switch (st) {
        case AGENT_STATUS_RUNNING: return "running";
        case AGENT_STATUS_WAITING: return "waiting";
        default:                   return "idle";
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

static void build_card(lv_obj_t *parent, dash_card_t *c)
{
    c->card = lv_obj_create(parent);
    lv_obj_remove_style_all(c->card);
    lv_obj_clear_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(c->card, 12, 0);
    lv_obj_set_style_border_width(c->card, 1, 0);
    lv_obj_set_style_border_opa(c->card, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(c->card, LV_OPA_20, 0);

    c->kind_lbl = lv_label_create(c->card);
    lv_obj_set_style_text_font(c->kind_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(c->kind_lbl, 1, 0);
    lv_label_set_text(c->kind_lbl, "—");
    lv_obj_align(c->kind_lbl, LV_ALIGN_TOP_LEFT, 12, 8);

    c->dot = lv_obj_create(c->card);
    lv_obj_remove_style_all(c->dot);
    lv_obj_set_size(c->dot, 8, 8);
    lv_obj_set_style_radius(c->dot, 4, 0);
    lv_obj_set_style_bg_opa(c->dot, LV_OPA_COVER, 0);
    lv_obj_align(c->dot, LV_ALIGN_TOP_RIGHT, -12, 16);

    c->msg_lbl = lv_label_create(c->card);
    lv_obj_set_style_text_font(c->msg_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(c->msg_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(c->msg_lbl, "");
    lv_obj_align(c->msg_lbl, LV_ALIGN_TOP_LEFT, 12, 32);

    c->tokens_lbl = lv_label_create(c->card);
    lv_obj_set_style_text_font(c->tokens_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(c->tokens_lbl, "0");
    lv_obj_align(c->tokens_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -8);
}

static void layout_card(dash_card_t *c, int x, int y, int w, int h)
{
    lv_obj_set_pos(c->card, x, y);
    lv_obj_set_size(c->card, w, h);
    lv_obj_set_width(c->msg_lbl, w - 24);
}

static void paint_card(dash_card_t *c, const agent_slot_t *slot,
                       const theme_palette_t *pal, bool active)
{
    if (!active) {
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(c->card, LV_OBJ_FLAG_HIDDEN);

    uint32_t accent = theme_accent_for_kind(slot->kind);
    lv_obj_set_style_border_color(c->card, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_color(c->card, lv_color_hex(accent), 0);
    lv_obj_set_style_text_color(c->kind_lbl, lv_color_hex(accent), 0);
    lv_label_set_text(c->kind_lbl, slot->kind);

    uint32_t scol = status_colour(slot->status, pal);
    lv_obj_set_style_bg_color(c->dot, lv_color_hex(scol), 0);

    const char *body = slot->msg[0]
        ? slot->msg
        : (slot->entry_count > 0 ? slot->entries[0].text : status_text(slot->status));
    lv_label_set_text(c->msg_lbl, body ? body : "");
    lv_obj_set_style_text_color(c->msg_lbl, lv_color_hex(pal->text), 0);
    lv_obj_set_style_text_opa(c->msg_lbl, LV_OPA_80, 0);

    char tk[24]; format_tokens(slot->tokens_cumulative, tk, sizeof(tk));
    lv_label_set_text(c->tokens_lbl, tk);
    lv_obj_set_style_text_color(c->tokens_lbl, lv_color_hex(pal->text), 0);
}

static void render_sparkline(dash_state_t *st, const agent_slot_t snap[],
                             const theme_palette_t *pal, int x, int y, int w, int h)
{
    /* Pick the agent with the most samples; render its sparkline.
     * Aggregate sparkline across agents is tempting but the host pushes
     * samples per-agent so summing wouldn't reflect timing alignment. */
    const agent_slot_t *primary = NULL;
    int best = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!snap[i].in_use) continue;
        if (snap[i].spark_count > best) {
            best = snap[i].spark_count;
            primary = &snap[i];
        }
    }
    if (!primary || primary->spark_count < 2) {
        lv_line_set_points(st->spark_line, NULL, 0);
        return;
    }
    int n = primary->spark_count;
    uint32_t maxv = 1;
    for (int i = 0; i < n; ++i) {
        int idx = (primary->spark_head - n + i + AGENT_SPARK_SAMPLES) % AGENT_SPARK_SAMPLES;
        if (primary->spark[idx] > maxv) maxv = primary->spark[idx];
    }
    int x_step = (n > 1) ? (w / (n - 1)) : w;
    for (int i = 0; i < n; ++i) {
        int idx = (primary->spark_head - n + i + AGENT_SPARK_SAMPLES) % AGENT_SPARK_SAMPLES;
        uint32_t v = primary->spark[idx];
        int yy = h - (int)(((uint64_t)v * (uint64_t)h) / (uint64_t)maxv);
        st->spark_pts[i].x = x + i * x_step;
        st->spark_pts[i].y = y + yy;
    }
    uint32_t accent = theme_accent_for_kind(primary->kind);
    lv_obj_set_style_line_color(st->spark_line, lv_color_hex(accent), 0);
    lv_line_set_points(st->spark_line, st->spark_pts, n);
}

static void dash_tick(lv_timer_t *t)
{
    dash_state_t *st = (dash_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    agent_slot_t snap[AGENT_SLOT_MAX];
    int run = 0, wait = 0;
    char dev_name[AGENT_DEVICE_NAME_MAX];
    uint32_t last_snap_ms;
    bool ever;

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) snap[i] = s->slots[i];
    run = s->running; wait = s->waiting;
    /* If totals weren't set, compute. */
    if (run == 0 && wait == 0) {
        for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
            if (!snap[i].in_use) continue;
            if (snap[i].status == AGENT_STATUS_RUNNING) run++;
            else if (snap[i].status == AGENT_STATUS_WAITING) wait++;
        }
    }
    strncpy(dev_name, s->device_name, sizeof(dev_name));
    dev_name[sizeof(dev_name)-1] = '\0';
    last_snap_ms = s->last_snapshot_ms;
    ever = s->ever_received;
    agent_state_unlock();

    const theme_palette_t *pal = theme_current();

    /* Header */
    lv_obj_set_style_text_color(st->header, lv_color_hex(pal->text), 0);
    lv_label_set_text(st->header, dev_name[0] ? dev_name : "DASHBOARD");

    /* Stale indicator */
    bool stale = ever && (lv_tick_get() - last_snap_ms > STALE_MS);
    if (stale) {
        lv_obj_clear_flag(st->stale, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(st->stale, lv_color_hex(pal->text_dim), 0);
    } else {
        lv_obj_add_flag(st->stale, LV_OBJ_FLAG_HIDDEN);
    }

    /* Cards */
    agent_slot_t *left = NULL, *right = NULL;
    int slot_count = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!snap[i].in_use) continue;
        slot_count++;
        if (!left)  { left  = &snap[i]; continue; }
        if (!right) { right = &snap[i]; }
    }

    int card_y = 60;
    int card_h = 130;
    if (slot_count == 1 && left) {
        layout_card(&st->cards[0], 50, card_y, SCREEN_W - 100, card_h);
        paint_card(&st->cards[0], left,  pal, true);
        paint_card(&st->cards[1], NULL,  pal, false);
    } else if (slot_count >= 2) {
        int half = (SCREEN_W - 30) / 2;
        layout_card(&st->cards[0], 10,            card_y, half, card_h);
        layout_card(&st->cards[1], 10 + half + 10, card_y, half, card_h);
        paint_card(&st->cards[0], left,  pal, true);
        paint_card(&st->cards[1], right, pal, true);
    } else {
        paint_card(&st->cards[0], NULL, pal, false);
        paint_card(&st->cards[1], NULL, pal, false);
    }

    /* Counters */
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", run);
    lv_label_set_text(st->running_lbl, buf);
    lv_obj_set_style_text_color(st->running_lbl, lv_color_hex(pal->success), 0);
    lv_obj_set_style_text_color(st->running_cap, lv_color_hex(pal->text_dim), 0);

    snprintf(buf, sizeof(buf), "%d", wait);
    lv_label_set_text(st->waiting_lbl, buf);
    lv_obj_set_style_text_color(st->waiting_lbl, lv_color_hex(pal->warning), 0);
    lv_obj_set_style_text_color(st->waiting_cap, lv_color_hex(pal->text_dim), 0);

    /* Sparkline area: bottom band */
    render_sparkline(st, snap, pal, 70, 320, 326, 80);
}

static void dash_init(scene_t *s, lv_obj_t *parent)
{
    dash_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    const theme_palette_t *pal = theme_current();

    /* Header */
    st->header = lv_label_create(parent);
    lv_obj_set_style_text_font(st->header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->header, 4, 0);
    lv_obj_set_style_text_color(st->header, lv_color_hex(pal->text), 0);
    lv_label_set_text(st->header, "DASHBOARD");
    lv_obj_align(st->header, LV_ALIGN_TOP_MID, 0, 20);

    /* Stale indicator next to header */
    st->stale = lv_label_create(parent);
    lv_obj_set_style_text_font(st->stale, &lv_font_montserrat_12, 0);
    lv_label_set_text(st->stale, "(stale)");
    lv_obj_align(st->stale, LV_ALIGN_TOP_MID, 80, 22);
    lv_obj_add_flag(st->stale, LV_OBJ_FLAG_HIDDEN);

    build_card(parent, &st->cards[0]);
    build_card(parent, &st->cards[1]);

    /* Running counter */
    st->running_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->running_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(st->running_lbl, "0");
    lv_obj_align(st->running_lbl, LV_ALIGN_CENTER, -90, 60);

    st->running_cap = lv_label_create(parent);
    lv_obj_set_style_text_font(st->running_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(st->running_cap, 2, 0);
    lv_label_set_text(st->running_cap, "RUN");
    lv_obj_align(st->running_cap, LV_ALIGN_CENTER, -90, 86);

    /* Waiting counter */
    st->waiting_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->waiting_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(st->waiting_lbl, "0");
    lv_obj_align(st->waiting_lbl, LV_ALIGN_CENTER, 90, 60);

    st->waiting_cap = lv_label_create(parent);
    lv_obj_set_style_text_font(st->waiting_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(st->waiting_cap, 2, 0);
    lv_label_set_text(st->waiting_cap, "WAIT");
    lv_obj_align(st->waiting_cap, LV_ALIGN_CENTER, 90, 86);

    /* Sparkline */
    st->spark_line = lv_line_create(parent);
    lv_obj_set_style_line_color(st->spark_line, lv_color_hex(pal->accent_claude), 0);
    lv_obj_set_style_line_width(st->spark_line, 2, 0);
    lv_obj_set_style_line_opa(st->spark_line, LV_OPA_80, 0);
    lv_obj_set_style_line_rounded(st->spark_line, true, 0);

    st->timer = lv_timer_create(dash_tick, 250, st);
    lv_timer_pause(st->timer);
    /* Defer first tick until on_show — see scene_sessions.c for the
     * stack-size rationale. */
}

static void dash_on_show(scene_t *s)
{
    dash_state_t *st = (dash_state_t *)s->user_data;
    if (st && st->timer) {
        lv_timer_resume(st->timer);
        dash_tick(st->timer);
    }
}

static void dash_on_hide(scene_t *s)
{
    dash_state_t *st = (dash_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_dashboard = {
    .id           = "dashboard",
    .display_name = "Dashboard",
    .accent       = LV_COLOR_MAKE(0xFF, 0x8B, 0x5C),
    .description  = "Home scene: dual-agent summary cards plus run/wait counters and sparkline.",
    .tags         = "agent,dashboard,home",
    .init         = dash_init,
    .on_show      = dash_on_show,
    .on_hide      = dash_on_hide,
};
