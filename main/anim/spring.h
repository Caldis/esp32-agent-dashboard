#pragma once
#include "lvgl.h"

/*
 * spring — 真·物理弹簧缓动 (v5.0 转场系统)。
 *
 * apple_ease 的 "spring" 是 cubic-bezier 拟合的近似；这里换成真正的
 * 欠阻尼谐振子采样：
 *
 *   x(t) = 1 - e^(-ζωt) · ( cos(ω_d t) + (ζω/ω_d)·sin(ω_d t) )
 *   ω_d  = ω·√(1-ζ²)，ζω = 6.2（t=1 时残差 < 0.2%）
 *
 * 对应 SwiftUI .spring(dampingFraction:) 的手感：
 *
 *   spring_disp — ζ=0.68：位移曲线。一次 ~5.5% 过冲 + 微回弹，用于
 *                 y/x 入场滑动（值会越过终点，位移语义下合法）。
 *   spring_opa  — ζ=0.92：透明度曲线。近临界阻尼，大初速注入后收敛、
 *                 永不过冲（opa 超过 255 是非法值，物理上"发光度"
 *                 也不该反弹）。也适合任何不允许过冲的收敛属性。
 *
 * 出场不用弹簧——离场是"被抛出"，用 apple_ease_in 加速即可。
 */
int32_t spring_disp(const lv_anim_t *a);
int32_t spring_opa(const lv_anim_t *a);
