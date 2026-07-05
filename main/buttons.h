/*
 * buttons — thin façade over iot_button for BOOT (GPIO 0) + USER (GPIO 18).
 *
 * button_router.c owns what a press MEANS (view / focus / prompt
 * decisions); this module only owns the GPIO mechanics. The board's
 * third key (PWR) is not a GPIO — see pwr_key.c. We don't reuse the
 * aurora example's `keys` peripheral here because that lives in the
 * example tree, not in the shared aurora-harness component. Wrapping
 * iot_button keeps this project self-contained.
 *
 * Each button supports exactly one single-click handler; buttons_set_handler
 * replaces the prior one. Handlers fire on the iot_button task and are
 * required to do nothing that needs the LVGL mutex synchronously (use
 * lv_async_call or scene_fw_show, which is documented as task-safe).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_BOOT = 0,
    BUTTON_USER,
    BUTTON_COUNT
} button_id_t;

typedef void (*button_press_cb_t)(void *handle, void *usr_data);

/* Initialise both buttons; safe to call once at boot. Returns true if
 * both were registered. */
bool buttons_init(void);

/* Replace the single-click handler for one button. cb may be NULL to
 * detach. usr_data is opaque. */
void buttons_set_handler(button_id_t which, button_press_cb_t cb, void *usr_data);

#ifdef __cplusplus
}
#endif
