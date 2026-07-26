/*
 * spring — implementation. See spring.h.
 *
 * LUT 由离线谐振子采样生成（33 点，Q10）：
 *   python: x(t) = 1 - e^(-6.2t)(cos(ωd t) + (6.2/ωd) sin(ωd t))
 *   ζ=0.68 → ωd=6.685（峰值 1079/1023 = +5.5% @ t≈0.47）
 *   ζ=0.92 → ωd=2.641（单调，无过冲）
 * 表值允许 >1023（过冲段），插值用 int32 直通——位移越过终点后回弹。
 */

#include "anim/spring.h"

#define NSAMPLES 33
#define Q10      1023

/* ζ=0.68 位移：过冲 + 微回弹 */
static const int16_t LUT_DISP[NSAMPLES] = {
        0,    36,   127,   248,   383,   517,   641,   751,
      844,   919,   977,  1020,  1049,  1066,  1076,  1079,
     1077,  1072,  1065,  1058,  1051,  1044,  1038,  1033,
     1029,  1026,  1023,  1022,  1021,  1020,  1020,  1020,
     1023,
};

/* ζ=0.92 透明度：近临界，单调收敛 */
static const int16_t LUT_OPA[NSAMPLES] = {
        0,    20,    70,   139,   218,   302,   385,   464,
      539,   607,   669,   724,   772,   814,   850,   881,
      907,   929,   947,   962,   975,   985,   994,  1000,
     1006,  1010,  1014,  1016,  1018,  1020,  1021,  1022,
     1023,
};

static int32_t path_with_lut(const int16_t *lut, const lv_anim_t *a)
{
    if (a->act_time >= a->duration) return a->end_value;
    if (a->act_time <= 0)           return a->start_value;
    int32_t dur = a->duration;
    if (dur <= 0) return a->end_value;
    int64_t f = (int64_t)a->act_time * (NSAMPLES - 1);
    int32_t idx = (int32_t)(f / dur);
    int32_t frac_num = (int32_t)(f - (int64_t)idx * dur);
    if (idx < 0) idx = 0;
    if (idx > NSAMPLES - 2) idx = NSAMPLES - 2;
    int32_t va = lut[idx];
    int32_t vb = lut[idx + 1];
    int32_t progress_q10 = va + (int32_t)(((int64_t)(vb - va) * frac_num) / dur);
    int32_t span = a->end_value - a->start_value;
    return a->start_value + (int32_t)(((int64_t)span * progress_q10) / Q10);
}

int32_t spring_disp(const lv_anim_t *a) { return path_with_lut(LUT_DISP, a); }
int32_t spring_opa(const lv_anim_t *a)  { return path_with_lut(LUT_OPA,  a); }
