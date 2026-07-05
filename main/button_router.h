/*
 * button_router — one place that decides what a physical key does.
 *
 * The three keys are OPTIONAL mode switches, never required input: the
 * device stays fully host-driven if nobody ever touches them. Semantics:
 *
 *                ambient (no prompt)            permission prompt up
 *   BOOT         cycle view (dashboard/idle)    approve once
 *   USER (Key3)  cycle focused agent            deny
 *   PWR          screen off / back on           (ignored)
 *
 * Any key wakes a dark screen and is CONSUMED by the wake — pressing
 * BOOT while the panel is black never approves something you couldn't
 * see. A prompt or an AWAITING takeover auto-wakes the screen (the
 * auto-switch timer calls button_router_screen_wake), so screen-off
 * can't hide input the agent is blocked on.
 *
 * Handlers run on the iot_button / pwr_key poll tasks; every LVGL
 * mutation goes through bsp_display_lock (recursive), the same pattern
 * scene_prompt already uses.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROUTER_KEY_BOOT = 0,
    ROUTER_KEY_USER,
    ROUTER_KEY_PWR,
} button_router_key_t;

/* Bind BOOT/USER (buttons.h) + PWR (pwr_key.h) to the router. Call once
 * after buttons_init(); safe to call even if pwr_key_init failed. */
void button_router_init(void);

/* Shared press path — physical handlers and the `dash btn` simulation
 * command both land here, so tests exercise the real routing logic. */
void button_router_press(button_router_key_t key);

/* Screen-off state, exposed for the auto-wake check in main's
 * scene_auto_switch_cb. Wake restores the pre-off brightness. */
bool button_router_screen_is_off(void);
void button_router_screen_wake(void);

#ifdef __cplusplus
}
#endif
