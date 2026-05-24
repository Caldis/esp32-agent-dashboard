/*
 * agent_commands — single `dash` console command with subcommand dispatch.
 *
 *   dash snapshot <json>   — full state push (v1: {agents:[...], totals:{...}}
 *                            also accepts v0 flat shape)
 *   dash prompt   <json>   — set prompt_active + switch scene
 *   dash event    <json>   — one-shot transcript line (per agent)
 *   dash tokens   <json>   — per-agent token counters + sparkline sample
 *   dash idle              — switch back to scene_idle
 *   dash config   <json>   — set device_name / owner / theme / default_scene
 *                            (persisted to NVS namespace "dashcfg")
 *   dash time     <json>   — set epoch_unix + tz_offset_seconds
 *   dash health            — replies HEALTH payload block with device internals
 *   dash push     <json>   — top-slide-down banner (v2.7.0): tool + hint, 3s
 *
 * The host bridge ships the JSON payload as ONE argv-token, leveraging
 * the console tokenizer's double-quote support (G-7 fix). Because the
 * harness registry keys on argv[0] only, we register a single `dash`
 * entry and dispatch on argv[1] inside this file. argv[2] is the JSON.
 *
 * JSON parsing uses our local tiny_json (cJSON was removed from IDF v6
 * and we don't want a managed-component dep just for this).
 */

#include "agent_commands.h"
#include "../agent_state.h"
#include "../theme.h"
#include "../tiny_json.h"
#include "../push_banner.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "harness/console_protocol.h"
#include "harness/scene_framework.h"

static const char *TAG = "agent_cmd";

#define NVS_NAMESPACE "dashcfg"

/* Switch to a scene by id, holding the LVGL lock around the call. */
static bool switch_scene(const char *id)
{
    int idx = scene_fw_find_by_id(id);
    if (idx < 0) return false;
    bsp_display_lock(-1);
    scene_fw_show(idx);
    bsp_display_unlock();
    return true;
}

/* argv[2] is the JSON. Returns NULL after replying ERR. */
static const char *json_arg(const console_args_t *a, const char **out_end)
{
    if (a->argc < 3 || a->argv[2] == NULL || a->argv[2][0] == '\0') {
        console_reply_err("expected JSON payload");
        return NULL;
    }
    const char *s = a->argv[2];
    if (out_end) *out_end = s + strlen(s);
    return s;
}

/* Map status string → enum. */
static agent_status_t parse_status(const char *s)
{
    if (!s) return AGENT_STATUS_IDLE;
    if (strcmp(s, "running") == 0) return AGENT_STATUS_RUNNING;
    if (strcmp(s, "waiting") == 0) return AGENT_STATUS_WAITING;
    return AGENT_STATUS_IDLE;
}

/* ── Merge one agent JSON object into the slot table. Lock held. ───── */

static void merge_agent_object(tj_span_t obj, const char *default_kind)
{
    char kind[AGENT_KIND_MAX]       = {0};
    char sid[AGENT_SESSION_ID_MAX]  = {0};
    char status[16]                 = {0};
    char cwd[AGENT_CWD_MAX]         = {0};
    char msg[AGENT_MSG_MAX]         = {0};
    double tokens = 0, tokens_today = 0, last_active = 0;
    bool has_tokens, has_tokens_today, has_last_active;

    tj_object_get_string(obj.begin, obj.end, "kind",        kind, sizeof(kind));
    if (kind[0] == '\0' && default_kind) strncpy(kind, default_kind, sizeof(kind) - 1);
    if (kind[0] == '\0') strcpy(kind, "claude-code");

    tj_object_get_string(obj.begin, obj.end, "session_id",  sid,  sizeof(sid));
    tj_object_get_string(obj.begin, obj.end, "status",      status, sizeof(status));
    tj_object_get_string(obj.begin, obj.end, "cwd",         cwd,  sizeof(cwd));
    tj_object_get_string(obj.begin, obj.end, "msg",         msg,  sizeof(msg));
    has_tokens       = tj_object_get_double(obj.begin, obj.end, "tokens",       &tokens);
    has_tokens_today = tj_object_get_double(obj.begin, obj.end, "tokens_today", &tokens_today);
    has_last_active  = tj_object_get_double(obj.begin, obj.end, "last_active_unix", &last_active);

    agent_slot_t *slot = agent_state_acquire_slot(kind, sid);
    if (!slot) return;  /* table full; silently drop overflow */

    /* Refresh slot identity (kind/sid may have been set on first use). */
    if (kind[0]) { strncpy(slot->kind, kind, sizeof(slot->kind) - 1); slot->kind[sizeof(slot->kind)-1]='\0'; }
    if (sid[0])  { strncpy(slot->session_id, sid, sizeof(slot->session_id) - 1); slot->session_id[sizeof(slot->session_id)-1]='\0'; }
    if (cwd[0]) {
        size_t n = strnlen(cwd, sizeof(slot->cwd) - 1);
        memcpy(slot->cwd, cwd, n); slot->cwd[n] = '\0';
    }
    if (msg[0]) {
        size_t n = strnlen(msg, sizeof(slot->msg) - 1);
        memcpy(slot->msg, msg, n); slot->msg[n] = '\0';
    }
    if (status[0]) slot->status = parse_status(status);
    if (has_tokens)       slot->tokens_cumulative = (uint64_t)tokens;
    if (has_tokens_today) slot->tokens_today      = (uint64_t)tokens_today;
    if (has_last_active)  slot->last_active_unix  = (uint32_t)last_active;
    slot->last_seen_monotonic_ms = lv_tick_get();

    /* Entries — replace ring. */
    tj_span_t entries_v;
    bool has_entries = tj_object_find(obj.begin, obj.end, "entries", &entries_v) &&
                       tj_value_is_array(entries_v);
    if (has_entries) {
        memset(slot->entries, 0, sizeof(slot->entries));
        slot->entry_count = 0;
        int n_items = 0;
        const char *cursor = NULL;
        tj_span_t it;
        while (tj_array_next(entries_v, cursor, &it)) {
            n_items++; cursor = it.end;
        }
        int skip = (n_items > AGENT_ENTRY_COUNT) ? (n_items - AGENT_ENTRY_COUNT) : 0;
        cursor = NULL;
        int i = 0;
        while (tj_array_next(entries_v, cursor, &it)) {
            if (i++ < skip) { cursor = it.end; continue; }
            char role[16] = {0}, text[AGENT_ENTRY_TEXT_MAX] = {0};
            char tool[AGENT_ENTRY_TOOL_MAX] = {0}, ts[AGENT_ENTRY_TIME_MAX] = {0};
            if (tj_value_is_object(it)) {
                tj_object_get_string(it.begin, it.end, "role",    role, sizeof(role));
                tj_object_get_string(it.begin, it.end, "tool",    tool, sizeof(tool));
                tj_object_get_string(it.begin, it.end, "t",       ts,   sizeof(ts));
                /* prefer summary (v1) then text then content */
                if (!tj_object_get_string(it.begin, it.end, "summary",
                                          text, sizeof(text))) {
                    if (!tj_object_get_string(it.begin, it.end, "text",
                                              text, sizeof(text))) {
                        tj_object_get_string(it.begin, it.end, "content",
                                             text, sizeof(text));
                    }
                }
                if (role[0] == '\0' && tool[0]) strcpy(role, "tool");
            } else if (it.begin < it.end && *it.begin == '"') {
                tj_value_string(it, text, sizeof(text));
                strcpy(role, "msg");
            }
            agent_state_push_entry(slot, role, text, tool, ts);
            cursor = it.end;
        }
    }

    /* v2.3.0: AWAITING fields. Bridge only emits awaiting_kind when a
     * session is actually awaiting; if absent we clear the slot's
     * AWAITING state so the auto_switch_cb returns to the ambient
     * scene. */
    char awaiting_kind_str[16] = {0};
    if (tj_object_get_string(obj.begin, obj.end, "awaiting_kind",
                             awaiting_kind_str, sizeof(awaiting_kind_str))
        && awaiting_kind_str[0] != '\0')
    {
        awaiting_kind_t k = agent_state_parse_awaiting_kind(awaiting_kind_str);
        double since = 0;
        tj_object_get_double(obj.begin, obj.end, "awaiting_since", &since);

        /* Pull up to 3 context lines from the awaiting_context array. */
        tj_span_t ctx_v;
        const char *ctx_lines[AGENT_AWAITING_CONTEXT_LINES] = {0};
        char ctx_bufs[AGENT_AWAITING_CONTEXT_LINES][AGENT_AWAITING_CONTEXT_MAX] = {{0}};
        int ctx_count = 0;
        if (tj_object_find(obj.begin, obj.end, "awaiting_context", &ctx_v) &&
            tj_value_is_array(ctx_v))
        {
            const char *cur2 = NULL;
            tj_span_t line;
            while (ctx_count < AGENT_AWAITING_CONTEXT_LINES
                   && tj_array_next(ctx_v, cur2, &line))
            {
                cur2 = line.end;
                if (line.begin < line.end && *line.begin == '"') {
                    tj_value_string(line, ctx_bufs[ctx_count],
                                    AGENT_AWAITING_CONTEXT_MAX);
                    ctx_lines[ctx_count] = ctx_bufs[ctx_count];
                    ctx_count++;
                }
            }
        }
        agent_state_set_awaiting(slot, k, ctx_lines, ctx_count, (uint32_t)since);

        /* v2.4.0: dash-state — summary + options from the snapshot.
         * Both are optional; absence clears the corresponding field. */
        char summary_buf[AGENT_AWAITING_SUMMARY_MAX] = {0};
        if (tj_object_get_string(obj.begin, obj.end, "awaiting_summary",
                                 summary_buf, sizeof(summary_buf))) {
            agent_state_set_awaiting_summary(slot, summary_buf);
        } else {
            agent_state_set_awaiting_summary(slot, "");
        }

        tj_span_t opts_v;
        if (tj_object_find(obj.begin, obj.end, "awaiting_options", &opts_v) &&
            tj_value_is_array(opts_v))
        {
            const char *opts_cursor = NULL;
            tj_span_t opt;
            char opt_bufs[AGENT_AWAITING_OPTIONS_MAX][AGENT_AWAITING_OPTION_MAX] = {{0}};
            const char *opts[AGENT_AWAITING_OPTIONS_MAX] = {0};
            int n_opts = 0;
            while (n_opts < AGENT_AWAITING_OPTIONS_MAX
                   && tj_array_next(opts_v, opts_cursor, &opt))
            {
                opts_cursor = opt.end;
                if (opt.begin < opt.end && *opt.begin == '"') {
                    tj_value_string(opt, opt_bufs[n_opts],
                                    AGENT_AWAITING_OPTION_MAX);
                    opts[n_opts] = opt_bufs[n_opts];
                    n_opts++;
                }
            }
            agent_state_set_awaiting_options(slot, opts, n_opts);
        } else {
            agent_state_set_awaiting_options(slot, NULL, 0);
        }
    } else {
        agent_state_clear_awaiting(slot);
    }
}

/* ── dash snapshot ───────────────────────────────────────────────── */

static int cmd_snapshot(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    /* Detect v1 (has "agents") vs v0 (flat). */
    tj_span_t agents_v;
    bool has_agents = tj_object_find(json, end, "agents", &agents_v) &&
                      tj_value_is_array(agents_v);

    /* Prompt: object means set; null means clear; missing means leave. */
    tj_span_t prompt_v;
    bool prompt_found = tj_object_find(json, end, "prompt", &prompt_v);
    bool prompt_set = false, prompt_clear = false;
    char prompt_id_buf[AGENT_PROMPT_ID_MAX]   = {0};
    char prompt_tool_buf[AGENT_TOOL_MAX]      = {0};
    char prompt_hint_buf[AGENT_HINT_MAX]      = {0};
    char prompt_kind_buf[AGENT_KIND_MAX]      = {0};
    char prompt_sid_buf[AGENT_SESSION_ID_MAX] = {0};
    if (prompt_found) {
        if (tj_value_is_null(prompt_v)) {
            prompt_clear = true;
        } else if (tj_value_is_object(prompt_v)) {
            tj_object_get_string(prompt_v.begin, prompt_v.end, "id",
                                 prompt_id_buf, sizeof(prompt_id_buf));
            tj_object_get_string(prompt_v.begin, prompt_v.end, "tool",
                                 prompt_tool_buf, sizeof(prompt_tool_buf));
            tj_object_get_string(prompt_v.begin, prompt_v.end, "hint",
                                 prompt_hint_buf, sizeof(prompt_hint_buf));
            tj_object_get_string(prompt_v.begin, prompt_v.end, "agent_kind",
                                 prompt_kind_buf, sizeof(prompt_kind_buf));
            tj_object_get_string(prompt_v.begin, prompt_v.end, "session_id",
                                 prompt_sid_buf, sizeof(prompt_sid_buf));
            prompt_set = (prompt_id_buf[0] != '\0');
        }
    }

    /* Buffers for "freed" slots, captured under lock, emitted after. */
    char freed_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    char freed_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX];
    int freed_count = 0;

    char before_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    char before_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX];
    bool before_inuse[AGENT_SLOT_MAX];

    agent_state_lock();
    agent_state_t *s = agent_state_get();

    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        before_inuse[i] = s->slots[i].in_use;
        memcpy(before_kind[i], s->slots[i].kind,       AGENT_KIND_MAX);
        memcpy(before_sid[i],  s->slots[i].session_id, AGENT_SESSION_ID_MAX);
    }

    if (has_agents) {
        const char *cursor = NULL;
        tj_span_t it;
        while (tj_array_next(agents_v, cursor, &it)) {
            cursor = it.end;
            if (!tj_value_is_object(it)) continue;
            merge_agent_object(it, NULL);
        }
        freed_count = agent_state_prune_unmarked(freed_kind, freed_sid);

        /* Totals — explicit object or compute from slots. */
        tj_span_t totals_v;
        if (tj_object_find(json, end, "totals", &totals_v) &&
            tj_value_is_object(totals_v)) {
            double td = 0, rd = 0, wd = 0, tk = 0, tkt = 0;
            if (tj_object_get_double(totals_v.begin, totals_v.end, "total", &td))   s->total   = (int)td;
            if (tj_object_get_double(totals_v.begin, totals_v.end, "running", &rd)) s->running = (int)rd;
            if (tj_object_get_double(totals_v.begin, totals_v.end, "waiting", &wd)) s->waiting = (int)wd;
            if (tj_object_get_double(totals_v.begin, totals_v.end, "tokens", &tk))         s->tokens_cumulative = (uint64_t)tk;
            if (tj_object_get_double(totals_v.begin, totals_v.end, "tokens_today", &tkt))  s->tokens_today      = (uint64_t)tkt;
        } else {
            int tot = 0, run = 0, wait = 0;
            uint64_t tk = 0, tkt = 0;
            for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
                if (!s->slots[i].in_use) continue;
                tot++;
                if (s->slots[i].status == AGENT_STATUS_RUNNING) run++;
                else if (s->slots[i].status == AGENT_STATUS_WAITING) wait++;
                tk  += s->slots[i].tokens_cumulative;
                tkt += s->slots[i].tokens_today;
            }
            s->total = tot; s->running = run; s->waiting = wait;
            s->tokens_cumulative = tk; s->tokens_today = tkt;
        }
    } else {
        /* v0 flat snapshot — treat as one implicit agent. */
        tj_span_t flat = { json, end };
        merge_agent_object(flat, "claude-code");
        freed_count = agent_state_prune_unmarked(freed_kind, freed_sid);

        double td, rd, wd, tk, tkt;
        if (tj_object_get_double(json, end, "total",        &td))  s->total   = (int)td;
        if (tj_object_get_double(json, end, "running",      &rd))  s->running = (int)rd;
        if (tj_object_get_double(json, end, "waiting",      &wd))  s->waiting = (int)wd;
        if (tj_object_get_double(json, end, "tokens",       &tk))  s->tokens_cumulative = (uint64_t)tk;
        if (tj_object_get_double(json, end, "tokens_today", &tkt)) s->tokens_today      = (uint64_t)tkt;
    }

    if (prompt_set) {
        memcpy(s->prompt_id,         prompt_id_buf,   sizeof(s->prompt_id));
        memcpy(s->prompt_tool,       prompt_tool_buf, sizeof(s->prompt_tool));
        memcpy(s->prompt_hint,       prompt_hint_buf, sizeof(s->prompt_hint));
        memcpy(s->prompt_agent_kind, prompt_kind_buf, sizeof(s->prompt_agent_kind));
        memcpy(s->prompt_session_id, prompt_sid_buf,  sizeof(s->prompt_session_id));
        s->prompt_active = true;
        s->prompt_shown_ms = lv_tick_get();
        s->prompts_received++;
    } else if (prompt_clear) {
        s->prompt_active = false;
        s->prompt_id[0] = s->prompt_tool[0] = s->prompt_hint[0] = '\0';
        s->prompt_agent_kind[0] = s->prompt_session_id[0] = '\0';
    }

    s->last_snapshot_ms  = lv_tick_get();
    s->ever_received     = true;
    s->snapshots_received++;
    int total_now = s->total ? s->total : s->slot_count;

    char after_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    char after_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX];
    bool after_inuse[AGENT_SLOT_MAX];
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        after_inuse[i] = s->slots[i].in_use;
        memcpy(after_kind[i], s->slots[i].kind,       AGENT_KIND_MAX);
        memcpy(after_sid[i],  s->slots[i].session_id, AGENT_SESSION_ID_MAX);
    }
    agent_state_unlock();

    for (int i = 0; i < freed_count; ++i) {
        console_send_evt("agent_removed kind=%s session_id=%s",
                         freed_kind[i], freed_sid[i]);
    }
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        bool was = before_inuse[i];
        bool now = after_inuse[i];
        bool ident_changed = (memcmp(before_kind[i], after_kind[i], AGENT_KIND_MAX) != 0 ||
                              memcmp(before_sid[i],  after_sid[i],  AGENT_SESSION_ID_MAX) != 0);
        if (now && (!was || ident_changed)) {
            console_send_evt("agent_added kind=%s session_id=%s",
                             after_kind[i], after_sid[i]);
        }
    }

    /* Auto-pick scene based on new state. */
    const scene_t *cur = scene_fw_current();
    const char *cur_id = cur ? cur->id : "";
    if (prompt_set) {
        switch_scene("prompt");
    } else if (prompt_clear && strcmp(cur_id, "prompt") == 0) {
        switch_scene("dashboard");
    } else if (total_now > 0 && strcmp(cur_id, "idle") == 0) {
        switch_scene("dashboard");
    } else if (total_now == 0 && (strcmp(cur_id, "sessions") == 0 ||
                                  strcmp(cur_id, "dashboard") == 0)) {
        switch_scene("idle");
    }

    console_reply_ok("{\"applied\":true,\"agents\":%d}", total_now);
    return 0;
}

/* ── dash prompt ─────────────────────────────────────────────────── */

static int cmd_prompt(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    char id[AGENT_PROMPT_ID_MAX]       = {0};
    char tool[AGENT_TOOL_MAX]          = {0};
    char hint[AGENT_HINT_MAX]          = {0};
    char kind[AGENT_KIND_MAX]          = {0};
    char sid[AGENT_SESSION_ID_MAX]     = {0};
    tj_object_get_string(json, end, "id",         id,   sizeof(id));
    tj_object_get_string(json, end, "tool",       tool, sizeof(tool));
    tj_object_get_string(json, end, "hint",       hint, sizeof(hint));
    tj_object_get_string(json, end, "agent_kind", kind, sizeof(kind));
    tj_object_get_string(json, end, "session_id", sid,  sizeof(sid));

    if (id[0] == '\0') { console_reply_err("prompt id required"); return 0; }

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    memcpy(s->prompt_id,         id,   sizeof(s->prompt_id));
    memcpy(s->prompt_tool,       tool, sizeof(s->prompt_tool));
    memcpy(s->prompt_hint,       hint, sizeof(s->prompt_hint));
    memcpy(s->prompt_agent_kind, kind, sizeof(s->prompt_agent_kind));
    memcpy(s->prompt_session_id, sid,  sizeof(s->prompt_session_id));
    s->prompt_active = true;
    s->prompt_shown_ms = lv_tick_get();
    s->prompts_received++;
    agent_state_unlock();

    switch_scene("prompt");
    console_reply_ok("{\"prompt\":\"%s\"}", id);
    return 0;
}

/* ── dash event ──────────────────────────────────────────────────── */

static int cmd_event(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    char role[16] = {0};
    char text[AGENT_ENTRY_TEXT_MAX] = {0};
    char kind[AGENT_KIND_MAX] = {0};
    char sid[AGENT_SESSION_ID_MAX] = {0};
    char tool[AGENT_ENTRY_TOOL_MAX] = {0};
    tj_object_get_string(json, end, "role",       role, sizeof(role));
    tj_object_get_string(json, end, "agent_kind", kind, sizeof(kind));
    tj_object_get_string(json, end, "session_id", sid,  sizeof(sid));
    tj_object_get_string(json, end, "tool",       tool, sizeof(tool));

    tj_span_t content_v;
    if (tj_object_find(json, end, "content", &content_v)) {
        if (tj_value_is_array(content_v)) {
            const char *cursor = NULL;
            tj_span_t it;
            while (tj_array_next(content_v, cursor, &it)) {
                cursor = it.end;
                if (!tj_value_is_object(it)) continue;
                if (tj_object_get_string(it.begin, it.end, "text",
                                         text, sizeof(text))) {
                    break;
                }
            }
        } else if (content_v.begin < content_v.end && *content_v.begin == '"') {
            tj_value_string(content_v, text, sizeof(text));
        }
    }
    if (text[0] == '\0') {
        tj_object_get_string(json, end, "text", text, sizeof(text));
    }

    agent_state_lock();
    agent_slot_t *slot = agent_state_acquire_slot(kind[0] ? kind : "claude-code", sid);
    agent_state_push_entry(slot, role, text, tool, NULL);
    agent_state_unlock();

    console_reply_ok("{\"event\":\"queued\"}");
    return 0;
}

/* ── dash tokens ─────────────────────────────────────────────────── */

static int cmd_tokens(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    double cum = 0, today = 0, sample = -1;
    bool has_cum    = tj_object_get_double(json, end, "cumulative",    &cum);
    bool has_today  = tj_object_get_double(json, end, "today",         &today);
    bool has_sample = tj_object_get_double(json, end, "latest_sample", &sample);
    char kind[AGENT_KIND_MAX] = {0};
    char sid[AGENT_SESSION_ID_MAX] = {0};
    tj_object_get_string(json, end, "agent_kind", kind, sizeof(kind));
    tj_object_get_string(json, end, "session_id", sid,  sizeof(sid));

    agent_state_lock();
    agent_slot_t *slot = agent_state_acquire_slot(kind[0] ? kind : "claude-code", sid);
    if (slot) {
        if (has_cum)   slot->tokens_cumulative = (uint64_t)cum;
        if (has_today) slot->tokens_today      = (uint64_t)today;
        if (has_sample && sample >= 0)
            agent_state_push_spark(slot, (uint32_t)sample);
    }
    agent_state_unlock();

    console_reply_ok("{\"tokens\":\"updated\"}");
    return 0;
}

/* ── dash idle ───────────────────────────────────────────────────── */

static int cmd_idle(const console_args_t *a)
{
    (void)a;
    if (!switch_scene("idle")) {
        console_reply_err("idle scene missing");
        return 0;
    }
    console_reply_ok("{\"scene\":\"idle\"}");
    return 0;
}

/* ── dash config ─────────────────────────────────────────────────── */

static void persist_string(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void persist_u8(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

void agent_commands_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        /* No config yet — set defaults. */
        agent_state_lock();
        agent_state_t *s = agent_state_get();
        strcpy(s->device_name, "DASHBOARD");
        strcpy(s->default_scene, "dashboard");
        agent_state_unlock();
        return;
    }
    size_t cap;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    cap = sizeof(s->device_name);
    if (nvs_get_str(h, "device_name", s->device_name, &cap) != ESP_OK) {
        strcpy(s->device_name, "DASHBOARD");
    }
    cap = sizeof(s->owner);
    if (nvs_get_str(h, "owner", s->owner, &cap) != ESP_OK) s->owner[0] = '\0';
    cap = sizeof(s->default_scene);
    if (nvs_get_str(h, "default_scene", s->default_scene, &cap) != ESP_OK) {
        strcpy(s->default_scene, "dashboard");
    }
    char theme_buf[16] = {0};
    cap = sizeof(theme_buf);
    if (nvs_get_str(h, "theme", theme_buf, &cap) == ESP_OK) {
        theme_set_by_name(theme_buf);
    }
    uint8_t mr = 0;
    if (nvs_get_u8(h, "motion_red", &mr) == ESP_OK) {
        s->motion_reduced = (mr != 0);
    }
    agent_state_unlock();
    nvs_close(h);
}

static int cmd_config(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    char device_name[AGENT_DEVICE_NAME_MAX] = {0};
    char owner[AGENT_OWNER_MAX] = {0};
    char theme_buf[16] = {0};
    char default_scene[AGENT_DEFAULT_SCENE_MAX] = {0};
    char motion_buf[8] = {0};
    bool has_dev   = tj_object_get_string(json, end, "device_name",     device_name,   sizeof(device_name));
    bool has_own   = tj_object_get_string(json, end, "owner",           owner,         sizeof(owner));
    bool has_theme = tj_object_get_string(json, end, "theme",           theme_buf,     sizeof(theme_buf));
    bool has_def   = tj_object_get_string(json, end, "default_scene",   default_scene, sizeof(default_scene));
    bool has_mr    = tj_object_get_string(json, end, "motion_reduced",  motion_buf,    sizeof(motion_buf));

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    if (has_dev) { strncpy(s->device_name,   device_name,   sizeof(s->device_name) - 1);   s->device_name[sizeof(s->device_name)-1]='\0'; }
    if (has_own) { strncpy(s->owner,         owner,         sizeof(s->owner) - 1);         s->owner[sizeof(s->owner)-1]='\0'; }
    if (has_def) { strncpy(s->default_scene, default_scene, sizeof(s->default_scene) - 1); s->default_scene[sizeof(s->default_scene)-1]='\0'; }
    if (has_mr)  { s->motion_reduced = (strcmp(motion_buf, "true") == 0 || strcmp(motion_buf, "1") == 0); }
    bool theme_ok = true;
    if (has_theme) theme_ok = theme_set_by_name(theme_buf);
    agent_state_unlock();

    if (has_dev)   persist_string("device_name",   device_name);
    if (has_own)   persist_string("owner",         owner);
    if (has_theme && theme_ok) persist_string("theme", theme_buf);
    if (has_def)   persist_string("default_scene", default_scene);
    if (has_mr)    persist_u8("motion_red", s->motion_reduced ? 1 : 0);

    console_reply_ok("{\"config\":\"applied\",\"theme\":\"%s\"}", theme_current_name());
    return 0;
}

/* ── dash time ───────────────────────────────────────────────────── */

static int cmd_time(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;
    double epoch = 0, tz = 0;
    bool has_epoch = tj_object_get_double(json, end, "epoch_unix",         &epoch);
    bool has_tz    = tj_object_get_double(json, end, "tz_offset_seconds",  &tz);

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    if (has_epoch) {
        s->host_epoch_unix = (uint32_t)epoch;
        s->host_clock_received_ms = lv_tick_get();
    }
    if (has_tz) s->host_tz_offset_seconds = (int32_t)tz;
    agent_state_unlock();

    console_reply_ok("{\"time\":\"set\"}");
    return 0;
}

/* ── dash health ─────────────────────────────────────────────────── */

static int cmd_health(const console_args_t *a)
{
    (void)a;
    char buf[512];
    char dn[AGENT_DEVICE_NAME_MAX], own[AGENT_OWNER_MAX];
    int agent_count;
    uint32_t snaps, prompts, decisions;
    uint32_t last_snap_ms;
    bool ever;

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    strncpy(dn,  s->device_name, sizeof(dn));  dn[sizeof(dn)-1]   = '\0';
    strncpy(own, s->owner,       sizeof(own)); own[sizeof(own)-1] = '\0';
    agent_count = s->slot_count;
    snaps = s->snapshots_received;
    prompts = s->prompts_received;
    decisions = s->decisions_sent;
    last_snap_ms = s->last_snapshot_ms;
    ever = s->ever_received;
    agent_state_unlock();

    const scene_t *cur = scene_fw_current();
    const char *scene_id = cur ? cur->id : "?";

    int64_t up_us = esp_timer_get_time();
    uint32_t up_s = (uint32_t)(up_us / 1000000ULL);
    size_t heap_free = esp_get_free_heap_size();
    size_t heap_min  = esp_get_minimum_free_heap_size();

    uint32_t conn_age_s = 0;
    if (ever) {
        uint32_t now = lv_tick_get();
        conn_age_s = (now - last_snap_ms) / 1000u;
    }

    int n = snprintf(buf, sizeof(buf),
        "{\"device_name\":\"%s\",\"owner\":\"%s\",\"scene\":\"%s\","
        "\"theme\":\"%s\",\"uptime_s\":%u,\"heap_free\":%u,"
        "\"heap_min\":%u,\"snapshots_received\":%u,\"prompts_received\":%u,"
        "\"decisions_sent\":%u,\"connection_age_s\":%u,\"agent_count\":%d}",
        dn, own, scene_id, theme_current_name(),
        (unsigned)up_s, (unsigned)heap_free, (unsigned)heap_min,
        (unsigned)snaps, (unsigned)prompts, (unsigned)decisions,
        (unsigned)conn_age_s, agent_count);

    if (n < 0) { console_reply_err("snprintf"); return 0; }

    char meta[64];
    snprintf(meta, sizeof(meta), "fmt=json bytes=%d", n);
    console_reply_ok("payload follows tag=HEALTH");
    console_begin_payload("HEALTH", meta);
    console_write_raw(buf, (size_t)n);
    console_end_payload("HEALTH");
    return 0;
}

/* ── dash push ──────────────────────────────────────────────────── */

static int cmd_push(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    char tool[48] = {0};
    char hint[48] = {0};
    double dur = 0;
    tj_object_get_string(json, end, "tool", tool, sizeof(tool));
    tj_object_get_string(json, end, "hint", hint, sizeof(hint));
    bool has_dur = tj_object_get_double(json, end, "duration_ms", &dur);

    if (tool[0] == '\0') {
        console_reply_err("push requires \"tool\"");
        return 0;
    }
    push_banner_show(tool, hint[0] ? hint : NULL,
                     has_dur ? (uint32_t)dur : 0);
    console_reply_ok("{\"push\":\"shown\"}");
    return 0;
}

/* ── Single command + sub-dispatch ────────────────────────────────── */

static int cmd_dash(const console_args_t *a)
{
    if (a->argc < 2) {
        console_reply_err("dash needs a subcommand "
                          "(snapshot|prompt|event|tokens|idle|config|time|health|push)");
        return 0;
    }
    const char *sub = a->argv[1];
    if      (strcmp(sub, "snapshot") == 0) return cmd_snapshot(a);
    else if (strcmp(sub, "prompt")   == 0) return cmd_prompt(a);
    else if (strcmp(sub, "event")    == 0) return cmd_event(a);
    else if (strcmp(sub, "tokens")   == 0) return cmd_tokens(a);
    else if (strcmp(sub, "idle")     == 0) return cmd_idle(a);
    else if (strcmp(sub, "config")   == 0) return cmd_config(a);
    else if (strcmp(sub, "time")     == 0) return cmd_time(a);
    else if (strcmp(sub, "health")   == 0) return cmd_health(a);
    else if (strcmp(sub, "push")     == 0) return cmd_push(a);
    console_reply_err("unknown dash subcommand: %s", sub);
    return 0;
}

static const console_cmd_t s_cmd_dash = {
    "dash",
    cmd_dash,
    "dash <snapshot|prompt|event|tokens|idle|config|time|health|push> [json]"
};

void agent_commands_register(void)
{
    console_protocol_register(&s_cmd_dash);
    ESP_LOGI(TAG, "dash command registered");
}
