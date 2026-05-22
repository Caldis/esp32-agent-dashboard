/*
 * agent_state — shared snapshot of agent / host activity that scenes read.
 *
 * v1: multi-agent. The single `agent_state_t` is now a container with up
 * to AGENT_SLOT_MAX (=4) per-agent slots. v0 flat snapshots are still
 * accepted; they land in slot 0 with kind="claude-code".
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

#define AGENT_SLOT_MAX          4   /* max concurrent agents we render */
#define AGENT_KIND_MAX         16
#define AGENT_SESSION_ID_MAX   32
#define AGENT_CWD_MAX          64
#define AGENT_MSG_MAX         128
#define AGENT_ENTRY_TEXT_MAX   80
#define AGENT_ENTRY_TIME_MAX    8   /* "10:42" */
#define AGENT_ENTRY_TOOL_MAX   16
#define AGENT_ENTRY_COUNT       5   /* rolling transcript window per agent */
#define AGENT_TOOL_MAX         24
#define AGENT_HINT_MAX         96
#define AGENT_PROMPT_ID_MAX    40
#define AGENT_SPARK_SAMPLES    32   /* tokens sparkline window */
#define AGENT_DEVICE_NAME_MAX  32
#define AGENT_OWNER_MAX        32
#define AGENT_DEFAULT_SCENE_MAX 16

typedef struct {
    char     role[16];               /* "user" / "assistant" / "tool" / ... */
    char     text[AGENT_ENTRY_TEXT_MAX];
    char     tool[AGENT_ENTRY_TOOL_MAX]; /* canonical tool name, may be empty */
    char     ts[AGENT_ENTRY_TIME_MAX];   /* short HH:MM stamp from host */
    uint32_t monotonic_ms;           /* when this entry was added */
} agent_entry_t;

typedef enum {
    AGENT_STATUS_IDLE = 0,
    AGENT_STATUS_RUNNING,
    AGENT_STATUS_WAITING,
} agent_status_t;

/* Per-agent slot. Identity is (kind, session_id); the snapshot handler
 * matches incoming agents to existing slots so the user sees stable
 * left/right placement frame-over-frame. */
typedef struct {
    bool           in_use;
    char           kind[AGENT_KIND_MAX];        /* "claude-code" / "codex" / "other" */
    char           session_id[AGENT_SESSION_ID_MAX];
    char           cwd[AGENT_CWD_MAX];
    char           msg[AGENT_MSG_MAX];
    agent_status_t status;

    agent_entry_t  entries[AGENT_ENTRY_COUNT];
    int            entry_count;
    uint32_t       entry_seq;                   /* bumped on insert/replace */

    uint64_t       tokens_cumulative;
    uint64_t       tokens_today;
    uint32_t       spark[AGENT_SPARK_SAMPLES];
    int            spark_count;
    int            spark_head;

    uint32_t       last_active_unix;            /* host clock, if known */
    uint32_t       last_seen_monotonic_ms;      /* lv_tick at last snapshot */
} agent_slot_t;

typedef struct {
    /* Slot array — index is stable across snapshots, so left pane is
     * always slot 0 and right pane slot 1 unless something explicitly
     * reorders. */
    agent_slot_t  slots[AGENT_SLOT_MAX];
    int           slot_count;                   /* how many `in_use` */

    /* Aggregate totals (from snapshot's "totals" object, or computed
     * from slots on v0 flat snapshots). */
    int           total;
    int           running;
    int           waiting;
    uint64_t      tokens_cumulative;
    uint64_t      tokens_today;

    /* Permission prompt — global, not per agent (only one prompt visible
     * at a time on this device). */
    bool          prompt_active;
    char          prompt_id[AGENT_PROMPT_ID_MAX];
    char          prompt_tool[AGENT_TOOL_MAX];
    char          prompt_hint[AGENT_HINT_MAX];
    char          prompt_agent_kind[AGENT_KIND_MAX];
    char          prompt_session_id[AGENT_SESSION_ID_MAX];
    uint32_t      prompt_shown_ms;              /* lv_tick_get at activation */

    /* Connection tracking — last time we received any snapshot.
     * Scenes show a "(stale)" hint if too old. */
    uint32_t      last_snapshot_ms;
    bool          ever_received;

    /* Host-supplied clock + tz. Optional; zero means "not set". */
    uint32_t      host_epoch_unix;
    int32_t       host_tz_offset_seconds;
    uint32_t      host_clock_received_ms;        /* lv_tick at receipt */

    /* Persisted config (mirrors NVS values). */
    char          device_name[AGENT_DEVICE_NAME_MAX];
    char          owner[AGENT_OWNER_MAX];
    char          default_scene[AGENT_DEFAULT_SCENE_MAX];

    /* Counters for `dash health` reply. */
    uint32_t      snapshots_received;
    uint32_t      prompts_received;
    uint32_t      decisions_sent;
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

/* Find a slot by (kind, session_id); returns NULL if no match. Lock held. */
agent_slot_t *agent_state_find_slot(const char *kind, const char *session_id);

/* Acquire or allocate a slot for (kind, session_id). Returns NULL if
 * all slots are in use AND nothing matches. Lock held. */
agent_slot_t *agent_state_acquire_slot(const char *kind, const char *session_id);

/* Drop slots that weren't touched this snapshot. Lock held. Called by
 * the snapshot handler after merging incoming agents. Returns number of
 * slots freed (for EVT emission). The `freed_kind`/`freed_sid` arrays
 * receive the identifiers (up to AGENT_SLOT_MAX entries) of dropped
 * slots so the caller can emit `agent_removed` EVTs after dropping the
 * lock. */
int agent_state_prune_unmarked(char freed_kind[][AGENT_KIND_MAX],
                               char freed_sid[][AGENT_SESSION_ID_MAX]);

/* Insert one entry at the head of a slot's ring. Lock held. */
void agent_state_push_entry(agent_slot_t *slot,
                            const char *role, const char *text,
                            const char *tool, const char *ts);

/* Push one tokens sparkline sample. Lock held. */
void agent_state_push_spark(agent_slot_t *slot, uint32_t sample);

#ifdef __cplusplus
}
#endif
