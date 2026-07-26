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
    int         headline_big;   /* 1 = HERO applied */
    uint32_t    breath_anim_armed;
    /* v5.6 dirty-check caches — the tick re-applies EVERYTHING every
     * 500 ms (unconditional convergence); these only suppress redundant
     * invalidations, they never gate whether a field gets corrected. */
    char        cached_headline[64];
    char        cached_chip[64];
} await_ui_t;

static await_ui_t s_ui;

/* ── Headline text per kind ──────────────────────────────────────── */

/* Chinese primary words (v4.4): 3 hanzi at HERO tier subtend 22' at
 * 1 m — the takeover signal must read across the desk, which no
 * 9-12-char English headline can do on a 38.8 mm panel. All chars are
 * in the GB2312 subset. */
static const char *headline_for(const agent_slot_t *a)
{
    switch (a->awaiting_kind) {
        /* CONTINUE rotates through the "your turn" greetings; the index was
         * rolled in agent_state_set_awaiting, so it's stable per takeover. */
        case AWAITING_CONTINUE: return agent_awaiting_greeting(a->awaiting_greeting_idx);
        case AWAITING_APPROVE:  return "请批准";
        case AWAITING_PICK:     return "请选择";
        case AWAITING_TYPE:     return "请输入";
        case AWAITING_CLARIFY:  return "需澄清";
        default:                return "";
    }
}

/* (v5.4: the per-kind "urgency" colour split is gone — it made the
 * takeover show a TEAL ring for "your turn" while the dashboard showed
 * the same waiting agent in GOLD, so keying between the two pages
 * looked like a state change that never happened. Colour now follows
 * STATE device-wide: gold = your move, teal = thinking, dim = idle.
 * A takeover on screen is by definition "your move" → always gold.) */

/* ── Layout constants ────────────────────────────────────────────── */

#define SCREEN_W  466
#define SCREEN_H  466

/* v4.4 layout, v5.1 revision: the top clock is BACK — the v5.0
 * transition contract makes the time a fixed anchor that never leaves
 * the screen, and this takeover was the one scene without it. Content
 * owns the band from AWAIT_TOP (below the top-clock chrome, ink ends
 * ~112) to UI_BAND_BOT (above the footer numbers). Two presentation
 * modes:
 *
 *   minimal (no summary/options/ctx) → glyph + HERO headline + chip;
 *   content                          → TITLE headline + chip + wrapped
 *                                      summary + up to 3 option rows
 *                                      (+ "+N" overflow caption).
 *
 * The visible stack is vertically centered per-frame, same as v2.4.0.
 * All heights come from ui_type_line() — no free-hand pixel sizes. */
#define AWAIT_TOP         116
/* v5.9: the shared footer (active/tokens) is BACK — parity with
 * dashboard (user call): every scene wears the same chrome, so the
 * takeover can't read as a stripped-down different device state (v5.7
 * had hidden it for breathing room). The band ends 4px above the
 * footer chrome zone, mirroring the 4px top margin under the clock
 * ink (112→116). The stack pays: glyph container 96→80 (ring ink is
 * 72, still 4px slack) and tighter gaps — the HERO headline is
 * untouchable (v5.5 pose contract).
 * Stack 80+24+106+14+32 = 256 ≤ band 262 → top_pad 3. */
#define AWAIT_BOT         (UI_CHROME_BOT - 4)   /* 378 */
#define AWAIT_GAP_GLYPH   24   /* ring → headline */
#define AWAIT_GAP_CHIP    14   /* headline → chip */
#define AWAIT_GLYPH_H     80   /* container; ring ink is 72 */
/* v5.2: 2 rows (was 3) — the top clock reclaimed 68px of the band, and
 * three option rows + summary now overflow into the footer. The panel
 * is a glanceable pager; full option lists live in the terminal. */
#define AWAIT_OPTS_SHOWN   2   /* option rows on screen; rest fold into "+N" */
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

/* v5.2 单一视觉语言：所有 awaiting kind 共用同一个呼吸圆环 glyph
 * （用户反馈：警告三角/列表/编辑/铃铛五种 Montserrat 图标变体 + 双布局
 * 让人分不清自己在看什么）。差异只留两个低噪声通道：headline 文字
 * （该你发挥/请选择/请输入/需澄清）和紧急度颜色（gold/teal）。 */
static void render_glyph(awaiting_kind_t k, uint32_t color)
{
    (void)k;
    if (s_ui.glyph == NULL) return;
    clear_children(s_ui.glyph);
    s_ui.glyph_inner_dot = NULL;
    glyph_pulse(s_ui.glyph, color);
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
    /* v5.4 device-wide state colour: a visible takeover IS "your move"
     * — always gold (#B89020), matching the dashboard's pulse ring for
     * the same waiting agent. */
    uint32_t color = 0xB89020;
    bool motion_ok = !st->motion_reduced;

    /* v5.6 UNCONDITIONAL CONVERGENCE. The old "rebuild only when kind or
     * session changed" incremental path let historical widget states
     * leak through as phantom looks (the user's "gold text, no ring":
     * a same-kind same-session re-takeover skipped render_glyph AND the
     * headline refresh, so whatever the widgets last held stayed up).
     * Now every tick drives every element to the ONE canonical pose —
     * ring present, gold, breathing; current greeting; gold chip —
     * dirty-checked only to avoid redundant redraws, never to skip a
     * correction. Whatever state history left behind, the page self-
     * heals within 500 ms. */
    if (lv_obj_get_child_count(s_ui.glyph) == 0) {
        render_glyph(kind, color);
        s_ui.breath_anim_armed = 0;
    }
    if (motion_ok && !s_ui.breath_anim_armed) arm_breath();

    const char *head = headline_for(anchor);
    if (strncmp(head, s_ui.cached_headline, sizeof(s_ui.cached_headline)) != 0) {
        snprintf(s_ui.cached_headline, sizeof(s_ui.cached_headline), "%s", head);
        lv_label_set_text(s_ui.headline, head);
    }
    lv_obj_set_style_text_color(s_ui.agent_chip, lv_color_hex(color), 0);

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
    if (strncmp(chip, s_ui.cached_chip, sizeof(s_ui.cached_chip)) != 0) {
        snprintf(s_ui.cached_chip, sizeof(s_ui.cached_chip), "%s", chip);
        lv_label_set_text(s_ui.agent_chip, chip);
    }

    /* Decide layout mode for this frame + compute the dynamic
     * vertical-center offset so the visible stack sits balanced
     * regardless of how many options / context lines exist. */
    /* v5.5: ONE pose, always — glyph + headline + chip. The panel's
     * single job is "your move"; summaries / option lists / context
     * lines live in the terminal. The old content mode (no glyph,
     * TITLE headline, teal-initialised chip, body text) also read as a
     * SECOND look — the user saw "a blue no-dot awaiting" and took it
     * for a different state. It is gone. */
    int head_h = ui_type_line(UI_T_HERO);
    int chip_h = ui_type_line(UI_T_LABEL);

    if (s_ui.headline_big != 1) {
        s_ui.headline_big = 1;
        lv_obj_set_style_text_font(s_ui.headline, ui_type_bold(UI_T_HERO), 0);
    }

    int content_h = AWAIT_GLYPH_H + AWAIT_GAP_GLYPH + head_h
                  + AWAIT_GAP_CHIP + chip_h;
    int avail_h = AWAIT_BOT - AWAIT_TOP;
    int top_pad = (avail_h - content_h) / 2;
    if (top_pad < 0) top_pad = 0;
    int y = AWAIT_TOP + top_pad;

    /* Re-align the fixed three-piece stack at the new y. */
    lv_obj_align(s_ui.glyph, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(s_ui.glyph, LV_OBJ_FLAG_HIDDEN);
    y += AWAIT_GLYPH_H + AWAIT_GAP_GLYPH;
    lv_obj_align(s_ui.headline,   LV_ALIGN_TOP_MID, 0, y);
    y += head_h + AWAIT_GAP_CHIP;
    lv_obj_align(s_ui.agent_chip, LV_ALIGN_TOP_MID, 0, y);

    /* Content widgets are permanently parked (kept in the tree for a
     * possible future revival, never shown). */
    lv_obj_add_flag(s_ui.summary_lbl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i)
        lv_obj_add_flag(s_ui.option_rows[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.more_lbl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < AGENT_AWAITING_CONTEXT_LINES; ++i)
        lv_obj_add_flag(s_ui.ctx_lines[i], LV_OBJ_FLAG_HIDDEN);

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
    s_ui.headline_big = -1;   /* force first tick to apply a headline font */
    lv_obj_t *root = parent;
    /* Background */
    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(root, lv_color_hex(pal ? pal->bg : 0x0B0A09), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* Shared status bar, full chrome (v5.9): top clock (time anchor) +
     * conn dot + active/tokens footer — parity with dashboard, so the
     * takeover no longer looks like a different device state. */
    status_bar_create(root, &s_ui.sb);
    /* v5.1: top clock stays visible — it sits at the consensus pose, so
     * transitions into/out of this takeover keep the time anchored. */

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
    /* v5.4 state colour: gold from birth — the tick recolours on kind
     * change only, so a teal initial would leak into the first frames. */
    lv_obj_set_style_text_color(s_ui.agent_chip, lv_color_hex(0xB89020), 0);
    lv_obj_set_style_text_font(s_ui.agent_chip, ui_type(UI_T_LABEL), 0);
    lv_label_set_text(s_ui.agent_chip, "");
    lv_obj_align(s_ui.agent_chip, LV_ALIGN_TOP_MID, 0, AWAIT_TOP + 180);

    /* (v5.2: the "BOOT approve · USER deny" affordance label is gone —
     * the prompt takeover is retired and keys never mean approve.) */

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
