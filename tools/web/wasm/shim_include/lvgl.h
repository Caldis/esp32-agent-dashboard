#pragma once
#include <stdint.h>
/* 最小 LVGL 替身:数据层只用到 lv_color_t / lv_color_hex(被 theme.h 的
 * static inline 引用但数据层不调用)与 lv_tick_get。 */
typedef struct { uint16_t full; } lv_color_t;
typedef void lv_obj_t;
static inline lv_color_t lv_color_hex(uint32_t hex) { (void)hex; lv_color_t c = {0}; return c; }
uint32_t lv_tick_get(void);   /* 由 wasm_shim.c 实现 */
