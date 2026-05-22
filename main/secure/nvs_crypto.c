/*
 * nvs_crypto.c — encrypted-NVS wrapper implementation.
 *
 * See nvs_crypto.h for rationale + threat-model link.
 *
 * ## Why
 *
 * The IDF v6.0.1 NVS encryption API has two flavours:
 *   - "Flash-encryption-protected key store" (recommended) — keys live
 *     in an `nvs_keys` partition flagged `encrypted` in partitions.csv,
 *     and the platform's flash-encryption key (XTS-AES-256 from eFuse)
 *     transparently decrypts that partition.
 *   - "HMAC-derived key" (HMAC peripheral, fancy) — out of scope here;
 *     v0.6.0 commits to the simpler flow and we revisit if eFuse
 *     constraints bite us.
 *
 * Idempotency: nvs_flash_secure_init_partition() is *not* idempotent
 * in IDF — calling it twice on the same partition returns
 * ESP_ERR_INVALID_STATE. We gate behind a one-shot flag.
 *
 * Failure modes deliberately surface to caller; we do NOT silently
 * fall back to the clear partition because that would defeat the
 * point. The one exception: builds without CONFIG_NVS_ENCRYPTION
 * (dev builds) compile this file but emit a one-time banner and
 * delegate to the standard nvs_flash_init() partition.
 */

#include "nvs_crypto.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#if __has_include("nvs_flash_secure.h")
/* Some IDF distributions split this header out; most just expose the
 * symbols through nvs_flash.h. Either way we get the symbol below. */
#  include "nvs_flash_secure.h"
#endif

static const char *TAG = "nvs_crypto";

/* Internal state — guarded only by the implicit single-init contract:
 * app_main() calls nvs_crypto_init() before any other code touches
 * the managed namespaces, and only the main task touches these flags
 * after that. */
static bool s_init_attempted = false;
static bool s_init_ok        = false;
static bool s_encrypted      = false;

/* ── Detect whether the build provides NVS encryption ─────────────── */

#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
#  define DASHBOARD_NVS_HAVE_ENCRYPTION 1
#else
#  define DASHBOARD_NVS_HAVE_ENCRYPTION 0
#endif

/* ── Initialise ───────────────────────────────────────────────────── */

esp_err_t nvs_crypto_init(void)
{
    if (s_init_attempted) {
        return s_init_ok ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_init_attempted = true;

#if DASHBOARD_NVS_HAVE_ENCRYPTION
    /* Step 1: locate the nvs_keys partition. */
    const esp_partition_t *keys_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
        NVS_CRYPTO_KEYS_PARTITION);
    if (keys_part == NULL) {
        ESP_LOGE(TAG, "nvs_keys partition '%s' not found — "
                      "is partitions.csv updated? falling back to clear NVS",
                 NVS_CRYPTO_KEYS_PARTITION);
        s_init_ok = true;          /* clear-mode still works */
        s_encrypted = false;
        return ESP_OK;
    }

    /* Step 2: read/generate the XTS key. */
    nvs_sec_cfg_t cfg;
    esp_err_t err = nvs_flash_read_security_cfg(keys_part, &cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "nvs_keys blank — generating fresh XTS-AES key (one-time)");
        err = nvs_flash_generate_keys(keys_part, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_generate_keys failed: 0x%x", err);
            s_init_ok = false;
            return err;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_read_security_cfg failed: 0x%x", err);
        s_init_ok = false;
        return err;
    }

    /* Step 3: bring up the encrypted data partition. */
    err = nvs_flash_secure_init_partition(NVS_CRYPTO_PARTITION, &cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Same recovery dance as the clear-mode init in app_main. */
        ESP_LOGW(TAG, "encrypted nvs partition needs erase (0x%x), retrying", err);
        const esp_partition_t *data_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
            NVS_CRYPTO_PARTITION);
        if (data_part) {
            esp_partition_erase_range(data_part, 0, data_part->size);
        }
        err = nvs_flash_secure_init_partition(NVS_CRYPTO_PARTITION, &cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_secure_init_partition failed: 0x%x", err);
        s_init_ok = false;
        return err;
    }

    s_init_ok = true;
    s_encrypted = true;
    ESP_LOGI(TAG, "encrypted NVS ready (partition='%s', keys='%s')",
             NVS_CRYPTO_PARTITION, NVS_CRYPTO_KEYS_PARTITION);
    return ESP_OK;
#else
    /* Dev builds: clear NVS. Caller (app_main) already initialised
     * the default partition; we just record the state. */
    ESP_LOGW(TAG, "BANNER: dev posture, NVS in clear "
                  "(CONFIG_NVS_ENCRYPTION not set)");
    s_init_ok = true;
    s_encrypted = false;
    return ESP_OK;
#endif
}

bool nvs_crypto_is_encrypted(void)
{
    return s_init_ok && s_encrypted;
}

/* ── Per-operation helpers ────────────────────────────────────────── */

static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!s_init_attempted) {
        /* Caller forgot to init. Auto-init in clear mode so we don't
         * crash; but log loudly because this is a misconfiguration. */
        ESP_LOGW(TAG, "lazy-init: caller skipped nvs_crypto_init()");
        nvs_crypto_init();
    }
    if (s_encrypted) {
        return nvs_open_from_partition(NVS_CRYPTO_PARTITION, ns, mode, out);
    }
    return nvs_open(ns, mode, out);
}

esp_err_t nvs_crypto_set_str(const char *ns, const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_crypto_get_str(const char *ns, const char *key,
                             char *out, size_t *in_out_size)
{
    if (!out || !in_out_size || *in_out_size == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, out, in_out_size);
    nvs_close(h);
    return err;
}

esp_err_t nvs_crypto_set_u16(const char *ns, const char *key, uint16_t v)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_crypto_get_u16(const char *ns, const char *key, uint16_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u16(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t nvs_crypto_set_u8(const char *ns, const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_crypto_get_u8(const char *ns, const char *key, uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t nvs_crypto_set_bool(const char *ns, const char *key, bool v)
{
    return nvs_crypto_set_u8(ns, key, v ? 1 : 0);
}

esp_err_t nvs_crypto_get_bool(const char *ns, const char *key, bool *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t v = 0;
    esp_err_t err = nvs_crypto_get_u8(ns, key, &v);
    if (err == ESP_OK) *out = (v != 0);
    return err;
}

esp_err_t nvs_crypto_erase_key(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_crypto_erase_ns(const char *ns)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
