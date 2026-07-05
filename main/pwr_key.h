/*
 * pwr_key — PWR button events from the AXP2101 PMU.
 *
 * The board's third button (PWR) is not a GPIO: it drives the AXP2101's
 * PWRON input. The PMU latches press events into IRQ status registers
 * we read over I2C (the bus is shared with touch + codec; ESP-IDF's
 * i2c_master driver serialises transactions per bus, so cross-task use
 * is safe). The PMU's IRQ *pin* is not usable on this board family — it
 * collides with the octal-PSRAM DQS pin — so we poll instead.
 *
 * Long press stays with the PMU hardware (forced power-off); this module
 * only surfaces SHORT presses. Handler fires on the poll task and must
 * follow the same rules as buttons.h handlers (no synchronous LVGL work
 * without the display lock).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pwr_key_cb_t)(void *usr_data);

/* Probe the PMU and start the poll task. Returns false (and stays inert)
 * if the AXP2101 doesn't answer — the feature degrades, nothing crashes. */
bool pwr_key_init(void);

/* Replace the short-press handler. cb may be NULL to detach. */
void pwr_key_set_handler(pwr_key_cb_t cb, void *usr_data);

#ifdef __cplusplus
}
#endif
