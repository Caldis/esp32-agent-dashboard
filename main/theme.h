/*
 * theme — palette + per-agent-kind accent registry for v1.
 *
 * v0 hard-coded a single accent per scene. v1 needs:
 *   • two agents side-by-side, each with its own accent so the eye can
 *     tell them apart at a glance;
 *   • three named palettes (noir / lab / mono) selectable at runtime via
 *     `dash config '{"theme":"..."}'`;
 *   • a stable colour lookup keyed by `agent_kind` string so scenes can
 *     ask "what colour for claude-code?" without knowing the palette.
 *
 * The palette table lives in .rodata; switching themes flips one global
 * pointer. No allocation. No mutex needed for reads — the pointer write
 * on theme change is atomic-enough on Xtensa for our use (worst case the
 * next frame paints with the old colours).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THEME_NOIR = 0,    /* default: dark bg, rust + teal accents */
    THEME_LAB,         /* light clinical (inverse polarity) */
    THEME_MONO,        /* one hue, opacity tiers */
    THEME_COUNT
} theme_id_t;

typedef struct {
    theme_id_t  id;
    const char *name;
    uint32_t    bg;             /* screen background */
    uint32_t    surface;        /* card / pane background */
    uint32_t    text;           /* primary text */
    uint32_t    text_dim;       /* secondary text */
    uint32_t    accent_claude;  /* claude-code agent accent */
    uint32_t    accent_codex;   /* codex agent accent */
    uint32_t    accent_other;   /* fallback agent accent */
    uint32_t    warning;        /* yellow-ish */
    uint32_t    danger;         /* red-ish */
    uint32_t    success;        /* green-ish */
} theme_palette_t;

/* Init defaults to NOIR. Idempotent. */
void theme_init(void);

/* Switch the active theme. Returns false if the name is unknown. */
bool theme_set_by_name(const char *name);

/* Current palette pointer — stable until next theme_set. */
const theme_palette_t *theme_current(void);

/* Convert one of the theme's hex32 values to an lv_color_t. */
static inline lv_color_t theme_hex(uint32_t hex)
{
    return lv_color_hex(hex);
}

/* Look up an agent kind's accent colour from the current palette. */
uint32_t theme_accent_for_kind(const char *kind);

/* Name of the current theme — useful for `dash health` reply. */
const char *theme_current_name(void);

#ifdef __cplusplus
}
#endif
