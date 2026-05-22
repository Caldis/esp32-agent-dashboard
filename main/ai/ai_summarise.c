/*
 * ai_summarise — v1.9.0 stub implementation.
 *
 * This is intentionally a no-op. The shape of the C API (header) is
 * the deliverable for the AI1 scaffolding cycle; the real inference
 * engine lands in the v1.9.0 ship cycle. See docs/ON_DEVICE_AI.md.
 *
 * What the stub does today:
 *   - ai_summarise_init() validates the model blob pointer/size and
 *     returns true if (and only if) CONFIG_AI_SUMMARISE is on AND a
 *     non-empty blob was passed. Otherwise it returns false and the
 *     module remains "disabled" — callers see false and fall back to
 *     the host-supplied msg.
 *   - ai_summarise() returns the literal string "summary unavailable"
 *     (truncated to out_len-1) and logs a single ESP_LOGI line the
 *     first time it's called per boot. Counter tracking still works
 *     so smoke tests of the stats path are meaningful.
 *
 * The contract is what matters: when the real inference engine drops
 * in, the public ABI is unchanged.
 */

#include "ai_summarise.h"

#include <string.h>
#include <stdio.h>

#ifndef CONFIG_AI_SUMMARISE
/* Build-default path: zero-cost stub. We still implement the symbols
 * so callers compile against this header without #ifdef noise.       */
#define AI_SUMMARISE_DISABLED  1
#endif

#if !defined(AI_SUMMARISE_DISABLED)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#endif

static const char *TAG = "ai_summarise";

/* Internal state. Kept file-static so the stub has the same shape the
 * real implementation will. */
static struct {
    bool                  initialised;
    bool                  load_logged;
    const void           *model_addr;
    size_t                model_size;
    ai_summarise_stats_t  stats;
#if !defined(AI_SUMMARISE_DISABLED)
    SemaphoreHandle_t     mutex;
#endif
} s_ctx;

static const char *PLACEHOLDER_MSG = "summary unavailable";

/* ---- helpers ----------------------------------------------------------- */

static void clamp_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) return;
    if (src == NULL) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#if !defined(AI_SUMMARISE_DISABLED)
static void lock(void)   { if (s_ctx.mutex) xSemaphoreTake(s_ctx.mutex, portMAX_DELAY); }
static void unlock(void) { if (s_ctx.mutex) xSemaphoreGive(s_ctx.mutex); }
#else
static void lock(void)   { /* no-op when disabled */ }
static void unlock(void) { /* no-op when disabled */ }
#endif

/* ---- public API -------------------------------------------------------- */

bool ai_summarise_init(const void *model_blob_addr, size_t model_blob_size)
{
#if defined(AI_SUMMARISE_DISABLED)
    /* Compile-time disabled. Stay zero-cost. */
    (void)model_blob_addr;
    (void)model_blob_size;
    s_ctx.initialised = false;
    s_ctx.stats.initialised = false;
    return false;
#else
    if (s_ctx.initialised) {
        return s_ctx.stats.initialised;
    }
    if (s_ctx.mutex == NULL) {
        s_ctx.mutex = xSemaphoreCreateMutex();
    }
    lock();
    s_ctx.model_addr = model_blob_addr;
    s_ctx.model_size = model_blob_size;

    /* TODO(v1.9.0 ship): mmap model_blob into PSRAM, validate header
     * magic + SHA-256 against tools/ai/model.bin.sha256, allocate
     * kv-cache, spin up the ai_summarise FreeRTOS task. See
     * docs/ON_DEVICE_AI.md "Threading model" and "Memory budget".
     *
     * Until then the stub considers a non-empty blob "loaded" so
     * callers can exercise the success path.
     */
    bool ok = (model_blob_addr != NULL && model_blob_size > 0);
    s_ctx.initialised = true;
    s_ctx.stats.initialised = ok;
    s_ctx.stats.model_resident_bytes = ok ? model_blob_size : 0;
    unlock();

    ESP_LOGI(TAG, "ai_summarise_init blob=%p size=%u ok=%d (stub)",
             model_blob_addr, (unsigned)model_blob_size, (int)ok);
    return ok;
#endif
}

bool ai_summarise(const char *transcript, char *out_msg, size_t out_len)
{
    if (out_msg == NULL || out_len == 0) {
        return false;
    }
    out_msg[0] = '\0';
    if (transcript == NULL) {
        clamp_copy(out_msg, out_len, "");
        return false;
    }

#if defined(AI_SUMMARISE_DISABLED)
    /* Build-default: deterministic placeholder so smoke tests of the
     * msg-rendering path don't crash, but the host bridge's msg
     * always wins because the caller falls back on `false`. */
    clamp_copy(out_msg, out_len, PLACEHOLDER_MSG);
    return false;
#else
    if (!s_ctx.initialised || !s_ctx.stats.initialised) {
        clamp_copy(out_msg, out_len, PLACEHOLDER_MSG);
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    lock();

    /* TODO(v1.9.0 ship): tokenise(transcript) → run llama2.c forward
     * pass on the resident model → decode greedy + newline-stop →
     * write to out_msg. See docs/ON_DEVICE_AI.md "Inference engine".
     * Until that lands, log once and return the placeholder. */
    if (!s_ctx.load_logged) {
        s_ctx.load_logged = true;
        ESP_LOGI(TAG, "ai_summarise() called — stub returning placeholder. "
                      "TODO: real inference lands in v1.9.0 ship cycle.");
    }
    clamp_copy(out_msg, out_len, PLACEHOLDER_MSG);

    int64_t t1 = esp_timer_get_time();
    uint32_t elapsed_ms = (uint32_t)((t1 - t0) / 1000);
    s_ctx.stats.calls_total++;
    s_ctx.stats.calls_succeeded++;       /* stub always "succeeds" */
    s_ctx.stats.last_latency_ms = elapsed_ms;
    if (elapsed_ms > s_ctx.stats.max_latency_ms) {
        s_ctx.stats.max_latency_ms = elapsed_ms;
    }
    unlock();
    return true;
#endif
}

void ai_summarise_get_stats(ai_summarise_stats_t *out)
{
    if (out == NULL) return;
    lock();
    *out = s_ctx.stats;
    unlock();
}
