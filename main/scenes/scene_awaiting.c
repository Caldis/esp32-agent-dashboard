/*
 * scene_awaiting — the takeover scene when an agent is blocking on user
 * input. v2.3.0 ships this as a first-class scene (not a banner over
 * dashboard) because Anthropic's own Agent View (2026-05) made
 * "which agents are waiting on you" the most important info to surface.
 *
 * One template, five kind variants:
 *
 *     ┌────────────────────────────────┐
 *     │     [eyebrow: time · device]    │
 *     │                                 │
 *     │            [GLYPH]              │  ← kind-specific
 *     │                                 │
 *     │         <HEADLINE>              │  ← 48pt, kind-specific
 *     │           cc · a3               │  ← agent (accent color)
 *     │       <context line 1>          │  ← up to 3 lines
 *     │       <context line 2>          │
 *     │       <context line 3>          │
 *     │                                 │
 *     │       waiting Xs · +N           │  ← duration + overflow
 *     └────────────────────────────────┘
 *
 * Reads the "most recent awaiting slot" from agent_state. The
 * agent_state code keeps awaiting_entered_ms stable across snapshots
 * that re-affirm the same kind, so "waiting Xs" counts from the
 * actual start of the wait.
 *
 * The headline pulses subtly on the continue kind (the dot inside the
 * ring grows + shrinks every 4s) so the device feels "alive" without
 * being distracting. Approve / clarify use a steady non-animated
 * presence — the gold accent already signals "agent stuck".
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "anim/apple_ease.h"
#include "harness/scene_framework.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

/* Cached UI objects (created in on_show, freed in on_hide). */
typedef struct {
    lv_obj_t *eyebrow;
    lv_obj_t *glyph;            /* parent for the kind-specific drawing */
    lv_obj_t *glyph_inner_dot;  /* used by continue kind for breathing */
    lv_obj_t *headline;
    lv_obj_t *agent_chip;
    lv_obj_t *ctx_lines[AGENT_AWAITING_CONTEXT_LINES];
    /* v2.4.0: marquee summary + numbered options list */
    lv_obj_t *summary_marquee;
    lv_obj_t *option_rows[AGENT_AWAITING_OPTIONS_MAX];
    lv_obj_t *affordance;
    lv_obj_t *footer;
    awaiting_kind_t last_rendered_kind;
    char        last_session_id[AGENT_SESSION_ID_MAX];
    uint32_t    breath_anim_armed;
} await_ui_t;

static await_ui_t s_ui;

/* ── Headline text per kind ──────────────────────────────────────── */

static const char *headline_for(awaiting_kind_t k)
{
    switch (k) {
        case AWAITING_CONTINUE: return "your turn";
        case AWAITING_APPROVE:  return "> approve?";
        case AWAITING_PICK:     return "pick one";
        case AWAITING_TYPE:     return "type a reply";
        case AWAITING_CLARIFY:  return "> clarify";
        default:                return "";
    }
}

static bool is_urgent(awaiting_kind_t k)
{
    return (k == AWAITING_APPROVE) || (k == AWAITING_CLARIFY);
}

/* ── Layout constants ────────────────────────────────────────────── */

#define SCREEN_W  466
#define SCREEN_H  466

/* v2.4.0 layout. The core group (glyph + headline + agent + summary +
 * options/ctx) is vertically centered per-frame so a 3-option turn
 * doesn't leave a void at the bottom and a 0-option turn doesn't
 * top-stick. Eyebrow and footer are anchored to panel edges and
 * unchanged; everything in between gets a dynamic top offset
 * computed in tick(). The constants below are the "stack heights" —
 * how tall each element is in lv coordinates — used to sum content
 * height for the centering math. */
#define EYEBROW_Y         44   /* fixed: top of panel inscribed area */
#define FOOTER_Y         432   /* fixed: bottom */
#define GLYPH_H           76   /* glyph container is 96x96 visually; usable centerline */
#define HEADLINE_H        54   /* 48pt montserrat line height */
#define AGENT_H           30   /* reduced from 36 with 28pt -> 22pt font */
#define SUMMARY_H         34   /* marquee strip */
#define OPTION_ROW_H      32
#define CTX_LINE_H        32
#define AFFORDANCE_H      24
#define INTER_GAP          6   /* between adjacent vertical elements */

/* ── Glyph rendering ────────────────────────────────────────────── */

static void clear_children(lv_obj_t *parent)
{
    if (parent == NULL) return;
    lv_obj_clean(parent);
}

/* Draws a "pulse" glyph: outer ring 72px + filled inner dot 16px.
 * The inner dot is stored as s_ui.glyph_inner_dot so the continue
 * kind can animate it. */
static void glyph_pulse(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_set_size(ring, 72, 72);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(ring);
    lv_obj_set_size(dot, 18, 18);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.glyph_inner_dot = dot;
}

/* Generic symbol glyph: uses LV_SYMBOL_* with a big font. */
static void glyph_symbol(lv_obj_t *parent, const char *symbol, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_36, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

static void render_glyph(awaiting_kind_t k, uint32_t color)
{
    if (s_ui.glyph == NULL) return;
    clear_children(s_ui.glyph);
    s_ui.glyph_inner_dot = NULL;
    switch (k) {
        case AWAITING_CONTINUE:
            glyph_pulse(s_ui.glyph, color);
            break;
        case AWAITING_APPROVE:
            glyph_symbol(s_ui.glyph, LV_SYMBOL_WARNING, color);
            break;
        case AWAITING_PICK:
            glyph_symbol(s_ui.glyph, LV_SYMBOL_LIST, color);
            break;
        case AWAITING_TYPE:
            glyph_symbol(s_ui.glyph, LV_SYMBOL_EDIT, color);
            break;
        case AWAITING_CLARIFY:
            glyph_symbol(s_ui.glyph, LV_SYMBOL_BELL, color);
            break;
        default:
            break;
    }
}

/* Breathing animation for the continue kind's inner dot.
 * Loops 0..1 over 2s, two phases (in + out), apple_ease_out path. */
static void anim_breath_size(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
}

static void arm_breath(void)
{
    if (s_ui.glyph_inner_dot == NULL) return;
    if (s_ui.breath_anim_armed) return;
    s_ui.breath_anim_armed = 1;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ui.glyph_inner_dot);
    lv_anim_set_values(&a, 14, 28);
    lv_anim_set_time(&a, 2000);
    lv_anim_set_playback_time(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, apple_ease_out);
    lv_anim_set_exec_cb(&a, anim_breath_size);
    lv_anim_start(&a);
}

/* ── Per-tick update ─────────────────────────────────────────────── */

static void format_duration(char *buf, size_t cap, uint32_t since_unix, uint32_t now_unix)
{
    if (since_unix == 0 || now_unix == 0 || since_unix > now_unix) {
        if (cap > 0) buf[0] = '\0';
        return;
    }
    uint32_t d = now_unix - since_unix;
    if (d < 60) snprintf(buf, cap, "waiting %us", (unsigned)d);
    else if (d < 3600) snprintf(buf, cap, "waiting %um %us", (unsigned)(d / 60), (unsigned)(d % 60));
    else snprintf(buf, cap, "waiting %uh %um", (unsigned)(d / 3600), (unsigned)((d % 3600) / 60));
}

static void format_eyebrow(char *buf, size_t cap, const agent_state_t *st)
{
    /* Best effort time string from host_epoch_unix; fallback to just device_name. */
    const char *name = st->device_name[0] ? st->device_name : "DASHBOARD";
    if (st->host_epoch_unix > 0) {
        uint32_t now = st->host_epoch_unix
                     + (lv_tick_get() - st->host_clock_received_ms) / 1000;
        int32_t tz_now = (int32_t)now + st->host_tz_offset_seconds;
        struct tm tmv;
        time_t tt = (time_t)tz_now;
        gmtime_r(&tt, &tmv);
        snprintf(buf, cap, "%02d:%02d  %s", tmv.tm_hour, tmv.tm_min, name);
    } else {
        snprintf(buf, cap, "%s", name);
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    agent_state_lock();
    agent_state_t *st = agent_state_get();
    agent_slot_t *anchor = agent_state_most_recent_awaiting();
    if (anchor == NULL) {
        /* No awaiting → leave to scene framework to switch us out;
         * we just don't update anything. */
        agent_state_unlock();
        return;
    }
    int more = agent_state_other_awaiting_count(anchor);
    awaiting_kind_t kind = anchor->awaiting_kind;
    /* Urgency-coded accent: gold for blocks that need attention,
     * teal-bright for "your turn but no rush" continuations. Both pull
     * from palette.md (gold = #B89020, teal-bright = #2BB3B1). */
    uint32_t color = is_urgent(kind) ? 0xB89020 : 0x2BB3B1;

    /* Has the kind or session changed since last render? Rebuild glyph + header. */
    bool sess_changed = strncmp(anchor->session_id, s_ui.last_session_id,
                                AGENT_SESSION_ID_MAX) != 0;
    bool kind_changed = (kind != s_ui.last_rendered_kind);
    bool motion_ok = !st->motion_reduced;
    if (kind_changed || sess_changed) {
        render_glyph(kind, color);
        if (kind == AWAITING_CONTINUE && motion_ok) {
            arm_breath();
        } else {
            s_ui.breath_anim_armed = 0;
        }
        lv_label_set_text(s_ui.headline, headline_for(kind));
        s_ui.last_rendered_kind = kind;
        strncpy(s_ui.last_session_id, anchor->session_id, AGENT_SESSION_ID_MAX - 1);
        s_ui.last_session_id[AGENT_SESSION_ID_MAX - 1] = '\0';
        /* Agent color follows urgency */
        lv_obj_set_style_text_color(s_ui.agent_chip, lv_color_hex(color), 0);
    }

    /* Agent chip — "cc abc4:6f" (kind + first 4 + ':' + last 2). v2.7.0
     * fix per Persona C: long session_ids (full UUIDs) trimmed to last-6
     * looked like gibberish ("ve_sx5"). first-4 + last-2 keeps the
     * recognisable prefix AND a uniqueness tail. Short sids (< 6 chars)
     * render whole. */
    char chip[64];
    const char *short_kind = (strcmp(anchor->kind, "claude-code") == 0) ? "cc"
                           : (strcmp(anchor->kind, "codex") == 0)       ? "cx"
                           : (strcmp(anchor->kind, "cursor") == 0)      ? "cu"
                           : (strcmp(anchor->kind, "aider") == 0)       ? "ai"
                           : (strcmp(anchor->kind, "windsurf") == 0)    ? "ws"
                           : (strcmp(anchor->kind, "copilot") == 0)     ? "cp"
                           : (strcmp(anchor->kind, "qwen-code") == 0)   ? "qw"
                           :                                              "ag";
    const char *sid = anchor->session_id;
    size_t sid_len = strlen(sid);
    char sid_display[10];
    if (sid_len <= 6) {
        snprintf(sid_display, sizeof(sid_display), "%s", sid);
    } else {
        /* "abcd:9f" — 4 chars head, colon, 2 chars tail */
        snprintf(sid_display, sizeof(sid_display), "%.4s:%s",
                 sid, sid + sid_len - 2);
    }
    snprintf(chip, sizeof(chip), "%s  %s", short_kind, sid_display);
    lv_label_set_text(s_ui.agent_chip, chip);

    /* v2.4.0: decide layout mode for this frame + compute the dynamic
     * vertical-center offset so the core group sits balanced regardless
     * of how many options / context lines are visible. The available
     * vertical real estate is from EYEBROW_Y+12 to FOOTER_Y-12. We sum
     * the visible content height (glyph + headline + agent + summary?
     * + N_options × row_h | + N_ctx × ctx_h), compute remaining
     * whitespace, and split it half-above the group. */
    bool has_summary = (anchor->awaiting_summary[0] != '\0');
    bool has_options = (anchor->awaiting_options_count > 0);
    int n_opts = anchor->awaiting_options_count;
    if (n_opts > AGENT_AWAITING_OPTIONS_MAX) n_opts = AGENT_AWAITING_OPTIONS_MAX;
    int n_ctx = anchor->awaiting_context_count;
    if (n_ctx > AGENT_AWAITING_CONTEXT_LINES) n_ctx = AGENT_AWAITING_CONTEXT_LINES;

    bool show_affordance = (kind == AWAITING_APPROVE);
    int content_h = GLYPH_H + INTER_GAP
                  + HEADLINE_H + INTER_GAP
                  + AGENT_H;
    if (show_affordance)          content_h += INTER_GAP + AFFORDANCE_H;
    if (has_summary)              content_h += INTER_GAP + SUMMARY_H;
    if (has_options)              content_h += INTER_GAP + n_opts * OPTION_ROW_H;
    if (!has_options && !has_summary && n_ctx > 0) {
        content_h += INTER_GAP + n_ctx * CTX_LINE_H;
    }

    int avail_top    = EYEBROW_Y + 16;        /* below eyebrow */
    int avail_bottom = FOOTER_Y - 14;         /* above footer */
    int avail_h      = avail_bottom - avail_top;
    int top_pad      = (avail_h - content_h) / 2;
    if (top_pad < 0) top_pad = 0;
    int y = avail_top + top_pad;

    /* Re-align the core group at the new y. */
    lv_obj_align(s_ui.glyph,      LV_ALIGN_TOP_MID, 0, y);
    y += GLYPH_H + INTER_GAP;
    lv_obj_align(s_ui.headline,   LV_ALIGN_TOP_MID, 0, y);
    y += HEADLINE_H + INTER_GAP;
    lv_obj_align(s_ui.agent_chip, LV_ALIGN_TOP_MID, 0, y);
    y += AGENT_H + INTER_GAP;

    if (show_affordance) {
        lv_obj_align(s_ui.affordance, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(s_ui.affordance, LV_OBJ_FLAG_HIDDEN);
        y += AFFORDANCE_H + INTER_GAP;
    } else {
        lv_obj_add_flag(s_ui.affordance, LV_OBJ_FLAG_HIDDEN);
    }

    if (has_summary) {
        lv_obj_align(s_ui.summary_marquee, LV_ALIGN_TOP_MID, 0, y);
        y += SUMMARY_H + INTER_GAP;
    }
    if (has_options) {
        int ox = (SCREEN_W - 380) / 2;
        for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
            lv_obj_align(s_ui.option_rows[i], LV_ALIGN_TOP_LEFT, ox, y);
            if (i < n_opts) y += OPTION_ROW_H;
        }
    } else if (!has_summary) {
        for (int i = 0; i < AGENT_AWAITING_CONTEXT_LINES; ++i) {
            lv_obj_align(s_ui.ctx_lines[i], LV_ALIGN_TOP_MID, 0, y);
            if (i < n_ctx) y += CTX_LINE_H;
        }
    }

    if (has_summary) {
        lv_label_set_long_mode(s_ui.summary_marquee,
            motion_ok ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
        lv_label_set_text(s_ui.summary_marquee, anchor->awaiting_summary);
        lv_obj_clear_flag(s_ui.summary_marquee, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.summary_marquee, LV_OBJ_FLAG_HIDDEN);
    }

    if (has_options) {
        for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
            if (i < anchor->awaiting_options_count
                && anchor->awaiting_options[i][0] != '\0') {
                char row[64];
                snprintf(row, sizeof(row), "%d.  %s",
                         i + 1, anchor->awaiting_options[i]);
                lv_label_set_text(s_ui.option_rows[i], row);
                lv_obj_clear_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
            lv_obj_add_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Context lines only when neither summary nor options present —
     * i.e. v2.3.0 fallback. */
    bool show_ctx = !has_summary && !has_options;
    for (int i = 0; i < AGENT_AWAITING_CONTEXT_LINES; ++i) {
        if (show_ctx && i < anchor->awaiting_context_count
            && anchor->awaiting_context[i][0] != '\0') {
            lv_label_set_text(s_ui.ctx_lines[i], anchor->awaiting_context[i]);
            lv_obj_clear_flag(s_ui.ctx_lines[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.ctx_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Footer "waiting Xs · +N more" */
    uint32_t now_unix = st->host_epoch_unix
                      ? (st->host_epoch_unix +
                         (lv_tick_get() - st->host_clock_received_ms) / 1000)
                      : 0;
    char dur[40];
    format_duration(dur, sizeof(dur), anchor->awaiting_since_unix, now_unix);
    char footer[128];
    if (more > 0) {
        snprintf(footer, sizeof(footer), "%s    +%d more", dur, more);
    } else {
        snprintf(footer, sizeof(footer), "%s", dur);
    }
    lv_label_set_text(s_ui.footer, footer);

    /* Eyebrow: "HH:MM · device_name" — needs room for up to 32-char
     * device_name + 10 fixed chars + nul, so 64 is safe. */
    char eb[64];
    format_eyebrow(eb, sizeof(eb), st);
    lv_label_set_text(s_ui.eyebrow, eb);

    agent_state_unlock();
}

/* ── Scene framework hooks ───────────────────────────────────────── */

static lv_timer_t *s_tick_timer;

static void init(scene_t *s, lv_obj_t *parent)
{
    s->container = parent;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.last_rendered_kind = AWAITING_NONE;
    lv_obj_t *root = parent;
    /* Background */
    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(root, lv_color_hex(pal ? pal->bg : 0x0B0A09), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* Eyebrow */
    s_ui.eyebrow = lv_label_create(root);
    /* v2.7.0 Persona D fix: ink-mute #5A514A has WCAG contrast 2.59:1 vs
     * noir bg — fails AA. ink-fade #8A807A is 5.13:1, AA pass. Eyebrow
     * is a real text label readers need to parse, not decorative. */
    lv_obj_set_style_text_color(s_ui.eyebrow, lv_color_hex(0x8A807A), 0);
    lv_obj_set_style_text_font(s_ui.eyebrow, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ui.eyebrow, "");
    lv_obj_align(s_ui.eyebrow, LV_ALIGN_TOP_MID, 0, EYEBROW_Y);

    /* Glyph container — initial position; tick() re-aligns per-frame. */
    s_ui.glyph = lv_obj_create(root);
    lv_obj_set_size(s_ui.glyph, 96, 96);
    lv_obj_set_style_bg_opa(s_ui.glyph, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.glyph, 0, 0);
    lv_obj_set_style_pad_all(s_ui.glyph, 0, 0);
    lv_obj_clear_flag(s_ui.glyph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_ui.glyph, LV_ALIGN_TOP_MID, 0, EYEBROW_Y + 24);

    /* Headline — initial position; tick() re-aligns. */
    s_ui.headline = lv_label_create(root);
    lv_obj_set_style_text_color(s_ui.headline, lv_color_hex(0xF3EEE2), 0);
    lv_obj_set_style_text_font(s_ui.headline, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_ui.headline, "");
    lv_obj_align(s_ui.headline, LV_ALIGN_TOP_MID, 0, EYEBROW_Y + 100);

    /* Agent chip — initial position; tick() re-aligns. */
    /* v2.7.0 hierarchy fix (Persona C P0): agent chip was 28pt — too
     * dominant, competed with headline. Drop to 22pt so headline reads
     * clearly as the focal element. */
    s_ui.agent_chip = lv_label_create(root);
    lv_obj_set_style_text_color(s_ui.agent_chip, lv_color_hex(0x2BB3B1), 0);
    lv_obj_set_style_text_font(s_ui.agent_chip, &lv_font_montserrat_22, 0);
    lv_label_set_text(s_ui.agent_chip, "");
    lv_obj_align(s_ui.agent_chip, LV_ALIGN_TOP_MID, 0, EYEBROW_Y + 154);

    /* Affordance hint for approve kind — tells user which physical
     * buttons map to approve / deny. Hidden until kind == APPROVE. */
    s_ui.affordance = lv_label_create(root);
    lv_obj_set_style_text_color(s_ui.affordance, lv_color_hex(0x8A807A), 0);
    lv_obj_set_style_text_font(s_ui.affordance, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ui.affordance, "BOOT approve  \xC2\xB7  USER deny");
    lv_obj_add_flag(s_ui.affordance, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_ui.affordance, LV_ALIGN_TOP_MID, 0, 0);

    /* Context lines (used when no dash-state summary). */
    for (int i = 0; i < AGENT_AWAITING_CONTEXT_LINES; ++i) {
        s_ui.ctx_lines[i] = lv_label_create(root);
        lv_obj_set_width(s_ui.ctx_lines[i], 370);
        lv_obj_set_style_text_align(s_ui.ctx_lines[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_ui.ctx_lines[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(s_ui.ctx_lines[i], lv_color_hex(0x8A807A), 0);
        lv_obj_set_style_text_font(s_ui.ctx_lines[i], &lv_font_montserrat_22, 0);
        lv_label_set_text(s_ui.ctx_lines[i], "");
        lv_obj_add_flag(s_ui.ctx_lines[i], LV_OBJ_FLAG_HIDDEN);
        /* Initial position; tick() re-aligns. */
        lv_obj_align(s_ui.ctx_lines[i], LV_ALIGN_TOP_MID, 0, EYEBROW_Y + 220);
    }

    /* v2.4.0: marquee summary — single line, scrolls left if text wider
     * than container. LV_LABEL_LONG_SCROLL is LVGL's "airport-board"
     * mode: text moves left at a steady ~30 px/s, wraps after a gap,
     * loops indefinitely. Width set to inscribed-square so it never
     * clips the round panel edges. */
    s_ui.summary_marquee = lv_label_create(root);
    lv_obj_set_width(s_ui.summary_marquee, 380);
    lv_label_set_long_mode(s_ui.summary_marquee, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_ui.summary_marquee, lv_color_hex(0xF3EEE2), 0);
    lv_obj_set_style_text_font(s_ui.summary_marquee, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_ui.summary_marquee, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ui.summary_marquee, "");
    lv_obj_add_flag(s_ui.summary_marquee, LV_OBJ_FLAG_HIDDEN);
    /* Initial position; tick() re-aligns. */
    lv_obj_align(s_ui.summary_marquee, LV_ALIGN_TOP_MID, 0, EYEBROW_Y + 220);

    /* Numbered options 1..4 — each row "N.  <option text>". The number
     * is in accent color, the text in TEXT_DIM. User reads, switches
     * to terminal, types the digit (CC accepts the verbatim option as
     * the next prompt). */
    for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
        s_ui.option_rows[i] = lv_label_create(root);
        lv_obj_set_width(s_ui.option_rows[i], 380);
        lv_label_set_long_mode(s_ui.option_rows[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(s_ui.option_rows[i], lv_color_hex(0xF3EEE2), 0);
        lv_obj_set_style_text_font(s_ui.option_rows[i], &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_align(s_ui.option_rows[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(s_ui.option_rows[i], "");
        lv_obj_add_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Footer */
    s_ui.footer = lv_label_create(root);
    /* v2.7.0 Persona D fix: ink-mute -> ink-fade for WCAG AA contrast. */
    lv_obj_set_style_text_color(s_ui.footer, lv_color_hex(0x8A807A), 0);
    lv_obj_set_style_text_font(s_ui.footer, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_ui.footer, "");
    lv_obj_align(s_ui.footer, LV_ALIGN_TOP_MID, 0, FOOTER_Y);

    /* Don't run tick yet — wait for on_show. */
}

static void on_show(scene_t *s)
{
    (void)s;
    tick(NULL);
    if (s_tick_timer == NULL) {
        s_tick_timer = lv_timer_create(tick, 500, NULL);
    } else {
        lv_timer_resume(s_tick_timer);
    }
}

static void on_hide(scene_t *s)
{
    (void)s;
    if (s_tick_timer != NULL) {
        lv_timer_pause(s_tick_timer);
    }
    /* UI tree persists across hide/show — scene framework keeps the
     * parent container around, just hidden. Re-show reuses the labels
     * we created in init(). */
}

scene_t scene_awaiting = {
    .id           = "awaiting",
    .display_name = "Awaiting",
    .accent       = LV_COLOR_MAKE(0x2B, 0xB3, 0xB1),
    .description  = "ball is in user's court — input needed",
    .tags         = "system,awaiting",
    .init         = init,
    .on_show      = on_show,
    .on_hide      = on_hide,
};
