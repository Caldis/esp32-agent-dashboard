/*
 * ai_summarise — v1.9.0 on-device LLM summariser, public C API.
 *
 * Turns the last N=10 entries of an agent_slot_t's transcript into a
 * single short line suitable for the AGENT_MSG_MAX-bounded `msg` field
 * rendered on scene_dashboard.
 *
 * The summariser runs entirely on-device (no network, no host bridge
 * round-trip) — see docs/ON_DEVICE_AI.md and docs/MODEL_CARD.md for
 * the architecture, model selection, and privacy story.
 *
 * Build gating:
 *   This whole module is a no-op stub unless CONFIG_AI_SUMMARISE is
 *   enabled in sdkconfig. The default build (v1.8.x and earlier) is
 *   unaffected — ai_summarise_init() returns false and ai_summarise()
 *   returns a short placeholder. Once CONFIG_AI_SUMMARISE lands in
 *   Kconfig (out of scope for the AI1 scaffolding cycle), real
 *   inference takes over.
 *
 * Threading:
 *   The intended runtime path runs on a dedicated FreeRTOS task with:
 *     - name        : "ai_summarise"
 *     - priority    : tskIDLE_PRIORITY + 1  (below LVGL, below console)
 *     - stack       : 16 KB
 *     - core        : pinned to APP_CPU (core 1) so PRO_CPU (core 0)
 *                     stays unblocked for console + display
 *   The current stub skips the task and answers inline on the caller's
 *   stack so smoke tests still see a deterministic reply.
 *
 * Heap / PSRAM:
 *   On real init the module allocates the model resident block out of
 *   PSRAM via heap_caps_malloc(MALLOC_CAP_SPIRAM). The stub allocates
 *   nothing. See ON_DEVICE_AI.md "Memory budget" for the full table.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of the input transcript blob accepted by ai_summarise().
 * Sized to comfortably hold AGENT_ENTRY_COUNT * AGENT_ENTRY_TEXT_MAX
 * plus a small per-entry header. Callers exceeding this get a clean
 * truncation, never a crash. */
#define AI_SUMMARISE_INPUT_MAX   1024

/* Conservative upper bound on the output line length. The caller's
 * out_len argument may be smaller — the function will honor whichever
 * is smaller. Aligns with AGENT_MSG_MAX (=128) in agent_state.h. */
#define AI_SUMMARISE_OUTPUT_MAX  128

/* Initialise the summariser.
 *
 * model_blob_addr / model_blob_size: pointer + size of the mmaped
 *   model partition. Pass NULL/0 to defer model load (the stub does
 *   this — see implementation note).
 *
 * Returns true on success. False means the module disabled itself and
 * callers should fall back to the host-supplied msg. Specifically:
 *   - CONFIG_AI_SUMMARISE is off
 *   - model_blob is NULL/empty
 *   - PSRAM allocation for the model resident block failed
 *   - model SHA-256 doesn't match tools/ai/model.bin.sha256
 *
 * Safe to call once at boot. Subsequent calls are no-ops returning
 * the previous result.
 */
bool ai_summarise_init(const void *model_blob_addr, size_t model_blob_size);

/* Summarise a transcript blob into a single line.
 *
 * transcript:   NUL-terminated input; format is
 *                 "[role] [tool] [text]\n[role] [tool] [text]\n..."
 *               one line per agent_entry_t, reverse-chronological,
 *               up to AGENT_ENTRY_COUNT entries. Newlines separate
 *               entries; the trailing newline is optional. The
 *               caller is responsible for redacting sensitive
 *               substrings (see docs/MODEL_CARD.md "Out of scope").
 *
 * out_msg:      caller-supplied buffer; will be NUL-terminated on
 *               return. Output is truncated at the last word boundary
 *               under out_len-1 (or AI_SUMMARISE_OUTPUT_MAX, whichever
 *               is smaller).
 *
 * out_len:      capacity of out_msg in bytes (including the NUL).
 *
 * Returns true on success. False means the call timed out, the module
 * was never initialised, or out_msg/out_len were invalid; out_msg is
 * always NUL-terminated even on failure (set to "" or a fallback
 * string) so callers can render it without further checks.
 *
 * Latency budget: < 2 s wall-clock on a successfully-initialised
 * module. The stub answers in microseconds.
 *
 * Safe to call from any FreeRTOS task. NOT safe to call from an ISR.
 * Internally serialises calls via a mutex so two scenes asking for
 * a summary at the same tick won't race.
 */
bool ai_summarise(const char *transcript, char *out_msg, size_t out_len);

/* Diagnostics — used by `dash health` to surface "is the summariser
 * up and how is it doing". Zero-cost when the stub is in play.
 */
typedef struct {
    bool     initialised;          /* matches return value of last init call */
    uint32_t calls_total;          /* lifetime ai_summarise() invocations */
    uint32_t calls_succeeded;
    uint32_t calls_timed_out;
    uint32_t last_latency_ms;      /* wall-clock of most recent call */
    uint32_t max_latency_ms;       /* watermark */
    size_t   model_resident_bytes; /* PSRAM bytes currently held by model */
} ai_summarise_stats_t;

/* Read a snapshot of the diagnostics. Thread-safe; no lock required
 * by the caller (the module copies its internal counters under its
 * own mutex). */
void ai_summarise_get_stats(ai_summarise_stats_t *out);

#ifdef __cplusplus
}
#endif
