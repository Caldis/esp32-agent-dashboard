#pragma once
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
