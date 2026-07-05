#pragma once
#include "harness/scene_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

extern scene_t scene_dashboard;
extern scene_t scene_idle;
extern scene_t scene_prompt;
extern scene_t scene_awaiting;   /* v2.3.0 takeover */

/* Resolve the active permission prompt from outside the scene (the
 * button router's BOOT=approve / USER=deny path). decision is "once" or
 * "deny"; no-op when no prompt is active. Task-safe: takes the display
 * lock internally. */
void scene_prompt_decide(const char *decision);

#ifdef __cplusplus
}
#endif
