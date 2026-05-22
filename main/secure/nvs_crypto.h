/*
 * nvs_crypto.h — encrypted-NVS wrapper for the dashboard's `dashcfg`
 *                namespace.
 *
 * Threat addressed: A1 in docs/THREAT_MODEL.md — a shoulder-surfer
 * who steals the device, pulls the `nvs` partition with esptool, and
 * reads the user's WiFi PSK / bridge auth token / configured strings.
 *
 * Mitigation: the `dashcfg` namespace (and the `ota` namespace
 * managed alongside it) live in an encrypted NVS partition. The
 * per-device XTS-AES key is stored in a separate `nvs_keys`
 * partition that is itself encrypted by the SoC's flash-encryption
 * key (eFuse-burned in release builds). An attacker reading raw
 * flash sees ciphertext for the values that matter.
 *
 * ## Why
 *
 * We wrap nvs_open/nvs_set_str/nvs_get_str in dashboard-specific
 * helpers rather than have callers reach into the encrypted partition
 * by name. The wrapper:
 *   1. routes every read/write through the *same* encrypted partition
 *      (so a future refactor can't accidentally split the namespace
 *      across a clear partition and an encrypted one),
 *   2. lazy-initialises the encrypted partition on first use so the
 *      caller can stay ignorant of nvs_flash_secure_init() ordering,
 *   3. fails safe — if the encrypted partition can't be opened, the
 *      wrapper returns an error rather than silently falling back to
 *      the clear partition.
 *
 * Idempotent: nvs_crypto_init() may be called multiple times from
 * different boot paths (factory reset flow, OTA rollback) without
 * double-initialising the partition.
 */

#ifndef DASHBOARD_SECURE_NVS_CRYPTO_H
#define DASHBOARD_SECURE_NVS_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Partition label for the encrypted NVS data. F2 must reserve this
 * in partitions.csv (see docs/THREAT_MODEL.md §Build integration). */
#define NVS_CRYPTO_PARTITION "nvs"

/* Partition label for the XTS key store (encrypted by flash
 * encryption). F2 must reserve `nvs_keys, data, nvs_keys, …`. */
#define NVS_CRYPTO_KEYS_PARTITION "nvs_keys"

/* Namespaces this module manages. All other code must go through the
 * wrappers below for these namespaces. */
#define NVS_CRYPTO_NS_DASHCFG "dashcfg"
#define NVS_CRYPTO_NS_OTA     "ota"

/*
 * Initialise the encrypted NVS subsystem. Safe to call repeatedly.
 *
 * On first call:
 *   - If the `nvs_keys` partition is blank, generates a fresh random
 *     XTS-AES-128 key and writes it there (one-time).
 *   - Reads the key back, calls nvs_flash_secure_init_partition() on
 *     the `nvs` partition.
 *
 * On subsequent calls: returns ESP_OK immediately if already
 * initialised, ESP_ERR_INVALID_STATE if a previous attempt
 * permanently failed.
 *
 * Falls back to ESP_ERR_NOT_SUPPORTED on builds without
 * CONFIG_NVS_ENCRYPTION — caller decides whether to proceed
 * (dev mode) or refuse to boot (release mode). The dashboard's
 * app_main() in release mode treats NOT_SUPPORTED as fatal.
 */
esp_err_t nvs_crypto_init(void);

/*
 * Whether nvs_crypto_init() succeeded with hardware encryption.
 * If false, the wrappers below operate on the (clear) default
 * partition and a banner is emitted to console.
 */
bool nvs_crypto_is_encrypted(void);

/*
 * Set / get a string in one of the managed namespaces.
 *
 * Semantics mirror nvs_set_str / nvs_get_str (NUL-terminated,
 * length includes the NUL byte). out_size is in/out: caller
 * passes capacity, function writes actual size including NUL.
 *
 * Returns ESP_ERR_NVS_NOT_FOUND if the key doesn't exist.
 */
esp_err_t nvs_crypto_set_str(const char *ns, const char *key,
                             const char *value);
esp_err_t nvs_crypto_get_str(const char *ns, const char *key,
                             char *out, size_t *in_out_size);

/* Convenience: u16 + u8 + bool, since OTA needs them (last_version
 * is u16, attempts is u8, pending is bool). */
esp_err_t nvs_crypto_set_u16(const char *ns, const char *key, uint16_t v);
esp_err_t nvs_crypto_get_u16(const char *ns, const char *key, uint16_t *out);
esp_err_t nvs_crypto_set_u8 (const char *ns, const char *key, uint8_t v);
esp_err_t nvs_crypto_get_u8 (const char *ns, const char *key, uint8_t *out);
esp_err_t nvs_crypto_set_bool(const char *ns, const char *key, bool v);
esp_err_t nvs_crypto_get_bool(const char *ns, const char *key, bool *out);

/*
 * Erase a single key. Used by the (future) `dash factory reset` flow.
 * Returns ESP_OK on success or if the key did not exist.
 */
esp_err_t nvs_crypto_erase_key(const char *ns, const char *key);

/*
 * Erase the entire namespace. Used by factory reset.
 */
esp_err_t nvs_crypto_erase_ns(const char *ns);

#ifdef __cplusplus
}
#endif

#endif /* DASHBOARD_SECURE_NVS_CRYPTO_H */
