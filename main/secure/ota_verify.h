/*
 * ota_verify.h — signed-OTA payload validation for the dashboard.
 *
 * Threat addressed: A4 in docs/THREAT_MODEL.md — a compromised OTA
 * distribution channel (CDN swap, corp MITM proxy, malicious update
 * server). We refuse to install firmware that doesn't carry a valid
 * ed25519 signature over the payload, made with the private key
 * whose public half is baked into the running firmware
 * (main/secure/ota_pubkey.h).
 *
 * See docs/OTA.md for the wire format diagram and the version-roll
 * rules. Field layout summary:
 *
 *   magic(4="DASH") | version(u16 LE) | size(u32 LE) | sig(64) | firmware
 *
 * The signature covers `magic || version_le || size_le || firmware`.
 *
 * ## Why
 *
 * IDF v6.0.1's mbedtls does NOT ship ed25519 (the header
 * `mbedtls/ed25519.h` does not exist in upstream mbedtls and is not
 * part of Espressif's fork either, as of 2026-05). We use mbedtls
 * for SHA-512 (which IS present and HW-accelerated on the S3 via
 * CONFIG_MBEDTLS_HARDWARE_SHA), and we bundle a portable verify-only
 * ed25519 in main/secure/ed25519_verify.c — the brief's call for
 * "mbedtls/ed25519.h" was aspirational; this is the deviation.
 *
 * The brief's spirit (use a well-trodden ed25519 library, do NOT
 * roll our own curve math from scratch) is honoured: the bundled
 * implementation is the public-domain ref10-derivative used by
 * (e.g.) BoringSSL's portable fallback. We do NOT touch field math
 * by hand.
 */

#ifndef DASHBOARD_SECURE_OTA_VERIFY_H
#define DASHBOARD_SECURE_OTA_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_VERIFY_MAGIC           "DASH"
#define OTA_VERIFY_MAGIC_LEN       4
#define OTA_VERIFY_SIG_LEN         64
#define OTA_VERIFY_PUBKEY_LEN      32
#define OTA_VERIFY_HEADER_LEN     (OTA_VERIFY_MAGIC_LEN + 2 + 4 + OTA_VERIFY_SIG_LEN) /* 74 */

/* Reasons emitted in `EVT: ota_rejected reason=<reason>`. */
typedef enum {
    OTA_VERIFY_OK              = 0,
    OTA_VERIFY_ERR_MAGIC       = 1, /* magic bytes wrong */
    OTA_VERIFY_ERR_SIZE        = 2, /* size field out of range */
    OTA_VERIFY_ERR_VERSION     = 3, /* version <= running */
    OTA_VERIFY_ERR_MAJOR_SKIP  = 4, /* new major - running major > 1 */
    OTA_VERIFY_ERR_SIGNATURE   = 5, /* signature failed verify */
    OTA_VERIFY_ERR_READ        = 6, /* failed to read flash slot */
    OTA_VERIFY_ERR_INTERNAL    = 7, /* mbedtls / allocation failure */
} ota_verify_result_t;

/* Parsed OTA header — fill via ota_verify_parse_header(). */
typedef struct {
    uint16_t version;     /* encoded (major<<12)|(minor<<6)|patch */
    uint32_t size;        /* firmware bytes following the header */
    uint8_t  sig[OTA_VERIFY_SIG_LEN];
} ota_header_t;

/*
 * Parse the first OTA_VERIFY_HEADER_LEN bytes of a payload into a
 * struct. Verifies the magic and basic size sanity. Does NOT verify
 * the signature (that requires the firmware bytes; use
 * ota_verify_payload_partition for the full flow).
 *
 * `header_buf` must be at least OTA_VERIFY_HEADER_LEN bytes.
 * `max_firmware_size` is the host's claimed upper bound — typically
 *   the size of the destination ota_X partition.
 */
ota_verify_result_t ota_verify_parse_header(const uint8_t *header_buf,
                                            size_t buf_len,
                                            uint32_t max_firmware_size,
                                            ota_header_t *out);

/*
 * Check a candidate `version` against the running firmware:
 *   - must be strictly greater than running encoded version;
 *   - must not jump major by more than 1.
 *
 * `running_version` should be derived from esp_app_desc.version
 * via ota_verify_encode_semver().
 */
ota_verify_result_t ota_verify_check_version(uint16_t candidate,
                                             uint16_t running_version);

/*
 * Encode "X.Y.Z" semver into u16 (major<<12)|(minor<<6)|patch.
 * Returns 0 on parse failure (which is also a valid encoded "0.0.0"
 * but we treat 0 as "unknown" in practice).
 */
uint16_t ota_verify_encode_semver(const char *semver);

/*
 * Full payload verification flow used by the OTA apply path:
 *   1. parses the header (already done by caller, passed in);
 *   2. reads `firmware_size` bytes from `slot` starting at
 *      `firmware_offset` (= OTA_VERIFY_HEADER_LEN);
 *   3. computes sha512(magic || ver_le || size_le || firmware);
 *   4. ed25519-verifies `sig` against that hash using the baked-in
 *      public key from ota_pubkey.h.
 *
 * Returns OTA_VERIFY_OK on success, the failure reason otherwise.
 * On success the caller may call esp_ota_set_boot_partition(slot).
 */
ota_verify_result_t ota_verify_payload_partition(const esp_partition_t *slot,
                                                 const ota_header_t *hdr);

/*
 * Convenience: stringify a result for the EVT line.
 * Returns a stable, lower-case token: "magic", "size", "version",
 * "major_skip", "signature", "read", "internal", or "ok".
 */
const char *ota_verify_reason_str(ota_verify_result_t r);

/*
 * Compute sha256(pubkey)[:8] hex for `dash ota info`. `out` must
 * hold 17 bytes (16 hex + NUL).
 */
void ota_verify_pubkey_fingerprint(char out[17]);

#ifdef __cplusplus
}
#endif

#endif /* DASHBOARD_SECURE_OTA_VERIFY_H */
