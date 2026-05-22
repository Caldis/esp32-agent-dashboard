/*
 * agent_state — implementation. See agent_state.h.
 *
 * The slot allocator favours stability: incoming agents that match an
 * existing (kind, session_id) tuple reuse their slot, so the left/right
 * pane mapping doesn't flip frame-over-frame.
 */

#include "agent_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static agent_state_t     s_state;
static SemaphoreHandle_t s_mutex;

/* Per-snapshot mark bitfield, separate from slot.in_use so the snapshot
 * handler can mark slots that appeared in THIS payload and prune the
 * rest in one pass. */
static bool s_marked[AGENT_SLOT_MAX];

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    if (src == NULL) { dst[0] = '\0'; return; }
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

agent_slot_t *agent_state_find_slot(const char *kind, const char *session_id)
{
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        agent_slot_t *s = &s_state.slots[i];
        if (!s->in_use) continue;
        /* Prefer match on session_id when both sides have one. Otherwise
         * fall back to matching on kind alone (handles v0 flat snapshots
         * where session_id is absent and the implicit kind is the only
         * identity bit). */
        if (session_id && session_id[0] && s->session_id[0]) {
            if (strcmp(s->session_id, session_id) == 0) return s;
            continue;
        }
        if (kind && kind[0] && strcmp(s->kind, kind) == 0) return s;
    }
    return NULL;
}

agent_slot_t *agent_state_acquire_slot(const char *kind, const char *session_id)
{
    agent_slot_t *found = agent_state_find_slot(kind, session_id);
    if (found) {
        /* Update mark */
        int idx = (int)(found - s_state.slots);
        if (idx >= 0 && idx < AGENT_SLOT_MAX) s_marked[idx] = true;
        return found;
    }
    /* Otherwise allocate the first free slot. */
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!s_state.slots[i].in_use) {
            agent_slot_t *s = &s_state.slots[i];
            memset(s, 0, sizeof(*s));
            s->in_use = true;
            copy_bounded(s->kind,       sizeof(s->kind),       kind);
            copy_bounded(s->session_id, sizeof(s->session_id), session_id);
            s_marked[i] = true;
            s_state.slot_count++;
            return s;
        }
    }
    return NULL;
}

int agent_state_prune_unmarked(char freed_kind[][AGENT_KIND_MAX],
                               char freed_sid[][AGENT_SESSION_ID_MAX])
{
    int freed = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        agent_slot_t *s = &s_state.slots[i];
        if (!s->in_use) { s_marked[i] = false; continue; }
        if (s_marked[i]) { s_marked[i] = false; continue; }
        /* Slot was in_use but didn't appear in this snapshot — drop. */
        if (freed_kind) memcpy(freed_kind[freed], s->kind, AGENT_KIND_MAX);
        if (freed_sid)  memcpy(freed_sid[freed],  s->session_id, AGENT_SESSION_ID_MAX);
        memset(s, 0, sizeof(*s));
        s_state.slot_count--;
        freed++;
    }
    return freed;
}

void agent_state_push_entry(agent_slot_t *slot,
                            const char *role, const char *text,
                            const char *tool, const char *ts)
{
    if (!slot) return;
    int keep = slot->entry_count;
    if (keep > AGENT_ENTRY_COUNT - 1) keep = AGENT_ENTRY_COUNT - 1;
    for (int i = keep; i > 0; --i) {
        slot->entries[i] = slot->entries[i - 1];
    }
    agent_entry_t *e = &slot->entries[0];
    memset(e, 0, sizeof(*e));
    copy_bounded(e->role, sizeof(e->role), role ? role : "");
    copy_bounded(e->text, sizeof(e->text), text ? text : "");
    copy_bounded(e->tool, sizeof(e->tool), tool ? tool : "");
    copy_bounded(e->ts,   sizeof(e->ts),   ts   ? ts   : "");
    e->monotonic_ms = lv_tick_get();
    if (slot->entry_count < AGENT_ENTRY_COUNT) slot->entry_count++;
    slot->entry_seq++;
}

void agent_state_push_spark(agent_slot_t *slot, uint32_t sample)
{
    if (!slot) return;
    slot->spark[slot->spark_head] = sample;
    slot->spark_head = (slot->spark_head + 1) % AGENT_SPARK_SAMPLES;
    if (slot->spark_count < AGENT_SPARK_SAMPLES) slot->spark_count++;
}
