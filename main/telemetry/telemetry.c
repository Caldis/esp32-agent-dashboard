/*
 * telemetry.c — on-device rolling stats ring.
 *
 * Sampler runs on whatever timer the caller wired up (recommended:
 * an lv_timer at TELEMETRY_SAMPLE_PERIOD_MS). The samples capture
 * heap_free, our local fps counter, the current scene id, and the
 * agent_state slot count. No PII — see docs/TELEMETRY_SPEC.md §3.
 *
 * Storage: a fixed-size ring with two indices (head, count). When
 * count hits TELEMETRY_RING_LEN, head rolls and we keep overwriting
 * the oldest slot. count never exceeds TELEMETRY_RING_LEN.
 *
 * Aggregates: we don't keep an O(1) running median because the cost
 * of sorting 60 ints in a 30-second-cadence path is irrelevant
 * (~5 µs). When stats are requested, snapshot the ring under the
 * mutex, then sort + percentile outside the lock.
 */

#include "telemetry.h"

#include "../agent_state.h"
#include "harness/scene_framework.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "telemetry";

/* ── Ring + mutex ───────────────────────────────────────────────── */

static telemetry_sample_t s_ring[TELEMETRY_RING_LEN];
static uint16_t           s_head;       /* index of *next* write */
static uint16_t           s_count;      /* number of valid entries */
static SemaphoreHandle_t  s_lock;

/* ── Local FPS counter (see header for rationale) ───────────────── */

static volatile uint32_t s_frame_count;
static volatile uint32_t s_last_fps_tick_ms;
static volatile uint32_t s_last_fps_count;
static volatile uint16_t s_fps_cached;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* ── Public API ─────────────────────────────────────────────────── */

void telemetry_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return;
    }
    memset(s_ring, 0, sizeof(s_ring));
    s_head = s_count = 0;
    s_frame_count = s_last_fps_count = 0;
    s_last_fps_tick_ms = lv_tick_get();
    s_fps_cached = 0;
    ESP_LOGI(TAG, "init: ring=%d samples, period=%u ms",
             TELEMETRY_RING_LEN, (unsigned)TELEMETRY_SAMPLE_PERIOD_MS);
}

void telemetry_record_frame(void)
{
    s_frame_count++;
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_last_fps_tick_ms;
    if (elapsed >= 500u) {
        uint32_t delta = s_frame_count - s_last_fps_count;
        uint32_t fps = (delta * 1000u + (elapsed / 2)) / elapsed;
        if (fps > 0xFFFFu) fps = 0xFFFFu;
        s_fps_cached = (uint16_t)fps;
        s_last_fps_count = s_frame_count;
        s_last_fps_tick_ms = now;
    }
}

void telemetry_clear(void)
{
    lock();
    memset(s_ring, 0, sizeof(s_ring));
    s_head = s_count = 0;
    unlock();
}

void telemetry_sample_now(void)
{
    if (!s_lock) return;

    telemetry_sample_t s = {0};
    s.t_uptime_ms = lv_tick_get();
    s.heap_free   = (uint32_t)esp_get_free_heap_size();
    s.fps         = s_fps_cached;

    /* Scene id — outside the agent_state lock; scene_fw_current is
     * snapshot-safe (pointer to static scene_t). */
    const scene_t *cur = scene_fw_current();
    const char *id = (cur && cur->id) ? cur->id : "?";
    size_t n = strnlen(id, TELEMETRY_SCENE_ID_MAX - 1);
    memcpy(s.scene_id, id, n);
    s.scene_id[n] = '\0';

    /* Agent count — take the agent_state lock briefly. */
    agent_state_lock();
    s.agent_count = (uint16_t)agent_state_get()->slot_count;
    agent_state_unlock();

    lock();
    s_ring[s_head] = s;
    s_head = (s_head + 1) % TELEMETRY_RING_LEN;
    if (s_count < TELEMETRY_RING_LEN) s_count++;
    unlock();
}

/* ── Stats helpers ──────────────────────────────────────────────── */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t pct_u32(uint32_t *arr, uint16_t n, int pct)
{
    if (n == 0) return 0;
    int idx = (int)((uint32_t)pct * (n - 1) / 100u);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return arr[idx];
}

bool telemetry_get_stats(telemetry_stats_t *out)
{
    if (!out || !s_lock) return false;

    /* Copy ring under lock, then sort + percentile outside. */
    static uint32_t heap_buf[TELEMETRY_RING_LEN];
    static uint32_t fps_buf [TELEMETRY_RING_LEN];
    static uint32_t agent_buf[TELEMETRY_RING_LEN];
    static uint32_t up_buf  [TELEMETRY_RING_LEN];
    uint16_t n;

    lock();
    n = s_count;
    if (n == 0) { unlock(); memset(out, 0, sizeof(*out)); return false; }
    /* Walk oldest → newest. When count == ring, head points at the
     * oldest slot too. */
    uint16_t start = (s_count < TELEMETRY_RING_LEN)
                     ? 0
                     : s_head;
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t idx = (start + i) % TELEMETRY_RING_LEN;
        heap_buf[i]  = s_ring[idx].heap_free;
        fps_buf[i]   = s_ring[idx].fps;
        agent_buf[i] = s_ring[idx].agent_count;
        up_buf[i]    = s_ring[idx].t_uptime_ms;
    }
    unlock();

    qsort(heap_buf,  n, sizeof(uint32_t), cmp_u32);
    qsort(fps_buf,   n, sizeof(uint32_t), cmp_u32);
    qsort(agent_buf, n, sizeof(uint32_t), cmp_u32);

    out->count            = n;
    out->heap_free_min    = heap_buf[0];
    out->heap_free_p50    = pct_u32(heap_buf, n, 50);
    out->fps_p50          = (uint16_t)pct_u32(fps_buf, n, 50);
    /* p95 fps: lower bound is "bad" — we want the 5th percentile
     * here (slowest 5% of frames). Document this in OBSERVABILITY. */
    out->fps_p95          = (uint16_t)pct_u32(fps_buf, n, 5);
    out->agent_count_p50  = (uint16_t)pct_u32(agent_buf, n, 50);
    out->agent_count_p95  = (uint16_t)pct_u32(agent_buf, n, 95);

    /* uptime min/max from unsorted ring snapshot. */
    uint32_t umin = up_buf[0], umax = up_buf[0];
    for (uint16_t i = 1; i < n; ++i) {
        if (up_buf[i] < umin) umin = up_buf[i];
        if (up_buf[i] > umax) umax = up_buf[i];
    }
    out->uptime_min_ms = umin;
    out->uptime_max_ms = umax;
    return true;
}

/* ── JSONL dump ─────────────────────────────────────────────────── */

int telemetry_dump_jsonl(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || !s_lock) return -1;

    /* Snapshot the ring under lock into a local copy so we hold
     * the mutex only briefly. */
    static telemetry_sample_t copy[TELEMETRY_RING_LEN];
    uint16_t n;

    lock();
    n = s_count;
    uint16_t start = (s_count < TELEMETRY_RING_LEN) ? 0 : s_head;
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t idx = (start + i) % TELEMETRY_RING_LEN;
        copy[i] = s_ring[idx];
    }
    unlock();

    size_t off = 0;
    for (uint16_t i = 0; i < n; ++i) {
        int w = snprintf(buf + off, buf_size - off,
            "{\"t\":%" PRIu32 ",\"heap_free\":%" PRIu32 ","
            "\"fps\":%u,\"scene\":\"%s\",\"agents\":%u}\n",
            copy[i].t_uptime_ms, copy[i].heap_free,
            (unsigned)copy[i].fps,
            copy[i].scene_id,
            (unsigned)copy[i].agent_count);
        if (w < 0 || (size_t)w >= buf_size - off) {
            /* Truncate cleanly: emit what we have, return -1. */
            buf[off] = '\0';
            return -1;
        }
        off += (size_t)w;
    }
    if (off < buf_size) buf[off] = '\0';
    return (int)off;
}
