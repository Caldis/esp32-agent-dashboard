/*
 * ble_provision — first-boot WiFi pairing over BLE NUS (v1.3.0 scaffold).
 *
 * Depends on:
 *   - v0.4.0 transport_ble_nus (GATT bring-up)
 *   - v0.6.0 nvs_crypto (encrypted credential storage)
 *
 * State machine:
 *   UNPAIRED -> PAIRING (BLE adv, mobile app connecting)
 *            -> TRYING  (`dash provision` received, WiFi connecting)
 *            -> PAIRED  (creds saved + reboot scheduled)
 *            -> FAILED  (-> back to UNPAIRED on retry)
 *
 * Full implementation lands in v1.3.x once the BLE stack is real.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROVISION_STATE_UNPAIRED = 0,
    PROVISION_STATE_PAIRING,
    PROVISION_STATE_TRYING,
    PROVISION_STATE_PAIRED,
    PROVISION_STATE_FAILED,
} provision_state_t;

typedef struct {
    char ssid[33];          /* IEEE 802.11 SSID max */
    char psk[65];            /* WPA2 max */
    char tz[40];             /* IANA */
    char device_name[32];
    char owner[32];
} provision_creds_t;

/* Boot-time check: if NVS has wifi creds, return PROVISION_STATE_PAIRED.
 * Otherwise enter PROVISION_STATE_UNPAIRED and start the BLE advert. */
provision_state_t provisioning_init(void);

/* Called by the `dash provision` console handler. Validates + stores
 * + reboots the device. Returns false if the payload is malformed
 * or the wifi creds don't connect within `connect_timeout_s`. */
bool provisioning_apply(const provision_creds_t *creds, int connect_timeout_s);

/* Wipe wifi creds + reboot. */
void provisioning_reset(void);

/* Current state, for `dash provision_status` and scene rendering. */
provision_state_t provisioning_state(void);

#ifdef __cplusplus
}
#endif
