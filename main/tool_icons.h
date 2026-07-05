/*
 * tool_icons — map a canonical tool name ("Bash", "Edit", ...) to a
 * small LVGL symbol that scenes can render in front of an entry row.
 *
 * Why a registry instead of an inline switch in each scene: the same
 * mapping is needed by the dashboard scene and the prompt scene's
 * badge. Centralising the table here means a new tool icon ships in
 * exactly one place.
 *
 * Symbols are LVGL FontAwesome glyphs already linked by the LVGL build
 * (lv_conf has the standard symbol font enabled). They render at any
 * font size — scenes typically use a 14 pt label.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a NUL-terminated UTF-8 string containing the LVGL symbol
 * glyph for `tool`. Always returns a non-NULL pointer; falls back to a
 * question-mark glyph for unknown tools. */
const char *tool_icon_for(const char *tool);

#ifdef __cplusplus
}
#endif
