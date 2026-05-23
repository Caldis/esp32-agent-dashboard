/*
 * apple_ease — cubic-bezier(0.4, 0.0, 0.2, 1) as an LVGL animation path.
 *
 * The "standard ease" Apple uses across iOS, watchOS, and macOS for
 * UI transitions. Slow at the start, quick acceleration, gentle
 * settle. Subjectively "soft" — perfect for the AWAITING takeover
 * morph and the AMBIENT feed-row insert.
 *
 * Cubic-bezier(0.4, 0.0, 0.2, 1.0) doesn't have a closed-form analytic
 * inverse, so we precompute a 33-sample lookup table once at init
 * and linearly interpolate at lookup time. 33 samples gives < 0.5%
 * error vs the true curve and adds ~134 bytes .rodata.
 *
 * Usage::
 *
 *     #include "anim/apple_ease.h"
 *
 *     lv_anim_t a;
 *     lv_anim_init(&a);
 *     lv_anim_set_path_cb(&a, apple_ease_out);
 *     lv_anim_set_time(&a, 350);
 *     // ... configure values + start
 *
 * Three variants for different feels:
 *   - apple_ease_out      cubic-bezier(0.4, 0, 0.2, 1)  — default UI ease
 *   - apple_ease_in       cubic-bezier(0.4, 0, 1, 1)    — for exits
 *   - apple_ease_spring   cubic-bezier(0.5, 1.5, 0.5, 1) — gentle overshoot
 */

#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* lv_anim_path_cb_t signature: int32_t(*)(const lv_anim_t *) returning
 * the current progress in 0..LV_ANIM_RESOLUTION (typically 0..1023). */

/* Apple's standard ease — cubic-bezier(0.4, 0.0, 0.2, 1.0). */
int32_t apple_ease_out(const lv_anim_t *a);

/* Slightly more abrupt — cubic-bezier(0.4, 0.0, 1.0, 1.0). For exits
 * where you want fast end + soft start. */
int32_t apple_ease_in(const lv_anim_t *a);

/* Gentle overshoot — cubic-bezier(0.5, 1.5, 0.5, 1.0). For scale-in
 * effects that should "settle" with a tiny bounce. */
int32_t apple_ease_spring(const lv_anim_t *a);

#ifdef __cplusplus
}
#endif
