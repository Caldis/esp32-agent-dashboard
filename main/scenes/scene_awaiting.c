/*
 * scene_awaiting — the takeover scene when an agent is blocking on user
 * input. v2.3.0 ships this as a first-class scene (not a banner over
 * dashboard) because Anthropic's own Agent View (2026-05) made
 * "which agents are waiting on you" the most important info to surface.
 *
 * One template, five kind variants (v4.4 type-scale layout):
 *
 *     ┌────────────────────────────────┐
 *     │        [conn pill only]         │  ← top clock hidden
 *     │            [GLYPH]              │  ← minimal mode only
 *     │          <该你了>                │  ← HERO 88 (minimal) /
 *     │                                 │    TITLE 52 (content)
 *     │        cc esp32-agent…          │  ← LABEL chip (accent)
 *     │      <summary, 1-2 lines>       │  ← BODY, static wrap
 *     │      1  <option>                │  ← BODY rows, ≤3
 *     │      2  <option>   +N 更多      │
 *     │                                 │
 *     │      [active]   [tokens]        │  ← shared footer
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
#include "cjk_font.h"   /* cjk_utf8_lcpy */
#include "ui_type.h"
#include "status_bar.h"
#include "anim/apple_ease.h"
#include "harness/scene_framework.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

/* Cached UI objects (created in on_show, freed in on_hide). */
typedef struct {
    status_bar_t sb;            /* footer + conn pill; top clock hidden */
    lv_obj_t *glyph;            /* parent for the kind-specific drawing */
    lv_obj_t *glyph_inner_dot;  /* used by continue kind for breathing */
    lv_obj_t *headline;
    lv_obj_t *agent_chip;
    lv_obj_t *ctx_lines[AGENT_AWAITING_CONTEXT_LINES];
    /* v4.4: static wrapped summary + numbered options (3 shown + "+N") */
    lv_obj_t *summary_lbl;
    lv_obj_t *option_rows[AGENT_AWAITING_OPTIONS_MAX];
    lv_obj_t *more_lbl;
    lv_obj_t *affordance;
    awaiting_kind_t last_rendered_kind;
    int         headline_big;   /* -1 = unset; 1 = HERO, 0 = TITLE */
    char        last_session_id[AGENT_SESSION_ID_MAX];
    uint32_t    breath_anim_armed;
} await_ui_t;

static await_ui_t s_ui;

/* ── Headline text per kind ──────────────────────────────────────── */

/* Chinese primary words (v4.4): 3 hanzi at HERO tier subtend 22' at
 * 1 m — the takeover signal must read across the desk, which no
 * 9-12-char English headline can do on a 38.8 mm panel. All chars are
 * in the GB2312 subset. */
static const char *headline_for(awaiting_kind_t k)
{
    switch (k) {
        case AWAITING_CONTINUE: return "该你了";
        case AWAITING_APPROVE:  return "请批准";
        case AWAITING_PICK:     return "请选择";
        case AWAITING_TYPE:     return "请输入";
        case AWAITING_CLARIFY:  return "需澄清";
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

/* v4.4 layout. The scene hides the status bar's top clock (a takeover
 * asking for input doesn't need the time — scene_clock owns that), so
 * content owns the band from AWAIT_TOP (below the conn pill) to
 * UI_BAND_BOT (above the footer numbers). Two presentation modes:
 *
 *   minimal (no summary/options/ctx) → glyph + HERO headline + chip;
 *   content                          → TITLE headline + chip + wrapped
 *                                      summary + up to 3 option rows
 *                                      (+ "+N" overflow caption).
 *
 * The visible stack is vertically centered per-frame, same as v2.4.0.
 * All heights come from ui_type_line() — no free-hand pixel sizes. */
#define AWAIT_TOP         48
#define AWAIT_BOT         UI_BAND_BOT
#define AWAIT_GLYPH_H     96
#define AWAIT_OPTS_SHOWN   3   /* option rows on screen; rest fold into "+N" */
#define AWAIT_OPT_ROW_H   (ui_type_line(UI_T_BODY) + UI_GAP_XS)

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
    /* LV_SYMBOL_* glyphs live in the built-in Montserrat private-use
     * range only — this is an icon, not text, so it sits outside the
     * ui_type scale. 48 is the largest built-in size. */
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_48, 0);
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
    /* Prefer a human-readable name: the project = last path segment of cwd
     * (e.g. "esp32-agent-dashboard"). The raw session id ("cc 135228" /
     * UUID) is meaningless to a person, so it's only the fallback when cwd
     * is unknown. */
    const char *base = anchor->cwd;
    for (const char *p = anchor->cwd; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    if (base && base[0]) {
        char basetrunc[27];   /* UTF-8-safe: never split a CJK folder name */
        cjk_utf8_lcpy(basetrunc, base, sizeof(basetrunc));
        snprintf(chip, sizeof(chip), "%s  %s", short_kind, basetrunc);
    } else {
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
    }
    lv_label_set_text(s_ui.agent_chip, chip);

    /* Decide layout mode for this frame + compute the dynamic
     * vertical-center offset so the visible stack sits balanced
     * regardless of how many options / context lines exist. */
    bool has_summary = (anchor->awaiting_summary[0] != '\0');
    bool has_options = (anchor->awaiting_options_count > 0);
    int n_opts = anchor->awaiting_options_count;
    if (n_opts > AGENT_AWAITING_OPTIONS_MAX) n_opts = AGENT_AWAITING_OPTIONS_MAX;
    int n_show = (n_opts > AWAIT_OPTS_SHOWN) ? AWAIT_OPTS_SHOWN : n_opts;
    int n_more = n_opts - n_show;
    int n_ctx = anchor->awaiting_context_count;
    if (n_ctx > AGENT_AWAITING_CONTEXT_LINES) n_ctx = AGENT_AWAITING_CONTEXT_LINES;

    bool show_affordance = (kind == AWAITING_APPROVE);
    /* Minimal turns ("your turn", nothing else) keep the breathing
     * glyph + HERO headline readable at 1 m. Any real content drops the
     * glyph and steps the headline down to TITLE — the content is why
     * the user leans in. */
    bool minimal = !has_summary && !has_options && n_ctx == 0;
    bool show_glyph = minimal;
    /* 3+ option rows leave room for only one summary line; 0-2 get two. */
    int summary_lines = (n_show >= 3) ? 1 : 2;

    int head_h = ui_type_line(minimal ? UI_T_HERO : UI_T_TITLE);
    int chip_h = ui_type_line(UI_T_LABEL);
    int body_h = ui_type_line(UI_T_BODY);
    int aff_h  = ui_type_line(UI_T_LABEL);
    int more_h = ui_type_line(UI_T_CAPTION);

    int big = minimal ? 1 : 0;
    if (big != s_ui.headline_big) {
        s_ui.headline_big = big;
        lv_obj_set_style_text_font(s_ui.headline,
            ui_type_bold(big ? UI_T_HERO : UI_T_TITLE), 0);
    }

    int content_h = head_h + UI_GAP_XS + chip_h;
    if (show_glyph)               content_h += AWAIT_GLYPH_H + UI_GAP_MD;
    if (show_affordance)          content_h += UI_GAP_SM + aff_h;
    if (has_summary)              content_h += UI_GAP_SM + summary_lines * body_h;
    if (has_options) {
        content_h += UI_GAP_SM + n_show * AWAIT_OPT_ROW_H;
        if (n_more > 0) content_h += more_h;
    } else if (!has_summary && n_ctx > 0) {
        content_h += UI_GAP_SM + n_ctx * body_h;
    }

    int avail_h = AWAIT_BOT - AWAIT_TOP;
    int top_pad = (avail_h - content_h) / 2;
    if (top_pad < 0) top_pad = 0;
    int y = AWAIT_TOP + top_pad;

    /* Re-align the stack at the new y. */
    if (show_glyph) {
        lv_obj_align(s_ui.glyph, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(s_ui.glyph, LV_OBJ_FLAG_HIDDEN);
        y += AWAIT_GLYPH_H + UI_GAP_MD;
    } else {
        lv_obj_add_flag(s_ui.glyph, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_ui.headline,   LV_ALIGN_TOP_MID, 0, y);
    y += head_h + UI_GAP_XS;
    lv_obj_align(s_ui.agent_chip, LV_ALIGN_TOP_MID, 0, y);
    y += chip_h;

    if (show_affordance) {
        y += UI_GAP_SM;
        lv_obj_align(s_ui.affordance, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(s_ui.affordance, LV_OBJ_FLAG_HIDDEN);
        y += aff_h;
    } else {
        lv_obj_add_flag(s_ui.affordance, LV_OBJ_FLAG_HIDDEN);
    }

    if (has_summary) {
        y += UI_GAP_SM;
        lv_obj_set_size(s_ui.summary_lbl, UI_CONTENT_W,
                        summary_lines * body_h);
        lv_obj_align(s_ui.summary_lbl, LV_ALIGN_TOP_MID, 0, y);
        y += summary_lines * body_h;
    }
    if (has_options) {
        y += UI_GAP_SM;
        int ox = (SCREEN_W - UI_CONTENT_W) / 2;
        for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
            lv_obj_align(s_ui.option_rows[i], LV_ALIGN_TOP_LEFT, ox, y);
            if (i < n_show) y += AWAIT_OPT_ROW_H;
        }
        lv_obj_align(s_ui.more_lbl, LV_ALIGN_TOP_MID, 0, y);
    } else if (!has_summary) {
        y += UI_GAP_SM;
        for (int i = 0; i < AGENT_AWAITING_CONTEXT_LINES; ++i) {
            lv_obj_align(s_ui.ctx_lines[i], LV_ALIGN_TOP_MID, 0, y);
            if (i < n_ctx) y += body_h;
        }
    }

    if (has_summary) {
        /* Static wrapped text (v4.4) — the old scroll-circular marquee
         * re-laid the FULL string out every frame and was the device-
         * freeze root cause (task-watchdog starvation, bisected
         * 2026-07-05); it is also poor ergonomics — reading moving text
         * at 0.6-1 m is ~3x slower than static. Two BODY lines with
         * DOT truncation show everything a glanceable panel should. */
        static char s_last_summary[AGENT_AWAITING_SUMMARY_MAX];
        static bool s_have_last = false;
        if (!s_have_last
            || strncmp(s_last_summary, anchor->awaiting_summary,
                       sizeof(s_last_summary)) != 0) {
            char capped[120];   /* 2 wrapped lines ≈ 22 hanzi; DOT handles the rest */
            cjk_utf8_lcpy(capped, anchor->awaiting_summary, sizeof(capped));
            lv_label_set_text(s_ui.summary_lbl, capped);
            strncpy(s_last_summary, anchor->awaiting_summary,
                    sizeof(s_last_summary) - 1);
            s_last_summary[sizeof(s_last_summary) - 1] = '\0';
            s_have_last = true;
        }
        lv_obj_clear_flag(s_ui.summary_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.summary_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    if (has_options) {
        for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
            if (i < n_show && anchor->awaiting_options[i][0] != '\0') {
                /* Compose with a UTF-8-safe copy — a byte-truncating
                 * snprintf can split a hanzi and leave garbage bytes. */
                char row[96];
                int p = snprintf(row, sizeof(row), "%d  ", i + 1);
                if (p > 0 && (size_t)p < sizeof(row)) {
                    cjk_utf8_lcpy(row + p, anchor->awaiting_options[i],
                                  (unsigned)(sizeof(row) - (size_t)p));
                }
                lv_label_set_text(s_ui.option_rows[i], row);
                lv_obj_clear_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (n_more > 0) {
            char more[24];
            snprintf(more, sizeof(more), "+%d 更多", n_more);
            lv_label_set_text(s_ui.more_lbl, more);
            lv_obj_clear_flag(s_ui.more_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.more_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
            lv_obj_add_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_ui.more_lbl, LV_OBJ_FLAG_HIDDEN);
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

    /* Shared status bar (top time + bottom active/tokens) replaces the old
     * eyebrow + "waiting Xs" footer — same header/footer as every other scene. */
    status_bar_update(&s_ui.sb, st);

    agent_state_unlock();
}

/* ── Scene framework hooks ───────────────────────────────────────── */

static lv_timer_t *s_tick_timer;

static void init(scene_t *s, lv_obj_t *parent)
{
    s->container = parent;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.last_rendered_kind = AWAITING_NONE;
    s_ui.headline_big = -1;   /* force first tick to apply a headline font */
    lv_obj_t *root = parent;
    /* Background */
    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(root, lv_color_hex(pal ? pal->bg : 0x0B0A09), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* Shared status bar. The top clock is hidden (v4.4): a takeover
     * asking for input doesn't need the time, and dropping it frees
     * ~76 px of band for full-size text. Footer + conn pill stay. */
    status_bar_create(root, &s_ui.sb);
    lv_obj_add_flag(s_ui.sb.time_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Glyph container — initial position; tick() re-aligns per-frame. */
    s_ui.glyph = lv_obj_create(root);
    lv_obj_set_size(s_ui.glyph, AWAIT_GLYPH_H, AWAIT_GLYPH_H);
    lv_obj_set_style_bg_opa(s_ui.glyph, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.glyph, 0, 0);
    lv_obj_set_style_pad_all(s_ui.glyph, 0, 0);
    lv_obj_clear_flag(s_ui.glyph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_ui.glyph, LV_ALIGN_TOP_MID, 0, AWAIT_TOP);

    /* Headline — tick() picks HERO or TITLE per mode and re-aligns. */
    s_ui.headline = lv_label_create(root);
    lv_obj_set_style_text_color(s_ui.headline, lv_color_hex(0xF3EEE2), 0);
    lv_obj_set_style_text_font(s_ui.headline, ui_type_bold(UI_T_TITLE), 0);
    lv_label_set_text(s_ui.headline, "");
    lv_obj_align(s_ui.headline, LV_ALIGN_TOP_MID, 0, AWAIT_TOP + 110);

    /* Agent chip (project name) — LABEL tier metadata under the
     * headline; CJK folder names come via the fallback chain. */
    s_ui.agent_chip = lv_label_create(root);
    lv_obj_set_style_text_color(s_ui.agent_chip, lv_color_hex(0x2BB3B1), 0);
    lv_obj_set_style_text_font(s_ui.agent_chip, ui_type(UI_T_LABEL), 0);
    lv_label_set_text(s_ui.agent_chip, "");
    lv_obj_align(s_ui.agent_chip, LV_ALIGN_TOP_MID, 0, AWAIT_TOP + 180);

    /* Affordance hint for approve kind — tells user which physical
     * buttons map to approve / deny. Hidden until kind == APPROVE. */
    s_ui.affordance = lv_label_create(root);
    lv_obj_set_style_text_color(s_ui.affordance, lv_color_hex(0x8A807A), 0);
    lv_obj_set_style_text_font(s_ui.affordance, ui_type(UI_T_LABEL), 0);
    lv_label_set_text(s_ui.affordance, "BOOT approve  \xC2\xB7  USER deny");
    lv_obj_add_flag(s_ui.affordance, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_ui.affordance, LV_ALIGN_TOP_MID, 0, 0);

    /* Context lines (used when no dash-state summary). */
    for (int i = 0; i < AGENT_AWAITING_CONTEXT_LINES; ++i) {
        s_ui.ctx_lines[i] = lv_label_create(root);
        lv_obj_set_size(s_ui.ctx_lines[i], UI_CONTENT_W,
                        ui_type_line(UI_T_BODY));
        lv_obj_set_style_text_align(s_ui.ctx_lines[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_ui.ctx_lines[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(s_ui.ctx_lines[i], lv_color_hex(0x8A807A), 0);
        lv_obj_set_style_text_font(s_ui.ctx_lines[i], ui_type(UI_T_BODY), 0);
        lv_label_set_text(s_ui.ctx_lines[i], "");
        lv_obj_add_flag(s_ui.ctx_lines[i], LV_OBJ_FLAG_HIDDEN);
        /* Initial position; tick() re-aligns. */
        lv_obj_align(s_ui.ctx_lines[i], LV_ALIGN_TOP_MID, 0, AWAIT_TOP + 220);
    }

    /* Summary — static wrapped BODY text, 1-2 lines, DOT-truncated.
     * (The scroll-circular marquee is gone; see the tick() comment.) */
    s_ui.summary_lbl = lv_label_create(root);
    lv_obj_set_size(s_ui.summary_lbl, UI_CONTENT_W,
                    2 * ui_type_line(UI_T_BODY));
    lv_label_set_long_mode(s_ui.summary_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_ui.summary_lbl, lv_color_hex(0xF3EEE2), 0);
    lv_obj_set_style_text_font(s_ui.summary_lbl, ui_type(UI_T_BODY), 0);
    lv_obj_set_style_text_align(s_ui.summary_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ui.summary_lbl, "");
    lv_obj_add_flag(s_ui.summary_lbl, LV_OBJ_FLAG_HIDDEN);
    /* Initial position; tick() re-aligns. */
    lv_obj_align(s_ui.summary_lbl, LV_ALIGN_TOP_MID, 0, AWAIT_TOP + 220);

    /* Numbered options — up to AWAIT_OPTS_SHOWN rows "N  <option>";
     * the remainder folds into the "+N 更多" caption. User reads,
     * switches to terminal, types the digit. */
    for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
        s_ui.option_rows[i] = lv_label_create(root);
        lv_obj_set_size(s_ui.option_rows[i], UI_CONTENT_W,
                        ui_type_line(UI_T_BODY));
        lv_label_set_long_mode(s_ui.option_rows[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(s_ui.option_rows[i], lv_color_hex(0xF3EEE2), 0);
        lv_obj_set_style_text_font(s_ui.option_rows[i], ui_type(UI_T_BODY), 0);
        lv_obj_set_style_text_align(s_ui.option_rows[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(s_ui.option_rows[i], "");
        lv_obj_add_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_ui.more_lbl = lv_label_create(root);
    lv_obj_set_style_text_color(s_ui.more_lbl, lv_color_hex(0x8A807A), 0);
    lv_obj_set_style_text_font(s_ui.more_lbl, ui_type(UI_T_CAPTION), 0);
    lv_label_set_text(s_ui.more_lbl, "");
    lv_obj_add_flag(s_ui.more_lbl, LV_OBJ_FLAG_HIDDEN);

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
