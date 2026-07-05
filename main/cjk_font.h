#pragma once
#include "lvgl.h"

/* On-device CJK text. Renders the embedded SimHei GB2312 subset at any size via
 * lv_tiny_ttf (runtime glyph rasterization — so any char in the subset works,
 * unlike a pre-baked bitmap font). Returns a cached lv_font_t for the requested
 * pixel size, or NULL if tiny_ttf is unavailable (callers fall back to the
 * built-in Latin font). Use for labels that may contain Chinese (awaiting
 * summary / options / context); the device font (Montserrat) has no CJK glyphs. */
const lv_font_t *cjk_font(int px);

/* UI text font (v4.2): Consolas subset (ASCII + ·×°—…) rendered via
 * tiny_ttf, with the SimHei CJK subset chained as the LVGL fallback —
 * Latin/symbols get the terminal look, Chinese falls through to zh.ttf
 * at the same pixel size. _bold uses Consolas Bold (numbers, verbs,
 * headlines). Returns NULL when tiny_ttf is unavailable; use the _or
 * helpers to fall back to a built-in Montserrat. */
const lv_font_t *ui_font(int px);
const lv_font_t *ui_font_bold(int px);

static inline const lv_font_t *ui_font_or(int px, const lv_font_t *fb)
{
    const lv_font_t *f = ui_font(px);
    return f ? f : fb;
}

static inline const lv_font_t *ui_font_bold_or(int px, const lv_font_t *fb)
{
    const lv_font_t *f = ui_font_bold(px);
    return f ? f : fb;
}

/* Clock face font: M PLUS Rounded 1c Black subset to "0-9 : -" only
 * (main/clock_digits.ttf, ~2.4KB, regenerate with tools/make_clock_font.py).
 * Rounded + heavy for the StandBy look; the colon's side bearings are
 * pre-tightened in the subset. ONLY those twelve glyphs exist — anything
 * else renders empty, so keep it to status_bar_format_time() output.
 * Returns NULL when tiny_ttf is unavailable (fall back to cjk_font /
 * Montserrat). */
const lv_font_t *clock_font(int px);

/* Copy a UTF-8 string into dst[cap] WITHOUT splitting a multi-byte character at
 * the truncation boundary. A byte-level truncation (strncpy) that cuts a 3-byte
 * CJK codepoint in half leaves invalid trailing bytes that render as garbage
 * ("乱码") even in a CJK-capable font. Always NUL-terminates. Available even
 * when tiny_ttf is not built (it's pure byte handling). */
void cjk_utf8_lcpy(char *dst, const char *src, unsigned cap);
