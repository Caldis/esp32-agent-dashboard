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
/* v2.3.0: AWAITING takeover state — what kind of input the agent is
 * blocking on, plus 1-3 short context lines the takeover scene shows
 * under the headline. */
#define AGENT_AWAITING_CONTEXT_LINES  3
#define AGENT_AWAITING_CONTEXT_MAX    48   /* per line */
/* v2.4.0 dash-state contract: marquee summary + numbered option list. */
#define AGENT_AWAITING_SUMMARY_MAX   208   /* one-line marquee */
#define AGENT_AWAITING_OPTIONS_MAX     4
#define AGENT_AWAITING_OPTION_MAX     36   /* per option */
/* v4.8: number of rotating "your turn" greetings for the CONTINUE
 * takeover headline (see agent_awaiting_greeting). */
#define AGENT_AWAITING_GREETING_COUNT  8
/* v4.9 weather: fixed five-day window — days[0]=yesterday, days[1]=today,
 * days[2..4]=next three days. Pushed by the bridge via `dash weather`
 * (see tools/claude_buddy_bridge.py WeatherPoller). */
#define WEATHER_DAYS            5
#define WEATHER_LOC_MAX        32   /* "深圳·福田" — UTF-8, host-supplied */

typedef enum {
    AWAITING_NONE     = 0,   /* not blocked on user */
    AWAITING_CONTINUE,       /* generic end-of-turn (Stop event) */
    AWAITING_APPROVE,        /* PreToolUse needs y/n decision */
    AWAITING_PICK,           /* assistant offered numbered options */
    AWAITING_TYPE,           /* assistant asked open-ended question */
    AWAITING_CLARIFY,        /* assistant flagged ambiguity */
} awaiting_kind_t;

typedef struct {
    char     role[16];               /* "user" / "assistant" / "tool" / ... */
    char     text[AGENT_ENTRY_TEXT_MAX];
    char     tool[AGENT_ENTRY_TOOL_MAX]; /* canonical tool name, may be empty */
    char     ts[AGENT_ENTRY_TIME_MAX];   /* short HH:MM stamp from host */
    uint32_t monotonic_ms;           /* when this entry was added */
} agent_entry_t;

/* v7.3: mirrors Claude Code's own fleet header
 * ("N awaiting input · N working · N completed") — before this the bridge
 * collapsed "finished" and "blocked on you" into WAITING, so every
 * background conversation shouted "your turn". DONE is listable but never
 * demands attention: it does not pull the display and does not count
 * toward the fleet-view switch. */
typedef enum {
    AGENT_STATUS_IDLE = 0,       /* exists, has done nothing yet */
    AGENT_STATUS_RUNNING,        /* working */
    AGENT_STATUS_WAITING,        /* blocked on the user right now */
    AGENT_STATUS_DONE,           /* turn finished, nothing pending */
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

    /* v2.3.0: AWAITING takeover state — set by the snapshot handler when
     * the bridge says this slot is blocking on user input. The scene
     * picks the most-recent-entered awaiting slot as the takeover
     * anchor. AWAITING_NONE means the slot is in ambient mode. */
    awaiting_kind_t awaiting_kind;
    char            awaiting_context[AGENT_AWAITING_CONTEXT_LINES]
                                    [AGENT_AWAITING_CONTEXT_MAX];
    int             awaiting_context_count;
    uint32_t        awaiting_since_unix;        /* host clock when awaiting began */
    uint32_t        awaiting_entered_ms;        /* lv_tick when we received it */
    /* v2.4.0: dash-state — agent-emitted summary + executable options.
     * If awaiting_summary[0] != 0, scene_awaiting prefers it over
     * awaiting_context for the marquee. If awaiting_options_count > 0,
     * the takeover renders a numbered list below the marquee. */
    char            awaiting_summary[AGENT_AWAITING_SUMMARY_MAX];
    char            awaiting_options[AGENT_AWAITING_OPTIONS_MAX]
                                    [AGENT_AWAITING_OPTION_MAX];
    int             awaiting_options_count;
    /* v4.8: which greeting the CONTINUE takeover shows. Rolled once when
     * the slot enters AWAITING_CONTINUE (agent_state_set_awaiting), so
     * every scene reads the same word and it never flickers per frame;
     * re-rolled each time the agent hands the turn back. Index into the
     * table behind agent_awaiting_greeting(). */
    uint8_t         awaiting_greeting_idx;
} agent_slot_t;

/* v4.9: one day of the weather window. Temperatures are whole °C (the
 * panel shows integers; the bridge rounds). code is the WMO weather
 * interpretation code Open-Meteo emits (0 clear … 99 thunderstorm);
 * wday is 0=Sunday..6=Saturday for that date. */
typedef struct {
    int16_t code;
    int8_t  t_lo;
    int8_t  t_hi;
    uint8_t wday;
} weather_day_t;

/* v4.9: weather snapshot pushed by the host bridge (`dash weather`).
 * valid stays false until the first push lands; received_ms lets scenes
 * grey the data out when the feed goes stale (bridge re-pushes on
 * reconnect and every poll interval). */
typedef struct {
    bool          valid;
    char          loc[WEATHER_LOC_MAX];   /* display name, e.g. "深圳·福田" */
    int16_t       cur_temp;               /* current °C, rounded */
    int16_t       cur_code;               /* current WMO code */
    weather_day_t days[WEATHER_DAYS];
    uint32_t      received_ms;            /* lv_tick at receipt */
} weather_state_t;

typedef struct {
    /* Slot array — index is stable across snapshots, so left pane is
     * always slot 0 and right pane slot 1 unless something explicitly
     * reorders. */
    agent_slot_t  slots[AGENT_SLOT_MAX];
    int           slot_count;                   /* how many `in_use` */

    /* Device-local UI state: Key3 focus cycling. -1 = auto (fleet rows
     * when 2+, ambient when ≤1); >=0 pins the dashboard to that slot's
     * single-agent detail view. Never comes from the wire; reset to -1
     * when the focused slot is pruned. */
    int           focused_slot;

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
    bool          prompt_mode_reply;
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
    bool          motion_reduced;
    /* v4.2 clock screensaver: minutes of no activity before the clock
     * scene takes over. 0 disables. Set via `dash config`
     * {"screensaver_min":N}, persisted to NVS. */
    int32_t       screensaver_min;
    /* v4.7 offline fallback: minutes after the snapshot stream (incl.
     * keepalives) goes silent before the device gives up on the host and
     * retreats to the clock scene — much sooner than the idle
     * screensaver. 0 disables. Set via `dash config`
     * {"offline_clock_min":N}, persisted to NVS. */
    int32_t       offline_clock_min;

    /* v4.2: lv_tick of the last "activity" — any key press, any dash
     * prompt/event, or a snapshot that actually changed agent state
     * (keepalives repeating the same state don't count). Drives the
     * screensaver timer in scene_auto_switch_cb. */
    uint32_t      last_activity_ms;

    /* v4.9: latest weather push (see weather_state_t above). */
    weather_state_t weather;

    /* Counters for `dash health` reply. */
    uint32_t      snapshots_received;
    uint32_t      snapshot_dropped_agents;
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

/* v4.2: stamp last_activity_ms = now. Takes the lock itself — call from
 * un-locked contexts (button task, console handlers). */
void agent_state_touch_activity(void);

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

/* v2.3.0: find the slot most recently entered into AWAITING state.
 * Returns NULL if no slot is awaiting. The takeover scene reads from
 * this slot. Lock held. */
agent_slot_t *agent_state_most_recent_awaiting(void);

/* v2.3.0: count slots currently in AWAITING_* (excluding the most
 * recent one). Used by the takeover footer's "+N more waiting". */
int agent_state_other_awaiting_count(const agent_slot_t *anchor);

/* v7.3: how many slots actually want something — running, or awaiting the
 * user. DONE/IDLE slots are listed but don't count. This, not slot_count,
 * decides whether the dashboard splits into fleet rows: with background
 * conversations the slot list fills up with finished turns, and counting
 * those tipped the display into a multi-row "everyone needs you" view for
 * agents Claude Code itself lists as "completed". Lock held. */
int agent_state_active_count(void);

/* 设备级"注意力等级"：2 = 有 agent 在等你、1 = 有 agent 在跑、0 = 空闲。
 * 与状态色是同一件事的两种读法（gold / teal / dim），供不该碰颜色的图层
 * （ui_deco）用【节奏和亮度】表达同一个状态。调用方需持锁。 */
#define AGENT_ATTN_IDLE   0
#define AGENT_ATTN_BUSY   1
#define AGENT_ATTN_ALERT  2
int agent_state_attention(void);

/* v2.3.0: parse a kind string from the wire snapshot (`"approve"`,
 * `"pick"`, etc.) into the enum. Returns AWAITING_NONE on unknown. */
awaiting_kind_t agent_state_parse_awaiting_kind(const char *s);

/* v4.8: the rotating "your turn" greeting for the CONTINUE takeover.
 * `idx` is a slot's awaiting_greeting_idx (out-of-range → first word).
 * Every word is GB2312 (in the device font subset) and ≤4 hanzi so it
 * fits the HERO-88 headline at 1 m. */
const char *agent_awaiting_greeting(uint8_t idx);

/* v2.3.0: clear AWAITING state on a slot. Lock held. */
void agent_state_clear_awaiting(agent_slot_t *slot);

/* v2.3.0: set AWAITING state on a slot, including up to 3 context
 * lines (NULL-terminated input strings). Each is truncated to
 * AGENT_AWAITING_CONTEXT_MAX-1. Lock held. */
void agent_state_set_awaiting(agent_slot_t *slot, awaiting_kind_t kind,
                              const char *const *context_lines,
                              int line_count, uint32_t since_unix);

/* v2.4.0: set the dash-state summary + options on an already-awaiting
 * slot. Pass NULL/0 to clear. Each option is truncated to
 * AGENT_AWAITING_OPTION_MAX-1. Lock held. */
void agent_state_set_awaiting_summary(agent_slot_t *slot,
                                       const char *summary);
void agent_state_set_awaiting_options(agent_slot_t *slot,
                                       const char *const *options,
                                       int option_count);

#ifdef __cplusplus
}
#endif
