/*
 * transport.c — shared framing + active-transport registry.
 *
 * v0.4.0 scaffold (TRANS1). NOT YET WIRED INTO BUILD.
 * See docs/TRANSPORTS.md §Build integration.
 *
 * Responsibilities:
 *   • Line framing layer shared across all three concretes — concretes
 *     just call transport_framer_feed(bytes, len) on their RX path.
 *   • Active-transport registry — transport_write_line() forwards to
 *     whichever concrete is currently "active".
 *   • Failover chain — default serial → BLE → WiFi, with user pin
 *     override.
 *
 * The actual scene / handler code doesn't touch this file directly —
 * it goes through transport_write_line() or the console_send_evt()
 * shim that the harness already provides (which F2 will re-route to
 * the active transport during build integration).
 */

#include "transport.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"

/* Line buffer size mirrors esp-harness console_protocol's
 * CONSOLE_MAX_LINE. Keep these in lockstep — if the harness raises its
 * limit, raise this one too. */
#ifndef TRANSPORT_LINE_BUF_BYTES
#define TRANSPORT_LINE_BUF_BYTES 1024
#endif

static const char *TAG = "transport";

/* ─── Kind name table ─── */

static const char *s_kind_names[TRANSPORT_KIND__COUNT] = {
    [TRANSPORT_KIND_SERIAL]   = "serial",
    [TRANSPORT_KIND_BLE_NUS]  = "ble_nus",
    [TRANSPORT_KIND_WIFI_TLS] = "wifi_tls",
};

const char *transport_kind_name(transport_kind_t k)
{
    if ((unsigned)k >= TRANSPORT_KIND__COUNT) return "unknown";
    return s_kind_names[k];
}

transport_kind_t transport_kind_parse(const char *name)
{
    if (!name) return TRANSPORT_KIND__COUNT;
    for (int i = 0; i < TRANSPORT_KIND__COUNT; ++i) {
        if (strcmp(name, s_kind_names[i]) == 0) return (transport_kind_t)i;
    }
    return TRANSPORT_KIND__COUNT;
}

/* ─── State name table ─── */

static const char *s_state_names[] = {
    "DOWN", "OPENING", "READY", "CLOSING", "BACKOFF",
};

const char *transport_state_name(transport_state_t s)
{
    if ((unsigned)s >= sizeof(s_state_names) / sizeof(s_state_names[0])) {
        return "unknown";
    }
    return s_state_names[s];
}

/* ─── Active-transport registry ─── */

static transport_t *s_concretes[TRANSPORT_KIND__COUNT] = {0};
static transport_t *s_active = NULL;
static transport_kind_t s_pin = TRANSPORT_KIND__COUNT;   /* COUNT = unpinned */
static uint32_t s_last_switch_ms = 0;
#define TRANSPORT_HOLDDOWN_MS 5000u

/* ─── Inbound framing (shared by all concretes) ─── */

static uint8_t s_line_buf[TRANSPORT_LINE_BUF_BYTES];
static size_t  s_line_pos = 0;
static bool    s_overflow_drain = false;
static transport_line_cb_t s_line_cb = NULL;
static void               *s_line_cb_user = NULL;

/* In the integrated build this should be a portMUX or task mutex.
 * For the scaffold (which compiles standalone) we leave it as a stub
 * comment — F2 will wire it during build integration.
 *
 * TODO(v0.4.0 F2): protect framer state with a portMUX_TYPE or use a
 * FreeRTOS queue that funnels into the LVGL task.
 */

void transport_framer_feed(const uint8_t *bytes, size_t len)
{
    if (!bytes || len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = bytes[i];
        if (b == '\r') continue;        /* tolerate CRLF / CR */
        if (b == '\n') {
            if (s_overflow_drain) {
                /* end-of-overflowed-line: discard and resync */
                s_overflow_drain = false;
                s_line_pos = 0;
                continue;
            }
            s_line_buf[s_line_pos] = '\0';
            if (s_line_cb && s_line_pos > 0) {
                s_line_cb((const char *)s_line_buf, s_line_pos, s_line_cb_user);
            }
            s_line_pos = 0;
            continue;
        }
        if (s_overflow_drain) continue;
        if (s_line_pos >= sizeof(s_line_buf) - 1) {
            /* line too long — drop until next NL */
            ESP_LOGW(TAG, "framer overflow at %u bytes; draining",
                     (unsigned)s_line_pos);
            s_overflow_drain = true;
            s_line_pos = 0;
            continue;
        }
        s_line_buf[s_line_pos++] = b;
    }
}

void transport_framer_reset(void)
{
    s_line_pos = 0;
    s_overflow_drain = false;
}

/* ─── Registry / lifecycle ─── */

static transport_t *get_concrete(transport_kind_t k)
{
    if ((unsigned)k >= TRANSPORT_KIND__COUNT) return NULL;
    return s_concretes[k];
}

esp_err_t transport_init_all(void)
{
    /* Populate the concrete table. Order matters only for diagnostics;
     * the failover chain enforces priority. */
    s_concretes[TRANSPORT_KIND_SERIAL]   = transport_serial_instance();
    s_concretes[TRANSPORT_KIND_BLE_NUS]  = transport_ble_nus_instance();
    s_concretes[TRANSPORT_KIND_WIFI_TLS] = transport_wifi_instance();

    /* Serial always comes up at boot — it's the canonical link and
     * doesn't require any user action. */
    transport_t *serial = get_concrete(TRANSPORT_KIND_SERIAL);
    if (!serial) {
        ESP_LOGE(TAG, "no serial transport instance — wiring error");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = serial->open(serial);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "serial open failed: %s", esp_err_to_name(err));
        /* keep going — BLE/WiFi can still come up later */
    } else {
        s_active = serial;
        ESP_LOGI(TAG, "active transport: %s", serial->name);
    }

    /* BLE and WiFi stay DOWN until explicitly opened or until the
     * failover layer is invoked (which currently isn't on the boot
     * path — F2 hooks that to the BLE/WiFi event handlers during
     * build integration). */
    return ESP_OK;
}

esp_err_t transport_open(transport_kind_t kind)
{
    transport_t *t = get_concrete(kind);
    if (!t || !t->open) return ESP_ERR_NOT_SUPPORTED;
    return t->open(t);
}

static void emit_transport_changed(transport_t *from, transport_t *to)
{
    /* In the integrated build this calls console_send_evt(...). For
     * the scaffold we log so the abstraction can be reasoned about
     * standalone.
     *
     * TODO(v0.4.0 F2): replace ESP_LOGI with
     *   console_send_evt("transport_changed from=%s to=%s",
     *                    from ? from->name : "none",
     *                    to   ? to->name   : "none");
     */
    ESP_LOGI(TAG, "EVT: transport_changed from=%s to=%s",
             from ? from->name : "none",
             to   ? to->name   : "none");
}

esp_err_t transport_set_active(transport_kind_t kind)
{
    transport_t *next = get_concrete(kind);
    if (!next) return ESP_ERR_NOT_SUPPORTED;
    if (next == s_active) return ESP_OK;

    /* Hold-down: don't flap. If we've just switched, ignore further
     * switch requests for TRANSPORT_HOLDDOWN_MS. (Real timer source
     * comes from esp_timer — wired during build integration.) */
    /* TODO(v0.4.0 F2): use esp_timer_get_time() / 1000 here. */

    transport_t *prev = s_active;
    s_active = next;
    emit_transport_changed(prev, next);
    transport_framer_reset();
    return ESP_OK;
}

transport_t *transport_active(void)
{
    return s_active;
}

const char *transport_active_name(void)
{
    return s_active ? s_active->name : "none";
}

esp_err_t transport_write_line(const char *line, size_t len)
{
    if (!s_active || !s_active->write_line) return ESP_ERR_INVALID_STATE;
    return s_active->write_line(s_active, line, len);
}

/* ─── Failover chain ─── */

/* Priority order — matches docs/TRANSPORTS.md §3. Serial first because
 * it's the lowest-latency, lowest-friction link; BLE second because
 * it's the bootstrap path for WiFi provisioning (v1.3.0); WiFi last
 * because it has the most failure modes. */
static const transport_kind_t s_failover_order[] = {
    TRANSPORT_KIND_SERIAL,
    TRANSPORT_KIND_BLE_NUS,
    TRANSPORT_KIND_WIFI_TLS,
};

transport_kind_t transport_failover_step(void)
{
    /* User pin wins. */
    if (s_pin != TRANSPORT_KIND__COUNT) {
        transport_t *t = get_concrete(s_pin);
        if (t && t->state && t->state(t) == TRANSPORT_STATE_READY) {
            transport_set_active(s_pin);
            return s_pin;
        }
        /* pinned but not ready — caller backs off and retries */
        return TRANSPORT_KIND__COUNT;
    }

    for (size_t i = 0; i < sizeof(s_failover_order) / sizeof(s_failover_order[0]); ++i) {
        transport_kind_t k = s_failover_order[i];
        transport_t *t = get_concrete(k);
        if (!t || !t->state) continue;
        if (t->state(t) == TRANSPORT_STATE_READY) {
            transport_set_active(k);
            return k;
        }
    }
    /* Nothing ready. */
    return TRANSPORT_KIND__COUNT;
}

void transport_pin_active(transport_kind_t kind)
{
    s_pin = kind;
    if (kind != TRANSPORT_KIND__COUNT) {
        /* try to honour the pin immediately */
        transport_failover_step();
    }
}
