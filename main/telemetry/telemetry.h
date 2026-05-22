/*
 * telemetry — on-device rolling stats ring.
 *
 * Samples (heap_free, fps, scene_idx, agent_count) every
 * TELEMETRY_SAMPLE_PERIOD_MS into a fixed-size ring of
 * TELEMETRY_RING_LEN entries. Provides aggregate accessors
 * (p50/p95, min/max) and a JSONL dump of the ring contents
 * for `dash health --jsonl`.
 *
 * Default sample period is 30 s; ring length 60 → ~30 min window.
 *
 * This is the *local* observability primitive. It is consulted by
 * the bridge to produce the 6-hourly telemetry envelope documented
 * in docs/TELEMETRY_SPEC.md. The ring itself contains no PII:
 * heap_free is a byte count, fps is a small integer, scene_idx is
 * an enum (rendered as a scene id string on dump), agent_count is
 * a small integer. No cwd, no msg, no session_id.
 *
 * Thread-safety: telemetry_sample_now() is callable from any task
 * (timer ISR, scene callbacks, console handler). All accessors take
 * an internal mutex.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TELEMETRY_RING_LEN
#define TELEMETRY_RING_LEN          60          /* samples */
#endif

#ifndef TELEMETRY_SAMPLE_PERIOD_MS
#define TELEMETRY_SAMPLE_PERIOD_MS  30000u      /* 30 s */
#endif

#ifndef TELEMETRY_SCENE_ID_MAX
#define TELEMETRY_SCENE_ID_MAX      16
#endif

typedef struct {
    uint32_t t_uptime_ms;       /* lv_tick_get() at sample time          */
    uint32_t heap_free;         /* esp_get_free_heap_size()              */
    uint16_t fps;               /* harness frame rate at sample time     */
    uint16_t agent_count;       /* agent_state.slot_count                */
    char     scene_id[TELEMETRY_SCENE_ID_MAX]; /* current scene id       */
} telemetry_sample_t;

typedef struct {
    /* Number of valid samples (0..TELEMETRY_RING_LEN).
     * Once at TELEMETRY_RING_LEN, subsequent samples roll the ring. */
    uint16_t count;
    uint32_t heap_free_min;
    uint32_t heap_free_p50;
    uint16_t fps_p50;
    uint16_t fps_p95;
    uint16_t agent_count_p50;
    uint16_t agent_count_p95;
    uint32_t uptime_min_ms;
    uint32_t uptime_max_ms;
} telemetry_stats_t;

/* Set up the mutex and zero the ring. Safe to call once at startup,
 * after esp_log + agent_state are ready. */
void telemetry_init(void);

/* Capture one sample using *current* device state. Call from a
 * periodic timer (the recommended driver is lv_timer with period
 * TELEMETRY_SAMPLE_PERIOD_MS). Cheap: ~20 µs, no allocations. */
void telemetry_sample_now(void);

/* Compute rolling stats over the ring. Returns true if at least
 * one sample is present. */
bool telemetry_get_stats(telemetry_stats_t *out);

/* Append a JSONL representation of every valid sample to `buf`.
 * Returns the number of bytes written (excluding NUL), or -1 if
 * the buffer is too small. Always NUL-terminates on success.
 *
 * Format (one line per sample, oldest first):
 *   {"t":<uptime_ms>,"heap_free":N,"fps":N,"scene":"<id>","agents":N}\n
 *
 * Sized for ~80 bytes/sample → TELEMETRY_RING_LEN * 96 byte buffer
 * is sufficient for safety. */
int telemetry_dump_jsonl(char *buf, size_t buf_size);

/* Reset the ring. Used after a `dash dump crash` to clear stale
 * pre-crash samples (optional). */
void telemetry_clear(void);

/* Bump a frame counter feeding the fps field of each sample.
 *
 * The upstream esp-harness ?stat command keeps a private fps cache
 * that we can't read from here, so the dashboard maintains its own
 * counter. Call this once per LVGL frame — the existing frame_cb
 * in esp32_agent_dashboard_main.c is the canonical hookpoint:
 *
 *   static void frame_cb(lv_timer_t *t) {
 *       (void)t;
 *       harness_record_frame();
 *       telemetry_record_frame();
 *   }
 *
 * If never called, every sample's `fps` field is 0 and the stats'
 * fps_p50/p95 are 0. This is documented in HARNESS_GAPS as a request
 * to expose harness_get_fps() upstream so we don't need a parallel
 * counter. */
void telemetry_record_frame(void);

#ifdef __cplusplus
}
#endif
