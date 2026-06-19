/* wasm_api.c — JS/ctypes ↔ 数据层的契约。第 1 步先建最小入口;
 * state_json / dash_feed_line / drain_signals 在后续任务补全。 */
#include <stdio.h>
#include <string.h>
#include "agent_state.h"
#include "agent_commands.h"

void dash_init(void) {
    agent_state_init();
    agent_commands_register();     /* 经 shim 捕获命令表 */
    agent_commands_load_config();  /* 设默认 device_name="DASHBOARD" 等 */
}

extern const char *shim_current_scene_id(void);

static char s_state[4096];

static const char *status_str(agent_status_t st) {
    switch (st) {
        case AGENT_STATUS_RUNNING: return "running";
        case AGENT_STATUS_WAITING: return "waiting";
        default:                   return "idle";
    }
}
static const char *awaiting_str(awaiting_kind_t k) {
    switch (k) {
        case AWAITING_CONTINUE: return "continue";
        case AWAITING_APPROVE:  return "approve";
        case AWAITING_PICK:     return "pick";
        case AWAITING_TYPE:     return "type";
        case AWAITING_CLARIFY:  return "clarify";
        default:                return "none";
    }
}
/* 把 src 作为 JSON 字符串值(含转义)写入 dst,返回写入字节数。 */
static int json_str(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (n < cap) dst[n++] = '"';
    for (const char *p = src; *p && n + 2 < cap; ++p) {
        if (*p == '"' || *p == '\\') dst[n++] = '\\';
        dst[n++] = *p;
    }
    if (n < cap) dst[n++] = '"';
    if (n < cap) dst[n] = 0;
    return (int)n;
}

const char *state_json(void) {
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    char *o = s_state; size_t cap = sizeof(s_state); int n = 0;

    n += snprintf(o + n, cap - n, "{\"scene\":");
    n += json_str(o + n, cap - n, shim_current_scene_id());
    n += snprintf(o + n, cap - n, ",\"device_name\":");
    n += json_str(o + n, cap - n, s->device_name);
    n += snprintf(o + n, cap - n, ",\"owner\":");
    n += json_str(o + n, cap - n, s->owner);
    n += snprintf(o + n, cap - n,
        ",\"totals\":{\"total\":%d,\"running\":%d,\"waiting\":%d,"
        "\"tokens\":%llu,\"tokens_today\":%llu}",
        s->total, s->running, s->waiting,
        (unsigned long long)s->tokens_cumulative,
        (unsigned long long)s->tokens_today);
    n += snprintf(o + n, cap - n, ",\"prompt\":{\"active\":%s,\"id\":",
                  s->prompt_active ? "true" : "false");
    n += json_str(o + n, cap - n, s->prompt_id);
    n += snprintf(o + n, cap - n, "}");

    n += snprintf(o + n, cap - n, ",\"slots\":[");
    int first = 1;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        agent_slot_t *sl = &s->slots[i];
        if (!sl->in_use) continue;
        if (!first) n += snprintf(o + n, cap - n, ",");
        first = 0;
        n += snprintf(o + n, cap - n, "{\"kind\":");
        n += json_str(o + n, cap - n, sl->kind);
        n += snprintf(o + n, cap - n, ",\"session_id\":");
        n += json_str(o + n, cap - n, sl->session_id);
        n += snprintf(o + n, cap - n, ",\"status\":\"%s\"", status_str(sl->status));
        n += snprintf(o + n, cap - n, ",\"msg\":");
        n += json_str(o + n, cap - n, sl->msg);
        n += snprintf(o + n, cap - n, ",\"cwd\":");
        n += json_str(o + n, cap - n, sl->cwd);
        n += snprintf(o + n, cap - n,
            ",\"tokens\":%llu,\"tokens_today\":%llu,\"awaiting\":\"%s\"}",
            (unsigned long long)sl->tokens_cumulative,
            (unsigned long long)sl->tokens_today,
            awaiting_str(sl->awaiting_kind));
    }
    n += snprintf(o + n, cap - n, "]}");
    agent_state_unlock();
    return s_state;
}
