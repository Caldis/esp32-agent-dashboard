/*
 * transport.h — abstract host-link transport (v0.4.0 scaffold, TRANS1).
 *
 * Single vtable that every concrete transport (serial / BLE NUS / WiFi
 * TLS) implements. Scenes and `dash *` handlers stay transport-agnostic:
 * they call transport_write_line() and the active transport (selected by
 * the failover layer) handles framing and delivery.
 *
 * The framing layer (line buffering + overflow drain) lives in
 * transport.c and is SHARED across all three concretes. That's why
 * write_line() takes a plain `const char *` — concretes don't reframe.
 *
 * Status: NOT YET WIRED INTO BUILD. See docs/TRANSPORTS.md §Build
 * integration. F2 picks this up during the v0.4.0 cycle.
 *
 * Cross-agent notes:
 *   • SEC1 (v0.6.0): TLS cert pinning lives inside transport_wifi.c
 *   • F2 (v0.4.0):    GATT registration + esp_wifi event handlers
 *   • OBS1 (v0.9.0):  link_quality EVT cadence + counter shape
 *   • H4 (v1.5.0):    discover.py JSON shape feeds the replay pipeline
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Transport kinds (mirrors `transport` field on `dash hello`) ─── */

typedef enum {
    TRANSPORT_KIND_SERIAL = 0,   /* USB-Serial JTAG (always built) */
    TRANSPORT_KIND_BLE_NUS,      /* Nordic UART over BLE GATT     */
    TRANSPORT_KIND_WIFI_TLS,     /* TCP + TLS to dashboard.local  */
    TRANSPORT_KIND__COUNT,
} transport_kind_t;

/* String form, suitable for the `dash hello` "transport" field. Stable
 * across versions. Returns "unknown" for out-of-range. */
const char *transport_kind_name(transport_kind_t k);

/* Inverse — parse "serial" / "ble_nus" / "wifi_tls". Returns
 * TRANSPORT_KIND__COUNT on unknown. */
transport_kind_t transport_kind_parse(const char *name);

/* ─── Capability bitmap (matches docs/TRANSPORTS.md §7) ─── */

#define TRANSPORT_CAP_LINES            (1u << 0)
#define TRANSPORT_CAP_PAYLOAD_FOLLOWS  (1u << 1)
#define TRANSPORT_CAP_EVT_STREAM       (1u << 2)
#define TRANSPORT_CAP_BIN_FRAMES       (1u << 3)
#define TRANSPORT_CAP_CONFIG_NVS       (1u << 4)
#define TRANSPORT_CAP_LINK_QUALITY     (1u << 5)
#define TRANSPORT_CAP_FAILOVER         (1u << 6)
#define TRANSPORT_CAP_MTU_HINT         (1u << 7)
#define TRANSPORT_CAP_FARE_WELL        (1u << 8)

/* Per-transport advertised capability sets. Keep in sync with
 * docs/TRANSPORTS.md §7 table. */
#define TRANSPORT_CAPS_SERIAL    (TRANSPORT_CAP_LINES \
                                  | TRANSPORT_CAP_PAYLOAD_FOLLOWS \
                                  | TRANSPORT_CAP_EVT_STREAM \
                                  | TRANSPORT_CAP_BIN_FRAMES \
                                  | TRANSPORT_CAP_CONFIG_NVS \
                                  | TRANSPORT_CAP_FAILOVER \
                                  | TRANSPORT_CAP_MTU_HINT \
                                  | TRANSPORT_CAP_FARE_WELL)

#define TRANSPORT_CAPS_BLE_NUS   (TRANSPORT_CAP_LINES \
                                  | TRANSPORT_CAP_PAYLOAD_FOLLOWS \
                                  | TRANSPORT_CAP_EVT_STREAM \
                                  | TRANSPORT_CAP_CONFIG_NVS \
                                  | TRANSPORT_CAP_LINK_QUALITY \
                                  | TRANSPORT_CAP_FAILOVER \
                                  | TRANSPORT_CAP_MTU_HINT \
                                  | TRANSPORT_CAP_FARE_WELL)

#define TRANSPORT_CAPS_WIFI_TLS  (TRANSPORT_CAP_LINES \
                                  | TRANSPORT_CAP_PAYLOAD_FOLLOWS \
                                  | TRANSPORT_CAP_EVT_STREAM \
                                  | TRANSPORT_CAP_BIN_FRAMES \
                                  | TRANSPORT_CAP_CONFIG_NVS \
                                  | TRANSPORT_CAP_LINK_QUALITY \
                                  | TRANSPORT_CAP_FAILOVER \
                                  | TRANSPORT_CAP_MTU_HINT \
                                  | TRANSPORT_CAP_FARE_WELL)

/* ─── State machine (mirrors TRANSPORTS.md §2) ─── */

typedef enum {
    TRANSPORT_STATE_DOWN = 0,
    TRANSPORT_STATE_OPENING,
    TRANSPORT_STATE_READY,
    TRANSPORT_STATE_CLOSING,
    TRANSPORT_STATE_BACKOFF,
} transport_state_t;

const char *transport_state_name(transport_state_t s);

/* ─── Inbound line callback ─── */

/* Called from the framing layer (transport.c) whenever a complete
 * line (NL-delimited, NL stripped, NUL-terminated) arrives on the
 * currently-active transport. The framing layer guarantees:
 *   • `line` is NUL-terminated and `len` excludes the NUL
 *   • the buffer is owned by the framer — copy if you need it past
 *     the callback return.
 *   • partial / overrun lines have already been dropped.
 */
typedef void (*transport_line_cb_t)(const char *line, size_t len, void *user);

/* ─── Link-quality counters (BLE / WiFi feed this, OBS1 emits EVTs) ─── */

typedef struct {
    int rssi_dbm;        /* -127 = unknown (serial always reports -127) */
    uint32_t tx_err;
    uint32_t rx_err;
    uint64_t bytes_tx;
    uint64_t bytes_rx;
} transport_link_quality_t;

/* ─── The vtable ─── */

typedef struct transport_s transport_t;

struct transport_s {
    const char *name;                  /* "serial" / "ble_nus" / "wifi_tls" */
    transport_kind_t kind;
    uint32_t caps;                     /* TRANSPORT_CAPS_* bitmap */

    /* lifecycle */
    esp_err_t (*open)(transport_t *self);
    esp_err_t (*close)(transport_t *self);

    /* I/O */
    esp_err_t (*write_line)(transport_t *self, const char *line, size_t len);
    void      (*set_line_callback)(transport_t *self,
                                   transport_line_cb_t cb, void *user);

    /* introspection */
    transport_state_t (*state)(transport_t *self);
    size_t            (*mtu)(transport_t *self);   /* SIZE_MAX = unlimited */
    void              (*link_quality)(transport_t *self,
                                      transport_link_quality_t *out);

    void *priv;                        /* per-impl state */
};

/* ─── Concretes (registered at boot) ─── */

transport_t *transport_serial_instance(void);
transport_t *transport_ble_nus_instance(void);   /* stub */
transport_t *transport_wifi_instance(void);      /* stub */

/* ─── Active-transport registry (failover layer manipulates this) ─── */

/* Initialise the transport subsystem. Call once at boot, after
 * console_protocol_init() and before agent_commands_register().
 * - opens TRANSPORT_KIND_SERIAL unconditionally
 * - leaves BLE / WiFi DOWN until transport_open(kind) is called or the
 *   failover chain promotes them
 *
 * F2 note: invoke from app_main after `console_protocol_init()`.
 */
esp_err_t transport_init_all(void);

/* Open a specific transport (no-op if already READY). Returns
 * ESP_ERR_NOT_SUPPORTED if the build doesn't include that transport
 * (CONFIG_TRANSPORT_BLE / CONFIG_TRANSPORT_WIFI gated). */
esp_err_t transport_open(transport_kind_t kind);

/* Set which transport is currently "active" — i.e. which one
 * transport_write_line() forwards to. Emits EVT: transport_changed
 * if the active changed. */
esp_err_t transport_set_active(transport_kind_t kind);

/* Returns the currently-active transport, or NULL if none ready. */
transport_t *transport_active(void);

/* Convenience: write a line on the active transport. NL is appended if
 * not already present. Returns ESP_ERR_INVALID_STATE if no transport
 * is active. */
esp_err_t transport_write_line(const char *line, size_t len);

/* Convenience: read the active transport's kind name (for hello). */
const char *transport_active_name(void);

/* ─── Failover policy (default: serial → BLE → WiFi) ─── */

/* Walk the default failover chain. Called by transport.c on link loss
 * and on a 5 s hold-down timer when a higher-priority link becomes
 * available again. F2 hooks this into the BLE / WiFi event callbacks.
 *
 * Returns the kind that's now active, or TRANSPORT_KIND__COUNT if none
 * are ready (caller should retry after BACKOFF).
 */
transport_kind_t transport_failover_step(void);

/* User override (from `dash config '{"transport":"..."}'`). Pinning to
 * a single transport disables failover. Pass TRANSPORT_KIND__COUNT to
 * clear the pin and re-enable the chain. */
void transport_pin_active(transport_kind_t kind);

/* ─── Shared framing layer (called by concretes on RX) ─── */

/* Concrete transports call this from their RX path with raw bytes
 * (may include partial lines, multiple lines, or just a fragment).
 * The framer:
 *   • appends to its line buffer
 *   • on '\n' delivers a NUL-terminated line to the registered callback
 *   • on overflow drops bytes up to and including the next '\n'
 *     (overflow-drain — same policy as console_protocol)
 *
 * Safe to call from any task; protected internally. F2: this is the
 * single point of entry for inbound bytes — every concrete uses it.
 */
void transport_framer_feed(const uint8_t *bytes, size_t len);

/* Reset the line buffer (e.g. on a transport change). */
void transport_framer_reset(void);

#ifdef __cplusplus
}
#endif
