/*
 * agent_state implementation. See agent_state.h.
 */

#include "agent_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static agent_state_t   s_state;
static SemaphoreHandle_t s_mutex;

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void agent_state_init(void)
{
    if (s_mutex != NULL) return;
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
}

void agent_state_lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
void agent_state_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

agent_state_t *agent_state_get(void) { return &s_state; }

void agent_state_push_entry(const char *role, const char *text)
{
    /* Shift one position; drop the oldest if at capacity. */
    int keep = s_state.entry_count;
    if (keep > AGENT_ENTRY_COUNT - 1) keep = AGENT_ENTRY_COUNT - 1;
    for (int i = keep; i > 0; --i) {
        s_state.entries[i] = s_state.entries[i - 1];
    }
    agent_entry_t *slot = &s_state.entries[0];
    copy_bounded(slot->role, sizeof(slot->role), role ? role : "");
    copy_bounded(slot->text, sizeof(slot->text), text ? text : "");
    slot->monotonic_ms = lv_tick_get();
    if (s_state.entry_count < AGENT_ENTRY_COUNT) s_state.entry_count++;
    s_state.entry_seq++;
}

void agent_state_push_spark(uint32_t sample)
{
    s_state.spark[s_state.spark_head] = sample;
    s_state.spark_head = (s_state.spark_head + 1) % AGENT_SPARK_SAMPLES;
    if (s_state.spark_count < AGENT_SPARK_SAMPLES) s_state.spark_count++;
}
