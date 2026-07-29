#pragma once
#include <stddef.h>
#include "harness/scene_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

extern scene_t scene_dashboard;
/* scene_overview retired in v5.2 — source kept, no longer built. */
extern scene_t scene_weather;    /* v4.9 weather + clock combo */
extern scene_t scene_clock;      /* v4 StandBy-style big clock */
/* scene_prompt retired in v5.2 (approvals happen in the terminal) —
 * source kept at scenes/scene_prompt.c, no longer built. Its
 * scene_prompt_decide/note_origin/return_home API went with it. */
/* scene_awaiting retired in v6.0 (the dashboard gold pose absorbed the
 * takeover: greeting word + project chip in place; auto-switch pulls
 * the display to the dashboard on a fresh awaiting rising edge).
 * Source kept at scenes/scene_awaiting.c, no longer built. */

/* v4.3: consume the clock-screensaver state (implemented in
 * esp32_agent_dashboard_main.c). Called by the button router at the
 * start of a key press: clears the saver flag atomically so the
 * auto-restore can't race the key's own scene change, and returns the
 * scene index the saver covered (-1 if the saver wasn't active). The
 * key then decides where to go (PWR adopts the clock as a manual lock;
 * BOOT cycles into the ambient pair). Takes the display lock. */
int scene_saver_consume(void);

#ifdef __cplusplus
}
#endif

/* 天气插画 accent 呼吸波形开关（默认开）。用于量化它的渲染成本：
 * `?wxbreath 0` 后测 idle render，再 `?wxbreath 1` 复测，差值即成本。 */
void scene_weather_set_breath(bool on);
bool scene_weather_get_breath(void);

/* v7.1 图标预合成开关（默认开）：矢量常驻离屏工作台，屏上是静置期烤好
 * 的 ARGB8888 位图，转场帧只剩图像 blit。`?wxcomp 0|1` A/B 用；合成
 * 失败自动翻回矢量直渲（WARN）。 */
void scene_weather_set_compose(bool on);
bool scene_weather_get_compose(void);

/* WMO 天气码 -> 中文短词。scene_clock 的常驻天气行要用同一份映射——
 * 两个界面显示同一个事实，就不该有两份翻译表。 */
const char *scene_weather_word(int code);

/* 组装时钟下方的常驻天气行："31° 多云 · 深圳·福田"。无数据时写空串。 */
void scene_weather_mini_line(char *buf, size_t cap);
