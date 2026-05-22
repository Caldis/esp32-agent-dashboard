/*
 * transport_ble_nus.c — Nordic UART Service over BLE (STUB).
 *
 * v0.4.0 scaffold (TRANS1). NOT YET WIRED INTO BUILD.
 *
 * Service UUID:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E   (Nordic NUS)
 * RX char:       6E400002-...   WRITE / WRITE-NO-RESP, host → device
 * TX char:       6E400003-...   NOTIFY, device → host
 *
 * ESP-IDF v6.0.1 ships NimBLE as the default BLE host. This file is
 * gated by CONFIG_TRANSPORT_BLE (set by F2 during build integration).
 * If CONFIG_TRANSPORT_BLE is not defined, the entire body compiles to
 * a single stubbed instance whose `open` returns ESP_ERR_NOT_SUPPORTED
 * — so the failover chain just skips this transport on builds without
 * BLE.
 *
 * Pairing UX:
 *   v0.4.0 (this file): just-works pairing, BLE_SM_IO_CAP_NO_IO
 *   v1.3.0 (U2 + TRANS1): numeric comparison, scene_pairing displays code
 */

#include "transport.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "transport_ble_nus";

typedef struct {
    transport_state_t state;
    transport_link_quality_t lq;
    uint16_t conn_handle;            /* NimBLE connection handle */
    uint16_t tx_char_handle;
    uint16_t mtu_bytes;              /* negotiated ATT MTU minus 3 */
    transport_line_cb_t line_cb;
    void *line_cb_user;
    bool   notifications_enabled;
} ble_nus_priv_t;

static ble_nus_priv_t s_priv = {
    .state          = TRANSPORT_STATE_DOWN,
    .lq             = { .rssi_dbm = -127, .tx_err = 0, .rx_err = 0,
                        .bytes_tx = 0, .bytes_rx = 0 },
    .conn_handle    = 0xFFFF,
    .tx_char_handle = 0,
    .mtu_bytes      = 244,
    .line_cb        = NULL,
    .line_cb_user   = NULL,
    .notifications_enabled = false,
};

/* ─── Forward declarations of the NimBLE-side entry points F2 fills in ─── */

#ifdef CONFIG_TRANSPORT_BLE

/* TODO(v0.4.0 F2): NimBLE host init.
 *   • nimble_port_init();
 *   • ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
 *   • ble_hs_cfg.sm_bonding = 1;
 *   • ble_hs_cfg.sm_mitm = 0;
 *   • ble_hs_cfg.sm_sc = 1;
 *   • ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
 *   • ble_hs_cfg.sync_cb = on_ble_sync;
 *   • ble_hs_cfg.reset_cb = on_ble_reset;
 *   • nimble_port_freertos_init(ble_host_task);
 */
static esp_err_t nimble_host_init(void);

/* TODO(v0.4.0 F2): GATT registration.
 * Build the NUS service tree:
 *   {
 *     .type = BLE_GATT_SVC_TYPE_PRIMARY,
 *     .uuid = NUS_SVC_UUID,
 *     .characteristics = (struct ble_gatt_chr_def[]) {
 *       { .uuid = NUS_RX_UUID,
 *         .access_cb = on_rx_write,
 *         .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
 *       { .uuid = NUS_TX_UUID,
 *         .access_cb = on_tx_access,
 *         .flags = BLE_GATT_CHR_F_NOTIFY,
 *         .val_handle = &s_priv.tx_char_handle },
 *       { 0 },
 *     },
 *   }
 */
static esp_err_t gatt_register_nus(void);

/* TODO(v0.4.0 F2): GAP advertising.
 * Advertise as "agentdash-<6 hex of MAC>" with the NUS service UUID
 * in the AD payload. Connectable, general-discoverable. Re-arm on
 * disconnect.
 */
static esp_err_t gap_start_advertising(void);

/* TODO(v0.4.0 F2): GAP event handler.
 * Handle CONNECT → state=READY, DISCONNECT → state=DOWN + re-advertise,
 * MTU exchange → update s_priv.mtu_bytes, SUBSCRIBE → notifications_enabled.
 * Pairing events fire EVT: pairing_started / pairing_completed
 * (see docs/PROTOCOL_v2.md §2).
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* TODO(v0.4.0 F2): RX write callback.
 * On WRITE to NUS_RX char:
 *   transport_framer_feed(om->om_data, om->om_len);
 *   s_priv.lq.bytes_rx += om->om_len;
 * The framer delivers complete lines to s_priv.line_cb.
 */
static int on_rx_write(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);

#endif /* CONFIG_TRANSPORT_BLE */

/* ─── Vtable impl ─── */

static esp_err_t ble_open(transport_t *self)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_BLE
    if (s_priv.state != TRANSPORT_STATE_DOWN) return ESP_OK;
    s_priv.state = TRANSPORT_STATE_OPENING;
    esp_err_t err = nimble_host_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        s_priv.state = TRANSPORT_STATE_BACKOFF;
        return err;
    }
    err = gatt_register_nus();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GATT register failed: %s", esp_err_to_name(err));
        s_priv.state = TRANSPORT_STATE_BACKOFF;
        return err;
    }
    err = gap_start_advertising();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GAP adv failed: %s", esp_err_to_name(err));
        s_priv.state = TRANSPORT_STATE_BACKOFF;
        return err;
    }
    /* Note: state moves to READY in the GAP CONNECT event, not here.
     * `open()` succeeding just means advertising is active. */
    ESP_LOGI(TAG, "advertising as NUS — waiting for connect");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "CONFIG_TRANSPORT_BLE not set — BLE NUS unavailable");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t ble_close(transport_t *self)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_BLE
    s_priv.state = TRANSPORT_STATE_CLOSING;
    /* TODO(v0.4.0 F2):
     *   • ble_gap_adv_stop()
     *   • if connected: ble_gap_terminate(conn_handle, ...);
     *   • nimble_port_stop() — only if we're tearing down for power
     *     management; usually we just stop advertising and keep the
     *     host running so we can re-open quickly.
     */
    s_priv.state = TRANSPORT_STATE_DOWN;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t ble_write_line(transport_t *self, const char *line, size_t len)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_BLE
    if (s_priv.state != TRANSPORT_STATE_READY) return ESP_ERR_INVALID_STATE;
    if (!s_priv.notifications_enabled) return ESP_ERR_INVALID_STATE;
    if (!line) return ESP_ERR_INVALID_ARG;

    /* Fragment into MTU-sized chunks. Re-join is on the host side using
     * '\n' as the frame delimiter — same wire grammar as serial, just
     * delivered in pieces.
     *
     * TODO(v0.4.0 F2): for each chunk:
     *   struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, chunk_len);
     *   int rc = ble_gatts_notify_custom(s_priv.conn_handle,
     *                                    s_priv.tx_char_handle, om);
     *   if (rc) { s_priv.lq.tx_err++; return ESP_FAIL; }
     *   s_priv.lq.bytes_tx += chunk_len;
     *
     * Don't forget to append '\n' if line doesn't end in one.
     */
    (void)len;
    return ESP_OK;
#else
    (void)line; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void ble_set_line_callback(transport_t *self,
                                  transport_line_cb_t cb, void *user)
{
    (void)self;
    s_priv.line_cb = cb;
    s_priv.line_cb_user = user;
    /* The actual RX path runs through transport_framer_feed() which
     * uses the framer's globally-registered callback. We keep these
     * locally too in case F2 wants to short-circuit the framer for
     * BLE-specific framing (e.g. binary frames, future). */
}

static transport_state_t ble_state(transport_t *self)
{
    (void)self;
    return s_priv.state;
}

static size_t ble_mtu(transport_t *self)
{
    (void)self;
    return s_priv.mtu_bytes;
}

static void ble_link_quality(transport_t *self, transport_link_quality_t *out)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_BLE
    /* TODO(v0.4.0 F2): pull RSSI via ble_gap_conn_rssi(conn_handle, &out_rssi) */
#endif
    if (out) *out = s_priv.lq;
}

static transport_t s_ble = {
    .name              = "ble_nus",
    .kind              = TRANSPORT_KIND_BLE_NUS,
    .caps              = TRANSPORT_CAPS_BLE_NUS,
    .open              = ble_open,
    .close             = ble_close,
    .write_line        = ble_write_line,
    .set_line_callback = ble_set_line_callback,
    .state             = ble_state,
    .mtu               = ble_mtu,
    .link_quality      = ble_link_quality,
    .priv              = &s_priv,
};

transport_t *transport_ble_nus_instance(void)
{
    return &s_ble;
}

#ifdef CONFIG_TRANSPORT_BLE

/* ─── Stubs of the NimBLE entry points — F2 fills these in ─── */

static esp_err_t nimble_host_init(void)
{
    /* TODO(v0.4.0 F2): see header-comment block above. */
    ESP_LOGW(TAG, "nimble_host_init: TODO");
    return ESP_OK;
}

static esp_err_t gatt_register_nus(void)
{
    /* TODO(v0.4.0 F2): build + register the NUS service. */
    ESP_LOGW(TAG, "gatt_register_nus: TODO");
    return ESP_OK;
}

static esp_err_t gap_start_advertising(void)
{
    /* TODO(v0.4.0 F2): advertise as agentdash-<MAC> with NUS UUID. */
    ESP_LOGW(TAG, "gap_start_advertising: TODO");
    return ESP_OK;
}

/* Real prototypes pulled in by the NimBLE headers — left as comments
 * so this file parses cleanly without those headers. F2 includes
 * "host/ble_hs.h", "host/ble_gap.h", "host/ble_gatt.h" and removes
 * these shims. */

#endif /* CONFIG_TRANSPORT_BLE */
