#pragma once
#include "lvgl.h"

/*
 * ui_type — the app-wide deterministic type scale (v4.4).
 *
 * Every piece of on-screen text MUST take its font from one of these
 * five tiers (plus the two clock-face sizes below). Raw pixel sizes are
 * framework-internal: ui_font()/cjk_font() from cjk_font.h are consumed
 * only by ui_type.c — scenes and overlays never call them directly.
 *
 * The tiers come from viewing-distance ergonomics, not taste. Panel:
 * 2.16" 466x466 → 38.8 mm square, 305 ppi, 0.0833 mm/px. One arcminute
 * of visual angle is 2.10 px at 0.6 m and 3.49 px at 1.0 m. ISO
 * 9241-303 wants 16-22' of character height for comfortable reading;
 * hanzi need ~25% more than Latin for equal legibility (stroke
 * density). The device sits on a desk 0.6-1.0 m from the user, so:
 *
 *   HERO    88 px  hanzi 22'@1m   — THE one fact per scene, read across
 *                                   the desk without leaning in
 *   TITLE   52 px  hanzi 22'@0.6m — secondary headline, comfortable at
 *                                   normal desk distance
 *   BODY    36 px  hanzi 15'@0.6m — content: options, project names,
 *                                   summaries; legible at 0.6 m
 *   LABEL   26 px  hanzi 11'@0.6m — supporting metadata; lean-in only,
 *                                   must never carry sole-source info
 *   CAPTION 20 px                 — smallest tier; decorative or
 *                                   redundant text only
 *
 * Anything below CAPTION is invisible at desk distance and is banned.
 */
typedef enum {
    UI_T_CAPTION = 0,   /* 20 px */
    UI_T_LABEL,         /* 26 px */
    UI_T_BODY,          /* 36 px */
    UI_T_TITLE,         /* 52 px */
    UI_T_HERO,          /* 88 px */
    UI_T_COUNT
} ui_tier_t;

/* Tier → font. Never NULL: falls back to a built-in Montserrat when
 * tiny_ttf is unavailable, so call sites need no _or dance. */
const lv_font_t *ui_type(ui_tier_t t);
const lv_font_t *ui_type_bold(ui_tier_t t);

/* Tier metrics for layout math. ui_type_px is the em size;
 * ui_type_line is the vertical budget one line needs (~1.2 em, matches
 * tiny_ttf's reported line height, rounded up). */
int ui_type_px(ui_tier_t t);
int ui_type_line(ui_tier_t t);

/* ── spacing tokens ──────────────────────────────────────────────────
 * Vertical rhythm between stacked elements. Same rule as the type
 * scale: pick a token, don't invent one-off gaps. */
#define UI_GAP_XS    4   /* number ↔ its caption */
#define UI_GAP_SM    8   /* lines inside one cluster */
#define UI_GAP_MD   14   /* clusters inside one group */
#define UI_GAP_LG   24   /* groups / breathing room */

/* Content column: panel width minus a 28-px margin each side. Labels
 * that wrap or truncate size themselves to this.
 * v7.4: 410 was 466-2*28, and 466 was never the panel width (see
 * "Panel geometry" in CLAUDE.md — it is 480). The stale value made
 * every centred-by-computation element sit ~7 px left: a 410 column
 * placed at (466-410)/2=28 leaves 28 px on the left but 42 on the
 * right of a 480-wide screen. */
#define UI_CONTENT_W   424
#define UI_MARGIN       28

/* ── vertical layout model (v4.6): the safe band ────────────────────
 * Every scene shares two fixed status-bar "chrome" zones it must never
 * crowd: the top clock (+ conn pill) above, and the active/tokens
 * footer below. Measured ink bounds on this panel — the 48-px top clock
 * ends at ~112, the footer numbers begin at ~382.
 *
 * ALL scene content lives in the SAFE BAND between them and keeps
 * UI_SAFE_MARGIN of clear whitespace off each chrome zone. This is the
 * one rule that stops the UI crowding its edges: size and CENTRE every
 * row block / cluster inside [UI_SAFE_TOP, UI_SAFE_BOT]; never push
 * content past them "to fit one more line" — drop a line or step a tier
 * down instead. Generous edges are what make the layout read as calm.
 *
 *      0 ┌───────────────────────────────┐
 *        │   conn pill · top clock       │ ── UI_CHROME_TOP (112)
 *        │        · margin ·             │
 *        ├───────────────────────────────┤ ── UI_SAFE_TOP  (134)
 *        │                               │
 *        │         SCENE CONTENT         │
 *        │                               │
 *        ├───────────────────────────────┤ ── UI_SAFE_BOT  (360)
 *        │        · margin ·             │
 *        │   active / tokens footer      │ ── UI_CHROME_BOT (382)
 *    466 └───────────────────────────────┘
 */
#define UI_CHROME_TOP   112   /* bottom of the top-clock chrome zone */
#define UI_CHROME_BOT   382   /* top of the footer chrome zone */
#define UI_SAFE_MARGIN   22   /* mandated whitespace: content ↔ chrome */
#define UI_SAFE_TOP     (UI_CHROME_TOP + UI_SAFE_MARGIN)   /* 134 */
#define UI_SAFE_BOT     (UI_CHROME_BOT - UI_SAFE_MARGIN)   /* 360 */
#define UI_SAFE_H       (UI_SAFE_BOT - UI_SAFE_TOP)        /* 226 */

/* Back-compat aliases (older scenes reference these). They now resolve
 * to the safe band, so the old UI_BAND_BOT=386 — which sat BELOW the
 * footer's 382 and let the last fleet card touch it — is gone. Prefer
 * UI_SAFE_* in new code. */
#define UI_BAND_TOP    UI_SAFE_TOP
#define UI_BAND_BOT    UI_SAFE_BOT
