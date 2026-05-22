/*
 * crash_dump.c — NVS-backed crash record + shutdown hook.
 *
 * Storage: `crashdump` NVS namespace, single blob key `last`.
 * Auxiliary key `boot_count` (uint32) increments on every boot;
 * `dump_present` (uint8) is the simple sentinel checked at init.
 *
 * Why a blob and not individual keys: a panic context is severely
 * constrained — fewer NVS ops = higher chance the dump actually
 * lands. One nvs_set_blob + nvs_commit is the cheapest path.
 *
 * The panic-handler hook is registered via
 * esp_register_shutdown_handler(). This handler runs when esp_restart
 * is called; for a true panic the panic handler calls esp_restart
 * after printing the panic dump, so our shutdown hook fires too —
 * provided no earlier shutdown hook calls abort(). On ESP-IDF v6
 * that's the case for the dashboard build.
 *
 * Gap: this does NOT survive a brownout reset (no time to write
 * NVS). For that we'd need a small static region in RTC memory
 * with checksum, which is out of scope for v0.9.0. Recorded in
 * HARNESS_GAPS as a follow-up.
 */

#include "crash_dump.h"
#include "telemetry.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "harness/console_protocol.h"

static const char *TAG          = "crash_dump";
#define NVS_NS                  "crashdump"
#define NVS_KEY_BLOB            "last"
#define NVS_KEY_BOOT_COUNT      "boot_count"
#define NVS_KEY_PRESENT         "present"

/* Build-time firmware version. Provided via -DDASH_FW_VERSION=... at
 * CMake; defaults to "0.0.0-dev" so file compiles standalone. */
#ifndef DASH_FW_VERSION
#define DASH_FW_VERSION "0.0.0-dev"
#endif

/* ── In-RAM state ───────────────────────────────────────────────── */

static crash_record_t s_cached;        /* pending dump (loaded at boot) */
static bool           s_have_cached;
static int            s_cached_json_bytes;

static char     s_evt_ring[CRASH_EVT_TAIL_LEN][CRASH_EVT_TEXT_MAX];
static uint16_t s_evt_head;            /* next write index */
static uint16_t s_evt_count;
static atomic_flag s_evt_lock = ATOMIC_FLAG_INIT;

/* ── EVT ring (single-writer; non-blocking) ─────────────────────── */

static inline void evt_lock(void)
{
    while (atomic_flag_test_and_set(&s_evt_lock)) {
        /* spin — contention is rare since EVT writes are slow path */
    }
}
static inline void evt_unlock(void) { atomic_flag_clear(&s_evt_lock); }

void crash_dump_record_evt(const char *line)
{
    if (!line) return;
    evt_lock();
    size_t n = strnlen(line, CRASH_EVT_TEXT_MAX - 1);
    memcpy(s_evt_ring[s_evt_head], line, n);
    s_evt_ring[s_evt_head][n] = '\0';
    s_evt_head = (s_evt_head + 1) % CRASH_EVT_TAIL_LEN;
    if (s_evt_count < CRASH_EVT_TAIL_LEN) s_evt_count++;
    evt_unlock();
}

static void evt_ring_snapshot(crash_record_t *r)
{
    evt_lock();
    uint16_t start = (s_evt_count < CRASH_EVT_TAIL_LEN) ? 0 : s_evt_head;
    r->evt_tail_count = s_evt_count;
    for (uint16_t i = 0; i < s_evt_count; ++i) {
        uint16_t idx = (start + i) % CRASH_EVT_TAIL_LEN;
        memcpy(r->evt_tail[i], s_evt_ring[idx], CRASH_EVT_TEXT_MAX);
    }
    evt_unlock();
}

/* ── NVS helpers ────────────────────────────────────────────────── */

static esp_err_t nvs_open_rw(nvs_handle_t *h)
{
    return nvs_open(NVS_NS, NVS_READWRITE, h);
}
static esp_err_t nvs_open_ro(nvs_handle_t *h)
{
    return nvs_open(NVS_NS, NVS_READONLY, h);
}

static uint32_t read_boot_count_and_bump(void)
{
    nvs_handle_t h;
    uint32_t bc = 0;
    if (nvs_open_rw(&h) != ESP_OK) return 0;
    if (nvs_get_u32(h, NVS_KEY_BOOT_COUNT, &bc) != ESP_OK) bc = 0;
    bc++;
    nvs_set_u32(h, NVS_KEY_BOOT_COUNT, bc);
    nvs_commit(h);
    nvs_close(h);
    return bc;
}

static bool load_pending(crash_record_t *out)
{
    nvs_handle_t h;
    if (nvs_open_ro(&h) != ESP_OK) return false;
    uint8_t present = 0;
    if (nvs_get_u8(h, NVS_KEY_PRESENT, &present) != ESP_OK || !present) {
        nvs_close(h);
        return false;
    }
    size_t sz = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_BLOB, out, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(*out)) return false;
    return true;
}

static void clear_pending(void)
{
    nvs_handle_t h;
    if (nvs_open_rw(&h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY_BLOB);
    nvs_set_u8(h, NVS_KEY_PRESENT, 0);
    nvs_commit(h);
    nvs_close(h);
}

/* ── JSON encoder ───────────────────────────────────────────────── */

/* Escape a string into a JSON literal segment. Returns chars written
 * including the surrounding double quotes, or -1 on overflow. */
static int json_escape(char *dst, size_t dst_size, const char *src)
{
    if (dst_size < 3) return -1;
    size_t o = 0;
    dst[o++] = '"';
    for (const char *p = src; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        const char *esc = NULL;
        char buf[8];
        if      (c == '"')  esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20)  { snprintf(buf, sizeof(buf), "\\u%04x", c); esc = buf; }
        if (esc) {
            size_t n = strlen(esc);
            if (o + n + 2 > dst_size) return -1;
            memcpy(dst + o, esc, n); o += n;
        } else {
            if (o + 2 > dst_size) return -1;
            dst[o++] = (char)c;
        }
    }
    dst[o++] = '"';
    if (o >= dst_size) return -1;
    dst[o] = '\0';
    return (int)o;
}

static int encode_record(const crash_record_t *r, char *buf, size_t buf_size)
{
    /* Escape strings first into small stack scratch buffers. */
    char esc_schema[40], esc_fw[40], esc_reason[64], esc_ptext[64], esc_task[40];
    if (json_escape(esc_schema, sizeof(esc_schema), r->schema) < 0) return -1;
    if (json_escape(esc_fw,     sizeof(esc_fw),     r->fw_version) < 0) return -1;
    if (json_escape(esc_reason, sizeof(esc_reason), r->reason) < 0) return -1;
    if (json_escape(esc_ptext,  sizeof(esc_ptext),  r->panic_text) < 0) return -1;
    if (json_escape(esc_task,   sizeof(esc_task),   r->task_name) < 0) return -1;

    int n = snprintf(buf, buf_size,
        "{\"schema\":%s,\"fw_version\":%s,"
        "\"boot_count_at_crash\":%" PRIu32 ","
        "\"uptime_s_at_crash\":%" PRIu32 ","
        "\"reason\":%s,\"panic_reason_code\":%" PRId32 ","
        "\"panic_excvaddr\":\"0x%08" PRIx32 "\","
        "\"panic_text\":%s,\"task_name\":%s,"
        "\"stack_pc\":\"0x%08" PRIx32 "\","
        "\"heap_free_at_crash\":%" PRIu32 ","
        "\"evt_tail\":[",
        esc_schema, esc_fw,
        r->boot_count_at_crash, r->uptime_s_at_crash,
        esc_reason, r->panic_reason_code,
        r->panic_excvaddr,
        esc_ptext, esc_task,
        r->stack_pc,
        r->heap_free_at_crash);
    if (n < 0 || (size_t)n >= buf_size) return -1;

    size_t off = (size_t)n;
    for (uint16_t i = 0; i < r->evt_tail_count; ++i) {
        char esc_line[CRASH_EVT_TEXT_MAX + 8];
        if (json_escape(esc_line, sizeof(esc_line), r->evt_tail[i]) < 0) return -1;
        int w = snprintf(buf + off, buf_size - off,
                         "%s%s", (i == 0) ? "" : ",", esc_line);
        if (w < 0 || (size_t)w >= buf_size - off) return -1;
        off += (size_t)w;
    }
    int w = snprintf(buf + off, buf_size - off, "]}");
    if (w < 0 || (size_t)w >= buf_size - off) return -1;
    off += (size_t)w;
    return (int)off;
}

/* ── Public API ─────────────────────────────────────────────────── */

void crash_dump_capture(const char *reason,
                        int32_t panic_reason_code,
                        const char *panic_text,
                        const char *task_name,
                        uint32_t stack_pc,
                        uint32_t panic_excvaddr)
{
    crash_record_t r;
    memset(&r, 0, sizeof(r));

    strncpy(r.schema, "dash.crashdump/v1", sizeof(r.schema) - 1);
    strncpy(r.fw_version, DASH_FW_VERSION, sizeof(r.fw_version) - 1);

    /* Boot count was bumped at init; re-read for accuracy. */
    nvs_handle_t h;
    if (nvs_open_ro(&h) == ESP_OK) {
        uint32_t bc = 0;
        nvs_get_u32(h, NVS_KEY_BOOT_COUNT, &bc);
        nvs_close(h);
        r.boot_count_at_crash = bc;
    }

    r.uptime_s_at_crash = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    if (reason) strncpy(r.reason, reason, sizeof(r.reason) - 1);
    else        strncpy(r.reason, "unknown", sizeof(r.reason) - 1);
    r.panic_reason_code = panic_reason_code;
    if (panic_text) strncpy(r.panic_text, panic_text, sizeof(r.panic_text) - 1);
    if (task_name)  strncpy(r.task_name,  task_name,  sizeof(r.task_name)  - 1);
    r.stack_pc       = stack_pc;
    r.panic_excvaddr = panic_excvaddr;
    r.heap_free_at_crash = (uint32_t)esp_get_free_heap_size();

    evt_ring_snapshot(&r);

    /* Persist. Best effort — no error path beyond logging since we're
     * about to reset. */
    nvs_handle_t hw;
    if (nvs_open_rw(&hw) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed during capture");
        return;
    }
    nvs_set_blob(hw, NVS_KEY_BLOB, &r, sizeof(r));
    nvs_set_u8(hw, NVS_KEY_PRESENT, 1);
    nvs_commit(hw);
    nvs_close(hw);
    ESP_LOGW(TAG, "crash dump persisted, reason=%s", r.reason);
}

/* esp_register_shutdown_handler() takes a void(void) callback.
 * We can't pass arguments, so this hook recovers what it can from
 * esp_reset_reason() + the current task name. */
static void shutdown_hook(void)
{
    esp_reset_reason_t rr = esp_reset_reason();

    /* Only persist a dump for *unusual* shutdowns. A clean
     * esp_restart() from `?reset` shouldn't litter the namespace. */
    const char *reason_name = "unknown";
    bool persist = false;
    switch (rr) {
        case ESP_RST_PANIC:    reason_name = "panic";     persist = true; break;
        case ESP_RST_INT_WDT:  reason_name = "int_wdt";   persist = true; break;
        case ESP_RST_TASK_WDT: reason_name = "task_wdt";  persist = true; break;
        case ESP_RST_WDT:      reason_name = "wdt";       persist = true; break;
        case ESP_RST_BROWNOUT: reason_name = "brownout";  persist = true; break;
        default: return;       /* clean shutdown — nothing to do */
    }
    if (!persist) return;

    crash_dump_capture(reason_name,
                       (int32_t)rr,
                       reason_name,    /* panic_text — we don't have IDF's verbose text here */
                       "unknown",      /* task name not available in shutdown hook */
                       0u, 0u);
}

bool crash_dump_init(void)
{
    s_have_cached = false;
    s_cached_json_bytes = 0;

    /* Bump boot count. */
    read_boot_count_and_bump();

    /* Try to load a pending dump. */
    if (load_pending(&s_cached)) {
        s_have_cached = true;
        /* Pre-encode to know the byte count for the EVT line. */
        static char encbuf[2048];
        s_cached_json_bytes = encode_record(&s_cached, encbuf, sizeof(encbuf));
        if (s_cached_json_bytes > 0) {
            console_send_evt("crash_dump_available bytes=%d", s_cached_json_bytes);
            ESP_LOGW(TAG, "pending dump from previous boot, %d JSON bytes",
                     s_cached_json_bytes);
        } else {
            ESP_LOGE(TAG, "pending dump load failed to re-encode; clearing");
            clear_pending();
            s_have_cached = false;
        }
    }

    /* Register the shutdown hook so panics get captured. ESP-IDF
     * silently caps the total number of hooks at 5 — that's fine. */
    esp_err_t err = esp_register_shutdown_handler(shutdown_hook);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_shutdown_handler failed: %d", err);
    }

    return s_have_cached;
}

bool crash_dump_available(void) { return s_have_cached; }
int  crash_dump_pending_bytes(void) { return s_have_cached ? s_cached_json_bytes : 0; }

int crash_dump_to_json(char *buf, size_t buf_size)
{
    if (!s_have_cached) return -1;
    return encode_record(&s_cached, buf, buf_size);
}

int crash_dump_emit_and_clear(void)
{
    if (!s_have_cached) {
        console_reply_ok("{\"crash_dump\":\"none\"}");
        return 0;
    }
    static char encbuf[2048];
    int n = encode_record(&s_cached, encbuf, sizeof(encbuf));
    if (n <= 0) {
        console_reply_err("encode failed");
        clear_pending();
        s_have_cached = false;
        s_cached_json_bytes = 0;
        return -1;
    }

    char meta[64];
    snprintf(meta, sizeof(meta), "fmt=json bytes=%d", n);
    console_reply_ok("payload follows tag=CRASHDUMP");
    console_begin_payload("CRASHDUMP", meta);
    console_write_raw(encbuf, (size_t)n);
    console_end_payload("CRASHDUMP");

    /* Clear after successful emit. */
    clear_pending();
    s_have_cached = false;
    s_cached_json_bytes = 0;
    return n;
}
