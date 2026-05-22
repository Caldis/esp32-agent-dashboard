/*
 * agent_state — shared snapshot of agent / host activity that scenes read.
 *
 * The host pushes updates via `dash *` console commands; handlers in
 * harness/agent_commands.c parse the JSON, lock the state, mutate fields,
 * unlock, and (where appropriate) request a scene transition.
 *
 * Scenes read fields under the same mutex during their per-tick handler.
 * Keep the critical sections short — copy what you need into locals and
 * release before LVGL widget mutations.
 *
 * The struct is intentionally fixed-size with bounded strings so we never
 * malloc inside a console handler. Snapshot/event JSON payloads exceeding
 * the limits are truncated.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_MSG_MAX        128
#define AGENT_ENTRY_TEXT_MAX  96
#define AGENT_ENTRY_COUNT      5    /* rolling transcript window */
#define AGENT_TOOL_MAX        24
#define AGENT_HINT_MAX        96
#define AGENT_PROMPT_ID_MAX   40
#define AGENT_SPARK_SAMPLES   32    /* tokens sparkline window */

typedef struct {
    char     role[16];               /* "user" / "assistant" / "tool" / ... */
    char     text[AGENT_ENTRY_TEXT_MAX];
    uint32_t monotonic_ms;           /* when this entry was added */
} agent_entry_t;

typedef struct {
    /* Session counters */
    int      total;
    int      running;
    int      waiting;
    char     msg[AGENT_MSG_MAX];

    /* Transcript ring — newest first at index 0. */
    agent_entry_t entries[AGENT_ENTRY_COUNT];
    int           entry_count;       /* 0..AGENT_ENTRY_COUNT */
    uint32_t      entry_seq;         /* monotonically increasing on each insert */

    /* Tokens */
    uint64_t tokens_cumulative;
    uint64_t tokens_today;
    uint32_t spark[AGENT_SPARK_SAMPLES];
    int      spark_count;
    int      spark_head;             /* index of next write slot */

    /* Permission prompt — when prompt_active, the prompt scene is the
     * intended foreground. Cleared on decision or timeout. */
    bool     prompt_active;
    char     prompt_id[AGENT_PROMPT_ID_MAX];
    char     prompt_tool[AGENT_TOOL_MAX];
    char     prompt_hint[AGENT_HINT_MAX];
    uint32_t prompt_shown_ms;        /* lv_tick_get() at activation */
} agent_state_t;

/* Initialise the global state + its mutex. Call once before any scene
 * or command handler runs. */
void agent_state_init(void);

/* Lock / unlock. Pair them strictly. Scenes hold the lock during tick
 * just long enough to copy the bits they need; never under bsp_display
 * mutex while sleeping. */
void agent_state_lock(void);
void agent_state_unlock(void);

/* Direct accessor — only valid while you hold the lock. */
agent_state_t *agent_state_get(void);

/* Insert one entry at the head of the ring; rotates older items out.
 * Must be called with the lock held. role/text are copied. */
void agent_state_push_entry(const char *role, const char *text);

/* Push one tokens sparkline sample (per the host's chosen cadence).
 * Must be called with the lock held. */
void agent_state_push_spark(uint32_t sample);

#ifdef __cplusplus
}
#endif
