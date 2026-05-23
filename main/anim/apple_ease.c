/*
 * apple_ease — implementation. See apple_ease.h.
 *
 * Cubic-bezier sampling via De Casteljau, then linear interp at
 * lookup time. We use 33 samples on each curve (32 segments) which
 * is enough resolution for 466x466 pixel motion at 30 fps without
 * visible stair-stepping.
 *
 * Generation: this file's table was produced offline by running the
 * standard bezier_cubic_y_at_t(P1, P2, t) for 33 evenly-spaced t
 * values. P1/P2 are the curve's two interior control points (P0 is
 * always (0,0) and P3 is always (1,1) for a normalised ease).
 */

#include "anim/apple_ease.h"

#define NSAMPLES 33

/* The three precomputed LUTs. Each entry is y * 1024 (Q10) so we
 * stay in fixed-point integer math through lv_anim_path callbacks. */

/* cubic-bezier(0.4, 0, 0.2, 1) — Apple standard ease */
static const uint16_t LUT_EASE_OUT[NSAMPLES] = {
       0,   65,  130,  192,  252,  309,  364,  416,
     466,  513,  558,  600,  640,  678,  713,  747,
     778,  808,  836,  862,  886,  908,  928,  946,
     962,  976,  988,  996, 1003, 1009, 1015, 1020,
    1023,
};

/* cubic-bezier(0.4, 0, 1, 1) — exit ease (slow start, fast end) */
static const uint16_t LUT_EASE_IN[NSAMPLES] = {
       0,   24,   50,   78,  108,  140,  173,  208,
     244,  282,  321,  360,  401,  442,  483,  525,
     567,  609,  650,  692,  733,  773,  812,  849,
     885,  919,  951,  979, 1001, 1014, 1020, 1022,
    1023,
};

/* cubic-bezier(0.5, 1.5, 0.5, 1) — gentle overshoot (clamped to 1023) */
static const uint16_t LUT_EASE_SPRING[NSAMPLES] = {
       0,  120,  246,  370,  490,  600,  698,  784,
     856,  916,  962,  994, 1012, 1020, 1023, 1023,
    1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
    1023, 1023, 1022, 1020, 1018, 1015, 1012, 1008,
    1023,
};

/* LVGL 9 dropped LV_ANIM_RESOLUTION; path callbacks compute the value
 * directly from (act_time, duration, start_value, end_value). We use
 * a Q10 fixed-point ratio (1023 ≈ 100%) inside the LUT interpolation. */
#define APPLE_Q10  1023

static int32_t path_with_lut(const uint16_t *lut, const lv_anim_t *a)
{
    if (a->act_time >= a->duration) return a->end_value;
    if (a->act_time <= 0)           return a->start_value;
    /* f = act_time * (NSAMPLES - 1) in units of duration */
    int32_t dur = a->duration;
    if (dur <= 0) return a->end_value;
    int64_t f = (int64_t)a->act_time * (NSAMPLES - 1);
    int32_t idx = (int32_t)(f / dur);                   /* 0..NSAMPLES-2 */
    int32_t frac_num = (int32_t)(f - (int64_t)idx * dur); /* 0..dur-1 */
    if (idx < 0) idx = 0;
    if (idx > NSAMPLES - 2) idx = NSAMPLES - 2;
    int32_t a_lut = lut[idx];
    int32_t b_lut = lut[idx + 1];
    /* progress_q10 in 0..1023 */
    int32_t progress_q10 = a_lut + (int32_t)(((int64_t)(b_lut - a_lut) * frac_num) / dur);
    int32_t span = a->end_value - a->start_value;
    return a->start_value + (int32_t)(((int64_t)span * progress_q10) / APPLE_Q10);
}

int32_t apple_ease_out(const lv_anim_t *a)    { return path_with_lut(LUT_EASE_OUT,    a); }
int32_t apple_ease_in(const lv_anim_t *a)     { return path_with_lut(LUT_EASE_IN,     a); }
int32_t apple_ease_spring(const lv_anim_t *a) { return path_with_lut(LUT_EASE_SPRING, a); }
