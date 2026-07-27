#pragma once

/* Line-box geometry that cjk_font applies to every tiny_ttf instance
 * (see normalise_line_box in cjk_font.c for WHY it is rebalanced).
 * Exposed only so ui_type.c can _Static_assert that its 1.2 em line
 * budget still covers the real box — the two are tuned in different
 * files and CAPTION has zero slack. */
#define CJK_LINE_ABOVE_PERMIL  880
#define CJK_LINE_BELOW_PERMIL  310
#define CJK_LINE_ABOVE_OF(px)  (((px) * CJK_LINE_ABOVE_PERMIL + 500) / 1000)
#define CJK_LINE_BELOW_OF(px)  (((px) * CJK_LINE_BELOW_PERMIL + 500) / 1000)

#include "lvgl.h"

/* ══ FRAMEWORK-INTERNAL — app code must NOT call these ═══════════════
 * The raw pixel-size font APIs below are consumed exclusively by
 * ui_type.c, which maps the app's five-tier type scale (ui_type.h)
 * onto them. Scenes, the status bar and overlays take fonts from
 * ui_type()/ui_type_bold() only — a free pixel size here would escape
 * the deterministic scale (and its viewing-distance guarantees).
 *
 * cjk_font: embedded SimHei GB2312 subset via lv_tiny_ttf (runtime
 * rasterization). ui_font/_bold: Consolas subsets with the same-size
 * CJK font chained as LVGL fallback. All return NULL when tiny_ttf is
 * unavailable. */
const lv_font_t *cjk_font(int px);
const lv_font_t *ui_font(int px);
const lv_font_t *ui_font_bold(int px);

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
