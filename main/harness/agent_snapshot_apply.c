/*
 * agent_snapshot_apply - snapshot JSON semantics for dash snapshot.
 */

#include "agent_snapshot_apply.h"

#include "../tiny_json.h"

#include <string.h>

#include "lvgl.h"

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static bool looks_like_object(const char *json, const char *end)
{
    if (!json || !end || end <= json) return false;
    const char *p = skip_ws(json, end);
    return p < end && *p == '{';
}

static agent_status_t parse_status(const char *s)
{
    if (!s) return AGENT_STATUS_IDLE;
    if (strcmp(s, "running") == 0) return AGENT_STATUS_RUNNING;
    if (strcmp(s, "waiting") == 0) return AGENT_STATUS_WAITING;
    return AGENT_STATUS_IDLE;
}

/* Does this JSON object carry at least one field that identifies it as an
 * agent? Guards against a malformed or non-agent line (an empty ``{}``, a
 * truncated payload, or the ``{"protocol":"v1"}`` handshake) being treated as
 * a single-agent v0 snapshot — which would phantom-create a "claude-code" slot
 * AND prune every real agent, wiping the screen on one bad line. */
static bool object_has_agent_fields(const char *b, const char *e)
{
    static const char *const keys[] = {
        "session_id", "status", "msg", "cwd", "entries",
        "tokens", "tokens_today", "last_active_unix", "awaiting_kind",
    };
    tj_span_t v;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (tj_object_find(b, e, keys[i], &v)) return true;
    }
    return false;
}

static bool merge_agent_object(tj_span_t obj, const char *default_kind)
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
    if (!slot) return false;

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

    char awaiting_kind_str[16] = {0};
    if (tj_object_get_string(obj.begin, obj.end, "awaiting_kind",
                             awaiting_kind_str, sizeof(awaiting_kind_str))
        && awaiting_kind_str[0] != '\0')
    {
        awaiting_kind_t k = agent_state_parse_awaiting_kind(awaiting_kind_str);
        double since = 0;
        tj_object_get_double(obj.begin, obj.end, "awaiting_since", &since);

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

    return true;
}

static void compute_added_slots(agent_snapshot_apply_result_t *out,
                                bool before_inuse[AGENT_SLOT_MAX],
                                char before_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX],
                                char before_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX],
                                bool after_inuse[AGENT_SLOT_MAX],
                                char after_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX],
                                char after_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX])
{
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        bool was = before_inuse[i];
        bool now = after_inuse[i];
        bool ident_changed = (memcmp(before_kind[i], after_kind[i], AGENT_KIND_MAX) != 0 ||
                              memcmp(before_sid[i],  after_sid[i],  AGENT_SESSION_ID_MAX) != 0);
        if (now && (!was || ident_changed) && out->added_count < AGENT_SLOT_MAX) {
            memcpy(out->added_kind[out->added_count], after_kind[i], AGENT_KIND_MAX);
            memcpy(out->added_sid[out->added_count],  after_sid[i],  AGENT_SESSION_ID_MAX);
            out->added_count++;
        }
    }
}

bool agent_snapshot_apply_json(const char *json,
                               const char *end,
                               agent_snapshot_apply_result_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!looks_like_object(json, end)) return false;

    tj_span_t agents_v;
    bool has_agents = tj_object_find(json, end, "agents", &agents_v) &&
                      tj_value_is_array(agents_v);

    tj_span_t prompt_v;
    bool prompt_found = tj_object_find(json, end, "prompt", &prompt_v);
    char prompt_id_buf[AGENT_PROMPT_ID_MAX]   = {0};
    char prompt_tool_buf[AGENT_TOOL_MAX]      = {0};
    char prompt_hint_buf[AGENT_HINT_MAX]      = {0};
    char prompt_kind_buf[AGENT_KIND_MAX]      = {0};
    char prompt_sid_buf[AGENT_SESSION_ID_MAX] = {0};
    if (prompt_found) {
        if (tj_value_is_null(prompt_v)) {
            out->prompt_clear = true;
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
            out->prompt_set = (prompt_id_buf[0] != '\0');
        }
    }

    char before_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    char before_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX];
    bool before_inuse[AGENT_SLOT_MAX];
    char after_kind[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    char after_sid[AGENT_SLOT_MAX][AGENT_SESSION_ID_MAX];
    bool after_inuse[AGENT_SLOT_MAX];

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
            if (!object_has_agent_fields(it.begin, it.end)) continue;
            if (!merge_agent_object(it, NULL)) out->dropped_count++;
        }
        out->removed_count = agent_state_prune_unmarked(out->removed_kind,
                                                        out->removed_sid);

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
    } else if (object_has_agent_fields(json, end)) {
        tj_span_t flat = { json, end };
        if (!merge_agent_object(flat, "claude-code")) out->dropped_count++;
        out->removed_count = agent_state_prune_unmarked(out->removed_kind,
                                                        out->removed_sid);

        double td, rd, wd, tk, tkt;
        if (tj_object_get_double(json, end, "total",        &td))  s->total   = (int)td;
        if (tj_object_get_double(json, end, "running",      &rd))  s->running = (int)rd;
        if (tj_object_get_double(json, end, "waiting",      &wd))  s->waiting = (int)wd;
        if (tj_object_get_double(json, end, "tokens",       &tk))  s->tokens_cumulative = (uint64_t)tk;
        if (tj_object_get_double(json, end, "tokens_today", &tkt)) s->tokens_today      = (uint64_t)tkt;
    }

    /* v5.2: the on-device prompt takeover is RETIRED (the user approves
     * in the terminal, not on the panel). Prompt metadata is still
     * recorded for `dash health` counters, but prompt_active must NEVER
     * go true — a stale true would suppress the awaiting takeover and
     * the screensaver forever now that nothing clears it via a scene. */
    if (out->prompt_set) {
        memcpy(s->prompt_id,         prompt_id_buf,   sizeof(s->prompt_id));
        memcpy(s->prompt_tool,       prompt_tool_buf, sizeof(s->prompt_tool));
        memcpy(s->prompt_hint,       prompt_hint_buf, sizeof(s->prompt_hint));
        memcpy(s->prompt_agent_kind, prompt_kind_buf, sizeof(s->prompt_agent_kind));
        memcpy(s->prompt_session_id, prompt_sid_buf,  sizeof(s->prompt_session_id));
        s->prompt_active = false;
        s->prompt_shown_ms = lv_tick_get();
        s->prompts_received++;
    } else if (out->prompt_clear) {
        s->prompt_active = false;
        s->prompt_id[0] = s->prompt_tool[0] = s->prompt_hint[0] = '\0';
        s->prompt_agent_kind[0] = s->prompt_session_id[0] = '\0';
    }

    s->last_snapshot_ms  = lv_tick_get();
    s->ever_received     = true;
    s->snapshots_received++;
    if (out->dropped_count > 0) {
        s->snapshot_dropped_agents += (uint32_t)out->dropped_count;
    }
    out->total_now = s->total ? s->total : s->slot_count;

    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        after_inuse[i] = s->slots[i].in_use;
        memcpy(after_kind[i], s->slots[i].kind,       AGENT_KIND_MAX);
        memcpy(after_sid[i],  s->slots[i].session_id, AGENT_SESSION_ID_MAX);
    }
    agent_state_unlock();

    compute_added_slots(out, before_inuse, before_kind, before_sid,
                        after_inuse, after_kind, after_sid);
    return true;
}
