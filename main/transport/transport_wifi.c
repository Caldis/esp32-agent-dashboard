/*
 * transport_wifi.c — TCP + TLS to dashboard.local:7321 (STUB).
 *
 * v0.4.0 scaffold (TRANS1). NOT YET WIRED INTO BUILD.
 *
 * Wire format: identical to USB-Serial — line-stream, '\n' delimited,
 * same `dash <verb>` / `OK:` / `ERR:` / `EVT:` grammar. No reframing.
 * TLS is just an envelope around the same bytes.
 *
 * Gated by CONFIG_TRANSPORT_WIFI. If not defined, open() returns
 * ESP_ERR_NOT_SUPPORTED and the failover chain skips this transport.
 *
 * Components consumed: esp_wifi, esp_netif, esp_event, esp_tls, mdns.
 * F2 wires those into REQUIRES during build integration.
 *
 * Cred storage:
 *   v0.4.0 (this file): SSID/PSK from sdkconfig defines, baked in
 *   v0.6.0 (SEC1): NVS-encrypted creds, runtime-provisioned via BLE
 */

#include "transport.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "transport_wifi";

#define WIFI_DEFAULT_HOST    "dashboard.local"
#define WIFI_DEFAULT_PORT    7321

typedef struct {
    transport_state_t state;
    transport_link_quality_t lq;
    int  sock_fd;
    void *tls_ctx;                  /* esp_tls_t * — opaque here */
    uint32_t backoff_ms;            /* exponential; cap 30000 */
    transport_line_cb_t line_cb;
    void *line_cb_user;
    char  host[128];
    uint16_t port;
} wifi_priv_t;

static wifi_priv_t s_priv = {
    .state      = TRANSPORT_STATE_DOWN,
    .lq         = { .rssi_dbm = -127, .tx_err = 0, .rx_err = 0,
                    .bytes_tx = 0, .bytes_rx = 0 },
    .sock_fd    = -1,
    .tls_ctx    = NULL,
    .backoff_ms = 1000,
    .line_cb    = NULL,
    .line_cb_user = NULL,
    .host       = WIFI_DEFAULT_HOST,
    .port       = WIFI_DEFAULT_PORT,
};

#ifdef CONFIG_TRANSPORT_WIFI

/* ─── Forward declarations of the WiFi-side entry points F2 fills in ─── */

/* TODO(v0.4.0 F2): WiFi STA bring-up.
 *   • esp_netif_init();
 *   • esp_event_loop_create_default();
 *   • esp_netif_create_default_wifi_sta();
 *   • esp_wifi_init(&cfg);
 *   • esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
 *   • esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);
 *   • esp_wifi_set_mode(WIFI_MODE_STA);
 *   • esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
 *   • esp_wifi_start();
 */
static esp_err_t wifi_sta_start(void);

/* TODO(v0.4.0 F2): mDNS resolve.
 *   • mdns_init();
 *   • mdns_query_a("dashboard", 2000, &addr);
 *   • if resolved, write to s_priv.host and return ESP_OK
 *   • on fail, leave s_priv.host as the configured value (static IP)
 */
static esp_err_t wifi_resolve_host(void);

/* TODO(v0.4.0 F2): TLS connect.
 *   • esp_tls_cfg_t cfg = { .cacert_buf = ..., .cacert_bytes = ... };
 *   • s_priv.tls_ctx = esp_tls_init();
 *   • esp_tls_conn_new_sync(s_priv.host, strlen, s_priv.port, &cfg, s_priv.tls_ctx);
 *   • on success: state = READY, reset backoff_ms = 1000
 *   • on fail: bump backoff, state = BACKOFF
 *
 * Cert pinning: v0.4.0 bakes one CA pubkey into .rodata. SEC1 (v0.6.0)
 * replaces this with NVS-stored creds + rotation.
 */
static esp_err_t wifi_tls_connect(void);

/* TODO(v0.4.0 F2): RX task.
 *   • read up to 512 bytes per recv
 *   • transport_framer_feed(buf, n)
 *   • on EOF / TLS error → close socket, state = BACKOFF, kick reconnect
 */
static void wifi_rx_task(void *arg);

/* TODO(v0.4.0 F2): WiFi+IP event handler. */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data);

#endif /* CONFIG_TRANSPORT_WIFI */

/* ─── Vtable impl ─── */

static esp_err_t wifi_open(transport_t *self)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_WIFI
    if (s_priv.state != TRANSPORT_STATE_DOWN) return ESP_OK;
    s_priv.state = TRANSPORT_STATE_OPENING;

    esp_err_t err = wifi_sta_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi STA start failed: %s", esp_err_to_name(err));
        s_priv.state = TRANSPORT_STATE_BACKOFF;
        return err;
    }
    /* state transitions to READY happen in wifi_tls_connect() after
     * IP_EVENT_STA_GOT_IP fires; open() returning ESP_OK just means
     * the STA bring-up was initiated. */
    return ESP_OK;
#else
    ESP_LOGW(TAG, "CONFIG_TRANSPORT_WIFI not set — WiFi transport unavailable");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t wifi_close(transport_t *self)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_WIFI
    s_priv.state = TRANSPORT_STATE_CLOSING;
    /* TODO(v0.4.0 F2):
     *   • esp_tls_conn_destroy(s_priv.tls_ctx); s_priv.tls_ctx = NULL;
     *   • close(s_priv.sock_fd); s_priv.sock_fd = -1;
     *   • esp_wifi_stop(); // optional — keep WiFi up if other consumers need it
     */
    s_priv.state = TRANSPORT_STATE_DOWN;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t wifi_write_line(transport_t *self, const char *line, size_t len)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_WIFI
    if (s_priv.state != TRANSPORT_STATE_READY) return ESP_ERR_INVALID_STATE;
    if (!line) return ESP_ERR_INVALID_ARG;

    /* TODO(v0.4.0 F2):
     *   ssize_t n = esp_tls_conn_write(s_priv.tls_ctx, line, len);
     *   if (n < 0) { s_priv.lq.tx_err++; reconnect_async(); return ESP_FAIL; }
     *   s_priv.lq.bytes_tx += n;
     *   if (len == 0 || line[len-1] != '\n') esp_tls_conn_write(s_priv.tls_ctx, "\n", 1);
     */
    (void)len;
    return ESP_OK;
#else
    (void)line; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void wifi_set_line_callback(transport_t *self,
                                   transport_line_cb_t cb, void *user)
{
    (void)self;
    s_priv.line_cb = cb;
    s_priv.line_cb_user = user;
    /* See transport_ble_nus.c note on the framer being the canonical
     * inbound path; this is a per-transport hook for future use. */
}

static transport_state_t wifi_state(transport_t *self)
{
    (void)self;
    return s_priv.state;
}

static size_t wifi_mtu(transport_t *self)
{
    (void)self;
    /* TCP is byte-stream; report unlimited. TLS record size (~16 KB)
     * is way above CONSOLE_MAX_LINE = 1024 so it's never the limiter. */
    return SIZE_MAX;
}

static void wifi_link_quality(transport_t *self, transport_link_quality_t *out)
{
    (void)self;
#ifdef CONFIG_TRANSPORT_WIFI
    /* TODO(v0.4.0 F2): pull RSSI via esp_wifi_sta_get_ap_info(&ap_info)
     * and stuff ap_info.rssi into s_priv.lq.rssi_dbm before returning. */
#endif
    if (out) *out = s_priv.lq;
}

static transport_t s_wifi = {
    .name              = "wifi_tls",
    .kind              = TRANSPORT_KIND_WIFI_TLS,
    .caps              = TRANSPORT_CAPS_WIFI_TLS,
    .open              = wifi_open,
    .close             = wifi_close,
    .write_line        = wifi_write_line,
    .set_line_callback = wifi_set_line_callback,
    .state             = wifi_state,
    .mtu               = wifi_mtu,
    .link_quality      = wifi_link_quality,
    .priv              = &s_priv,
};

transport_t *transport_wifi_instance(void)
{
    return &s_wifi;
}

#ifdef CONFIG_TRANSPORT_WIFI

/* ─── Stubs of the esp_wifi/esp_tls entry points — F2 fills in ─── */

static esp_err_t wifi_sta_start(void)
{
    /* TODO(v0.4.0 F2): see comment block above. */
    ESP_LOGW(TAG, "wifi_sta_start: TODO");
    return ESP_OK;
}

static esp_err_t wifi_resolve_host(void)
{
    /* TODO(v0.4.0 F2): mDNS resolve dashboard.local; fall back to NVS
     * static IP if mDNS fails. */
    ESP_LOGW(TAG, "wifi_resolve_host: TODO");
    return ESP_OK;
}

static esp_err_t wifi_tls_connect(void)
{
    /* TODO(v0.4.0 F2): esp_tls handshake. On success start the RX task
     * and flip state to READY. On fail bump backoff_ms (exponential,
     * cap 30000) and schedule a retry. */
    ESP_LOGW(TAG, "wifi_tls_connect: TODO");
    return ESP_OK;
}

static void wifi_rx_task(void *arg)
{
    /* TODO(v0.4.0 F2):
     *   uint8_t buf[512];
     *   for (;;) {
     *       int n = esp_tls_conn_read(s_priv.tls_ctx, buf, sizeof(buf));
     *       if (n <= 0) { reconnect_async(); break; }
     *       s_priv.lq.bytes_rx += n;
     *       transport_framer_feed(buf, (size_t)n);
     *   }
     */
    (void)arg;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    /* TODO(v0.4.0 F2): handle WIFI_EVENT_STA_START → esp_wifi_connect();
     * WIFI_EVENT_STA_DISCONNECTED → schedule retry; IP_EVENT_STA_GOT_IP
     * → wifi_resolve_host() then wifi_tls_connect(). */
}

#endif /* CONFIG_TRANSPORT_WIFI */
