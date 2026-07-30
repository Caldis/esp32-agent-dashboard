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

#include "cjk_font.h"   /* cjk_utf8_lcpy — UTF-8-safe bounded copy */

static agent_state_t     s_state;
static SemaphoreHandle_t s_mutex;

/* Per-snapshot mark bitfield, separate from slot.in_use so the snapshot
 * handler can mark slots that appeared in THIS payload and prune the
 * rest in one pass. */
static bool s_marked[AGENT_SLOT_MAX];

/* UTF-8-safe bounded copy: a byte-level truncation (the old strnlen +
 * memcpy) slices multi-byte CJK characters in half, and those torn
 * bytes flow straight into LVGL labels. cjk_utf8_lcpy only ever cuts
 * on character boundaries. */
static void copy_bounded(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    cjk_utf8_lcpy(dst, src, (unsigned)cap);
}

void agent_state_init(void)
{
    if (s_mutex != NULL) return;
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    s_state.focused_slot = -1;
    s_state.screensaver_min = 10;   /* default; NVS may override */
    s_state.offline_clock_min = 5;  /* host lost → clock after 5 min */
    s_state.last_activity_ms = lv_tick_get();
}

void agent_state_touch_activity(void)
{
    agent_state_lock();
    s_state.last_activity_ms = lv_tick_get();
    agent_state_unlock();
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
        if (s_state.focused_slot == i) s_state.focused_slot = -1;
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

/* ── v2.3.0: AWAITING helpers ────────────────────────────────────── */

awaiting_kind_t agent_state_parse_awaiting_kind(const char *s)
{
    if (!s || !*s) return AWAITING_NONE;
    if (strcmp(s, "continue") == 0) return AWAITING_CONTINUE;
    if (strcmp(s, "approve")  == 0) return AWAITING_APPROVE;
    if (strcmp(s, "pick")     == 0) return AWAITING_PICK;
    if (strcmp(s, "type")     == 0) return AWAITING_TYPE;
    if (strcmp(s, "clarify")  == 0) return AWAITING_CLARIFY;
    return AWAITING_NONE;
}

void agent_state_clear_awaiting(agent_slot_t *slot)
{
    if (!slot) return;
    slot->awaiting_kind = AWAITING_NONE;
    slot->awaiting_context_count = 0;
    slot->awaiting_since_unix = 0;
    slot->awaiting_entered_ms = 0;
    for (int i = 0; i < AGENT_AWAITING_CONTEXT_LINES; ++i) {
        slot->awaiting_context[i][0] = '\0';
    }
    /* v2.4.0: also clear summary + options. */
    slot->awaiting_summary[0] = '\0';
    slot->awaiting_options_count = 0;
    for (int i = 0; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
        slot->awaiting_options[i][0] = '\0';
    }
}

void agent_state_set_awaiting_summary(agent_slot_t *slot, const char *summary)
{
    if (!slot) return;
    copy_bounded(slot->awaiting_summary, AGENT_AWAITING_SUMMARY_MAX,
                 summary ? summary : "");
}

void agent_state_set_awaiting_options(agent_slot_t *slot,
                                       const char *const *options,
                                       int option_count)
{
    if (!slot) return;
    int n = option_count;
    if (n < 0) n = 0;
    if (n > AGENT_AWAITING_OPTIONS_MAX) n = AGENT_AWAITING_OPTIONS_MAX;
    for (int i = 0; i < n; ++i) {
        copy_bounded(slot->awaiting_options[i],
                     AGENT_AWAITING_OPTION_MAX,
                     (options && options[i]) ? options[i] : "");
    }
    for (int i = n; i < AGENT_AWAITING_OPTIONS_MAX; ++i) {
        slot->awaiting_options[i][0] = '\0';
    }
    slot->awaiting_options_count = n;
}

/* v4.8: rotating "your turn" greetings for the CONTINUE takeover. All
 * GB2312 (device font subset). v5.7: uniformly THREE hanzi — the old
 * 3/4-char mix made the HERO headline's width jump between takeovers,
 * which the user read as "the main text keeps changing size". */
static const char *const k_awaiting_greetings[AGENT_AWAITING_GREETING_COUNT] = {
    "该你了", "到你啦", "你来吧", "轮到你",
    "接着来", "交给你", "请接手", "该你咯",
};

const char *agent_awaiting_greeting(uint8_t idx)
{
    if (idx >= AGENT_AWAITING_GREETING_COUNT) idx = 0;
    return k_awaiting_greetings[idx];
}

/* Pick a greeting index different from `prev` so the word visibly changes
 * every time the turn comes back. lv_tick seeds it (this file also builds
 * for the wasm data-layer, whose shim provides lv_tick_get but not
 * esp_random); a running counter guarantees rotation even if two picks
 * land on the same tick. */
static uint8_t pick_awaiting_greeting(uint8_t prev)
{
    static uint32_t rot;
    uint8_t idx = (uint8_t)((lv_tick_get() + rot++ * 7u)
                            % AGENT_AWAITING_GREETING_COUNT);
    if (idx == prev) {
        idx = (uint8_t)((idx + 1u) % AGENT_AWAITING_GREETING_COUNT);
    }
    return idx;
}

void agent_state_set_awaiting(agent_slot_t *slot, awaiting_kind_t kind,
                              const char *const *context_lines,
                              int line_count, uint32_t since_unix)
{
    if (!slot) return;
    /* Only bump entered_ms on a fresh awaiting (slot was clear before,
     * OR the kind changed). Keep the existing entered_ms across snapshots
     * that just re-affirm the same kind — so "waiting Xs" counts from
     * the actual start of the wait, not from each snapshot. */
    bool transitioning = (slot->awaiting_kind != kind);
    slot->awaiting_kind = kind;
    if (transitioning) {
        slot->awaiting_entered_ms = lv_tick_get();
        /* v4.8: a fresh entry into "your turn" rolls a new greeting so the
         * CONTINUE headline rotates each time the agent hands back. Other
         * kinds keep their fixed instructional headline. */
        if (kind == AWAITING_CONTINUE) {
            slot->awaiting_greeting_idx =
                pick_awaiting_greeting(slot->awaiting_greeting_idx);
        }
    }
    slot->awaiting_since_unix = since_unix ? since_unix : slot->awaiting_since_unix;
    if (slot->awaiting_since_unix == 0) {
        slot->awaiting_since_unix = since_unix;
    }
    int n = line_count;
    if (n < 0) n = 0;
    if (n > AGENT_AWAITING_CONTEXT_LINES) n = AGENT_AWAITING_CONTEXT_LINES;
    for (int i = 0; i < n; ++i) {
        copy_bounded(slot->awaiting_context[i],
                     AGENT_AWAITING_CONTEXT_MAX,
                     context_lines && context_lines[i] ? context_lines[i] : "");
    }
    for (int i = n; i < AGENT_AWAITING_CONTEXT_LINES; ++i) {
        slot->awaiting_context[i][0] = '\0';
    }
    slot->awaiting_context_count = n;
}

agent_slot_t *agent_state_most_recent_awaiting(void)
{
    agent_slot_t *best = NULL;
    uint32_t best_ms = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        agent_slot_t *s = &s_state.slots[i];
        if (!s->in_use || s->awaiting_kind == AWAITING_NONE) continue;
        if (best == NULL || s->awaiting_entered_ms > best_ms) {
            best = s;
            best_ms = s->awaiting_entered_ms;
        }
    }
    return best;
}

int agent_state_other_awaiting_count(const agent_slot_t *anchor)
{
    int n = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        const agent_slot_t *s = &s_state.slots[i];
        if (!s->in_use || s->awaiting_kind == AWAITING_NONE) continue;
        if (s == anchor) continue;
        n++;
    }
    return n;
}

int agent_state_active_count(void)
{
    int n = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        const agent_slot_t *s = &s_state.slots[i];
        if (!s->in_use) continue;
        if (s->status == AGENT_STATUS_RUNNING || s->awaiting_kind != AWAITING_NONE)
            n++;
    }
    return n;
}
