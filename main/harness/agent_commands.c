/*
 * agent_commands — single `dash` console command with subcommand dispatch.
 *
 *   dash snapshot <json>   — full state push (counters + entries + tokens
 *                            + optional prompt)
 *   dash prompt   <json>   — set prompt_active + switch scene
 *   dash event    <json>   — one-shot transcript line
 *   dash tokens   <json>   — update token counters + push a sparkline sample
 *   dash idle              — switch back to scene_idle
 *
 * The host bridge ships the JSON payload as ONE argv-token, leveraging
 * the console tokenizer's double-quote support. Because the harness
 * registry keys on argv[0] only (whitespace-tokenised), we register a
 * single `dash` entry and dispatch on argv[1] inside this file. argv[2]
 * is the JSON.
 *
 * JSON parsing uses our local tiny_json (cJSON was removed from IDF v6
 * and we don't want a managed-component dep just for this).
 */

#include "agent_commands.h"
#include "../agent_state.h"
#include "../tiny_json.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "harness/console_protocol.h"
#include "harness/scene_framework.h"

static const char *TAG = "agent_cmd";

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

/* ── dash snapshot ───────────────────────────────────────────────── */

static int cmd_snapshot(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    /* Read numbers / strings */
    double total_d, run_d, wait_d, tk_d, tkt_d;
    bool has_total = tj_object_get_double(json, end, "total", &total_d);
    bool has_run   = tj_object_get_double(json, end, "running", &run_d);
    bool has_wait  = tj_object_get_double(json, end, "waiting", &wait_d);
    bool has_tk    = tj_object_get_double(json, end, "tokens", &tk_d);
    bool has_tkt   = tj_object_get_double(json, end, "tokens_today", &tkt_d);

    char msg_buf[AGENT_MSG_MAX];
    bool has_msg = tj_object_get_string(json, end, "msg", msg_buf, sizeof(msg_buf));

    /* Entries */
    tj_span_t entries_v;
    bool has_entries = tj_object_find(json, end, "entries", &entries_v) &&
                       tj_value_is_array(entries_v);

    /* Prompt: object means set; null means clear; missing means leave. */
    tj_span_t prompt_v;
    bool prompt_found = tj_object_find(json, end, "prompt", &prompt_v);
    bool prompt_set = false, prompt_clear = false;
    char prompt_id_buf[AGENT_PROMPT_ID_MAX];
    char prompt_tool_buf[AGENT_TOOL_MAX];
    char prompt_hint_buf[AGENT_HINT_MAX];
    prompt_id_buf[0] = prompt_tool_buf[0] = prompt_hint_buf[0] = '\0';
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
            prompt_set = (prompt_id_buf[0] != '\0');
        }
    }

    agent_state_lock();
    agent_state_t *s = agent_state_get();

    if (has_total) s->total   = (int)total_d;
    if (has_run)   s->running = (int)run_d;
    if (has_wait)  s->waiting = (int)wait_d;
    if (has_tk)    s->tokens_cumulative = (uint64_t)tk_d;
    if (has_tkt)   s->tokens_today      = (uint64_t)tkt_d;
    if (has_msg) {
        size_t n = strnlen(msg_buf, sizeof(s->msg) - 1);
        memcpy(s->msg, msg_buf, n);
        s->msg[n] = '\0';
    }

    if (has_entries) {
        /* Replace ring: walk array, take last AGENT_ENTRY_COUNT items so
         * newest ends at idx 0 in the ring (we push them in order). */
        memset(s->entries, 0, sizeof(s->entries));
        s->entry_count = 0;

        /* First, count items so we know how many to skip. */
        int n_items = 0;
        const char *cursor = NULL;
        tj_span_t it;
        while (tj_array_next(entries_v, cursor, &it)) {
            n_items++;
            cursor = it.end;
        }
        int skip = (n_items > AGENT_ENTRY_COUNT) ? (n_items - AGENT_ENTRY_COUNT) : 0;
        cursor = NULL;
        int i = 0;
        while (tj_array_next(entries_v, cursor, &it)) {
            if (i++ < skip) { cursor = it.end; continue; }
            char role[16] = {0};
            char text[AGENT_ENTRY_TEXT_MAX] = {0};
            if (tj_value_is_object(it)) {
                tj_object_get_string(it.begin, it.end, "role",
                                     role, sizeof(role));
                /* Accept both `text` and (for OpenAI-ish shape) `content` */
                if (!tj_object_get_string(it.begin, it.end, "text",
                                          text, sizeof(text))) {
                    tj_object_get_string(it.begin, it.end, "content",
                                         text, sizeof(text));
                }
            } else if (it.begin < it.end && *it.begin == '"') {
                /* Plain string entries are treated as role=msg text=<the string>. */
                tj_value_string(it, text, sizeof(text));
                strcpy(role, "msg");
            }
            agent_state_push_entry(role, text);
            cursor = it.end;
        }
    }

    if (prompt_set) {
        memcpy(s->prompt_id,   prompt_id_buf,   sizeof(s->prompt_id));
        memcpy(s->prompt_tool, prompt_tool_buf, sizeof(s->prompt_tool));
        memcpy(s->prompt_hint, prompt_hint_buf, sizeof(s->prompt_hint));
        s->prompt_active = true;
        s->prompt_shown_ms = lv_tick_get();
    } else if (prompt_clear) {
        s->prompt_active = false;
        s->prompt_id[0] = s->prompt_tool[0] = s->prompt_hint[0] = '\0';
    }

    int total_now = s->total;
    agent_state_unlock();

    /* Auto-pick scene based on new state. Only move if we'd benefit. */
    const scene_t *cur = scene_fw_current();
    const char *cur_id = cur ? cur->id : "";
    if (prompt_set) {
        switch_scene("prompt");
    } else if (prompt_clear && strcmp(cur_id, "prompt") == 0) {
        switch_scene("idle");
    } else if (total_now > 0 && strcmp(cur_id, "idle") == 0) {
        switch_scene("sessions");
    } else if (total_now == 0 && strcmp(cur_id, "sessions") == 0) {
        switch_scene("idle");
    }

    console_reply_ok("{\"applied\":true}");
    return 0;
}

/* ── dash prompt ─────────────────────────────────────────────────── */

static int cmd_prompt(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    char id[AGENT_PROMPT_ID_MAX]   = {0};
    char tool[AGENT_TOOL_MAX]      = {0};
    char hint[AGENT_HINT_MAX]      = {0};
    tj_object_get_string(json, end, "id",   id,   sizeof(id));
    tj_object_get_string(json, end, "tool", tool, sizeof(tool));
    tj_object_get_string(json, end, "hint", hint, sizeof(hint));

    if (id[0] == '\0') { console_reply_err("prompt id required"); return 0; }

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    memcpy(s->prompt_id,   id,   sizeof(s->prompt_id));
    memcpy(s->prompt_tool, tool, sizeof(s->prompt_tool));
    memcpy(s->prompt_hint, hint, sizeof(s->prompt_hint));
    s->prompt_active = true;
    s->prompt_shown_ms = lv_tick_get();
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
    tj_object_get_string(json, end, "role", role, sizeof(role));

    /* content can be a string or an array of {type,text} items. Prefer
     * the first .text we find. */
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
    agent_state_push_entry(role, text);
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

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    if (has_cum)   s->tokens_cumulative = (uint64_t)cum;
    if (has_today) s->tokens_today      = (uint64_t)today;
    if (has_sample && sample >= 0)
        agent_state_push_spark((uint32_t)sample);
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

/* ── Single command + sub-dispatch ────────────────────────────────── */

static int cmd_dash(const console_args_t *a)
{
    if (a->argc < 2) {
        console_reply_err("dash needs a subcommand "
                          "(snapshot|prompt|event|tokens|idle)");
        return 0;
    }
    const char *sub = a->argv[1];
    if      (strcmp(sub, "snapshot") == 0) return cmd_snapshot(a);
    else if (strcmp(sub, "prompt")   == 0) return cmd_prompt(a);
    else if (strcmp(sub, "event")    == 0) return cmd_event(a);
    else if (strcmp(sub, "tokens")   == 0) return cmd_tokens(a);
    else if (strcmp(sub, "idle")     == 0) return cmd_idle(a);
    console_reply_err("unknown dash subcommand: %s", sub);
    return 0;
}

static const console_cmd_t s_cmd_dash = {
    "dash",
    cmd_dash,
    "dash <snapshot|prompt|event|tokens|idle> [json]"
};

void agent_commands_register(void)
{
    console_protocol_register(&s_cmd_dash);
    ESP_LOGI(TAG, "dash command registered");
}
