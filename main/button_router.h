/*
 * button_router — one place that decides what a physical key does.
 *
 * The three keys are OPTIONAL mode switches, never required input: the
 * device stays fully host-driven if nobody ever touches them. Semantics
 * (v4.3):
 *
 *                ambient (no prompt)             permission prompt up
 *   BOOT         toggle view (dashboard/idle)    approve once
 *   USER (Key3)  cycle focused agent             deny
 *   PWR          lock to / unlock from clock     (ignored)
 *
 * PWR's clock lock is sticky: unlike the idle screensaver (which yields
 * to fresh agent activity), a locked clock stays until PWR again (back
 * to the covered view) or BOOT (into the ambient pair). Takeovers still
 * outrank it — a prompt/awaiting will grab the panel and restore it.
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

/* Legacy screen-off API — inert since v4.3 (PWR locks to the clock
 * instead of darkening the panel). is_off always returns false; wake is
 * a no-op. Kept so scene_auto_switch_cb and the wasm shim stay stable. */
bool button_router_screen_is_off(void);
void button_router_screen_wake(void);

#ifdef __cplusplus
}
#endif
