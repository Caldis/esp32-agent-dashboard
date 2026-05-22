/*
 * transport_serial.c — USB-Serial JTAG transport (refactor).
 *
 * v0.4.0 scaffold (TRANS1). NOT YET WIRED INTO BUILD.
 *
 * This is the *only* concrete that isn't a stub. It wraps the existing
 * esp-harness console_protocol path so the rest of the codebase can be
 * transport-agnostic. The actual serial bring-up still happens inside
 * console_protocol_init() (called from app_main); this wrapper just
 * exposes the standard transport_t vtable surface for it.
 *
 * Functional change vs v0.1.x: NONE. Same wire, same latency, same
 * buffer sizes. The point is the interface.
 */

#include "transport.h"

#include <string.h>

#include "esp_log.h"

/* The harness header is included as forward-declared here so this .c
 * compiles standalone in the scaffold. In the integrated build F2 will
 * include the real header.
 *
 * TODO(v0.4.0 F2): include "harness/console_protocol.h" and remove
 * these prototypes.
 */
extern int console_write_raw(const char *buf, size_t len);

static const char *TAG = "transport_serial";

typedef struct {
    transport_state_t state;
    transport_link_quality_t lq;
} serial_priv_t;

static serial_priv_t s_priv = {
    .state = TRANSPORT_STATE_DOWN,
    .lq = { .rssi_dbm = -127, .tx_err = 0, .rx_err = 0, .bytes_tx = 0, .bytes_rx = 0 },
};

static esp_err_t serial_open(transport_t *self)
{
    (void)self;
    /* The harness has already brought USB-Serial JTAG up via
     * console_protocol_init() during boot. We just mark READY. */
    s_priv.state = TRANSPORT_STATE_READY;
    ESP_LOGI(TAG, "ready (wrapping console_protocol)");
    return ESP_OK;
}

static esp_err_t serial_close(transport_t *self)
{
    (void)self;
    /* Serial stays up permanently — we don't actually tear down the
     * USB-Serial JTAG driver, just mark our wrapper as DOWN so the
     * failover chain skips us. (In practice serial_close() should
     * almost never be called.) */
    s_priv.state = TRANSPORT_STATE_DOWN;
    return ESP_OK;
}

static esp_err_t serial_write_line(transport_t *self, const char *line, size_t len)
{
    (void)self;
    if (!line) return ESP_ERR_INVALID_ARG;
    if (s_priv.state != TRANSPORT_STATE_READY) return ESP_ERR_INVALID_STATE;

    /* console_write_raw expects the caller to manage NL itself. We
     * append one if not present. */
    int written = console_write_raw(line, len);
    if (written < 0) {
        s_priv.lq.tx_err++;
        return ESP_FAIL;
    }
    s_priv.lq.bytes_tx += (uint64_t)written;
    if (len == 0 || line[len - 1] != '\n') {
        console_write_raw("\n", 1);
        s_priv.lq.bytes_tx += 1;
    }
    return ESP_OK;
}

static void serial_set_line_callback(transport_t *self,
                                     transport_line_cb_t cb, void *user)
{
    (void)self;
    /* Serial's RX is driven by the harness's console_protocol task,
     * which already calls into the harness command dispatch. We do NOT
     * intercept it for the scaffold — the existing `dash *` handlers
     * keep working as before.
     *
     * TODO(v0.4.0 F2): if we want to unify framing through
     * transport_framer_feed(), we'll need a hook in console_protocol
     * that forwards each RX'd line to our framer. For now, the framer
     * is exercised by BLE/WiFi paths only; serial stays on the harness
     * path.
     */
    (void)cb;
    (void)user;
}

static transport_state_t serial_state(transport_t *self)
{
    (void)self;
    return s_priv.state;
}

static size_t serial_mtu(transport_t *self)
{
    (void)self;
    /* Effective limit is CONSOLE_MAX_LINE = 1024; report unlimited so
     * the caller knows it doesn't need to fragment. */
    return SIZE_MAX;
}

static void serial_link_quality(transport_t *self, transport_link_quality_t *out)
{
    (void)self;
    if (out) *out = s_priv.lq;
}

static transport_t s_serial = {
    .name              = "serial",
    .kind              = TRANSPORT_KIND_SERIAL,
    .caps              = TRANSPORT_CAPS_SERIAL,
    .open              = serial_open,
    .close             = serial_close,
    .write_line        = serial_write_line,
    .set_line_callback = serial_set_line_callback,
    .state             = serial_state,
    .mtu               = serial_mtu,
    .link_quality      = serial_link_quality,
    .priv              = &s_priv,
};

transport_t *transport_serial_instance(void)
{
    return &s_serial;
}
