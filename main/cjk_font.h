#pragma once
#include "lvgl.h"

/* On-device CJK text. Renders the embedded SimHei GB2312 subset at any size via
 * lv_tiny_ttf (runtime glyph rasterization — so any char in the subset works,
 * unlike a pre-baked bitmap font). Returns a cached lv_font_t for the requested
 * pixel size, or NULL if tiny_ttf is unavailable (callers fall back to the
 * built-in Latin font). Use for labels that may contain Chinese (awaiting
 * summary / options / context); the device font (Montserrat) has no CJK glyphs. */
const lv_font_t *cjk_font(int px);
