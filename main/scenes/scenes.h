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

/* v4 manual-switch contract: the prompt takeover remembers which
 * environment scene it covered and restores it on exit — the user's
 * BOOT-cycled view (overview/clock) must survive a prompt, so no exit
 * path may hardcode "back to dashboard".
 *
 * note_origin: call BEFORE switching to the prompt scene (the `dash
 * prompt` / snapshot prompt_set handlers). No-op when the current scene
 * is already a takeover (prompt/awaiting).
 * return_home: switch back to the noted origin (default scene when none
 * was noted) and forget it. Replaces every prompt-exit switch.
 *
 * Both mutate the scene registry — call under the display lock or from
 * the LVGL task. */
void scene_prompt_note_origin(void);
void scene_prompt_return_home(void);

#ifdef __cplusplus
}
#endif
