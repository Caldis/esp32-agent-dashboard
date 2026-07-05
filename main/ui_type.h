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

/* Content column: 466-px panel minus a 28-px margin each side. Labels
 * that wrap or truncate size themselves to this. */
#define UI_CONTENT_W   410
#define UI_MARGIN       28

/* ── shared vertical anchors (the scene "safe band") ─────────────────
 * Scenes that show the status bar lay their content between these:
 * below the 48-px top clock, above the footer numbers. */
#define UI_BAND_TOP    124
#define UI_BAND_BOT    386
