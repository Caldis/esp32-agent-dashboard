/* wasm_api.c — JS/ctypes ↔ 数据层的契约。第 1 步先建最小入口;
 * state_json / dash_feed_line / drain_signals 在后续任务补全。 */
#include <stdio.h>
#include <string.h>
#include "agent_state.h"
#include "agent_commands.h"
#include "harness/console_protocol.h"

extern const char *shim_last_reply(void);
extern int         shim_last_reply_is_err(void);
extern const console_cmd_t *shim_find_cmd(const char *name);
extern const char *shim_current_scene_id(void);

const char *last_reply(void)        { return shim_last_reply(); }
int         last_reply_is_err(void) { return shim_last_reply_is_err(); }
const char *current_scene(void)     { return shim_current_scene_id(); }

/* G-7 tokeniser —— 移植自 mock_device_v1.py._tokenise / 固件 console_protocol.c:
 *  - 以 '"' 起始的 token:去掉前导 '"',累积所有字符(含内层 '"' 和空白)
 *    直到「后面紧跟空白或行尾」的那个 '"' 收尾;
 *  - 非 '"' 起始的 token:遇到任意 '"' 切换 in_quote,所有 '"' 被剥除。
 * 把切分结果写进 argv_buf(NUL 分隔)与 argv[](≤CONSOLE_MAX_ARGS)。 */
static int tokenise(const char *line, char *buf, size_t bufcap,
                    const char *argv[], int max_args) {
    int argc = 0; size_t w = 0; size_t n = strlen(line); size_t i = 0;
    while (i < n && argc < max_args) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= n) break;
        if (w >= bufcap) break;
        argv[argc] = &buf[w];
        if (line[i] == '"') {
            i++;                                  /* drop leading " */
            int close = -1;
            for (size_t j = i; j < n; ++j) {
                if (line[j] == '"' && (j + 1 == n || line[j+1] == ' ' || line[j+1] == '\t')) {
                    close = (int)j; break;
                }
            }
            size_t endp = (close == -1) ? n : (size_t)close;
            for (size_t j = i; j < endp && w + 1 < bufcap; ++j) buf[w++] = line[j];
            i = (close == -1) ? n : (size_t)close + 1;
        } else {
            int in_q = 0;
            while (i < n) {
                char ch = line[i];
                if (!in_q && (ch == ' ' || ch == '\t')) break;
                if (ch == '"') { in_q = !in_q; i++; continue; }
                if (w + 1 < bufcap) buf[w++] = ch;
                i++;
            }
        }
        if (w < bufcap) buf[w++] = 0;             /* NUL-terminate token */
        argc++;
    }
    return argc;
}

int dash_feed_line(const char *line) {
    char buf[CONSOLE_MAX_LINE];
    const char *argv[CONSOLE_MAX_ARGS];
    console_args_t args;
    args.argc = tokenise(line, buf, sizeof(buf), argv, CONSOLE_MAX_ARGS);
    for (int i = 0; i < args.argc; ++i) args.argv[i] = argv[i];
    for (int i = args.argc; i < CONSOLE_MAX_ARGS; ++i) args.argv[i] = NULL;
    if (args.argc < 1) return -1;
    const console_cmd_t *cmd = shim_find_cmd(args.argv[0]);
    if (!cmd) return -1;
    cmd->fn(&args);
    return 0;
}

void dash_init(void) {
    agent_state_init();
    agent_commands_register();     /* 经 shim 捕获命令表 */
    agent_commands_load_config();  /* 设默认 device_name="DASHBOARD" 等 */
}

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

/* json_str: 把 src 作为 JSON 字符串值(含转义)写入 dst[0..cap-1]。
 * 返回"实际写入字节数"(已写入 dst 的真实字节数,不含 NUL)。
 * 注意:与 snprintf 语义不同 — snprintf 返回"应写入字节数"(可超出 cap);
 * json_str 保证返回值 ≤ cap,且 dst[return_value] == '\0'。
 * cap < 3 时只能写空字符串 "" 或更少,始终保证 NUL 终止。 */
static size_t json_str(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    /* 开头引号:需要位置留给:开头 " + 闭合 " + NUL,共 3 字节 */
    if (cap < 2) {
        if (cap >= 1) dst[0] = '\0';
        return 0;
    }
    dst[n++] = '"';
    /* 每个字符最多写 2 字节(转义字符),退出时要留 闭合"(1) + NUL(1) = 2 字节 */
    for (const char *p = src; *p; ++p) {
        int need = (*p == '"' || *p == '\\') ? 2 : 1;
        if (n + need + 2 > cap) break; /* 确保退出后还有闭合"和NUL的位置 */
        if (*p == '"' || *p == '\\') dst[n++] = '\\';
        dst[n++] = *p;
    }
    dst[n++] = '"';  /* 闭合引号,此时 n <= cap-1 */
    dst[n]   = '\0'; /* NUL 终止,此时 n <= cap-1,所以 dst[n] 合法 */
    return n;
}

/* SAPP: 钳位追加宏,替代 n += snprintf(...)。
 * snprintf 返回"应写入字节数"(截断时可超出剩余空间),直接累加 n 会导致
 * cap-n 下溢为巨大 size_t 并使后续 snprintf 越界写。
 * 本宏将实际追加量钳位到剩余空间,确保 n 始终 ≤ cap-1。 */
#define SAPP(...)  do {                                         \
    if (n + 1 < cap) {                                          \
        int _r = snprintf(o + n, cap - n, __VA_ARGS__);        \
        if (_r > 0) {                                           \
            size_t _w = (size_t)_r;                             \
            n += (_w < cap - n) ? _w : (cap - n - 1);          \
        }                                                       \
    }                                                           \
} while (0)

const char *state_json(void) {
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    char *o = s_state; size_t cap = sizeof(s_state); size_t n = 0;

    /* SAPP 使用 snprintf 语义:返回"应写入字节数",由宏钳位后安全追加。
     * json_str 使用实际写入语义:返回值 ≤ cap-n,直接追加 n 安全。 */
    SAPP("{\"scene\":");
    n += json_str(o + n, cap - n, shim_current_scene_id()); /* json_str: 实际写入 */
    SAPP(",\"device_name\":");
    n += json_str(o + n, cap - n, s->device_name);
    SAPP(",\"owner\":");
    n += json_str(o + n, cap - n, s->owner);
    SAPP(",\"totals\":{\"total\":%d,\"running\":%d,\"waiting\":%d,"
         "\"tokens\":%llu,\"tokens_today\":%llu}",
         s->total, s->running, s->waiting,
         (unsigned long long)s->tokens_cumulative,
         (unsigned long long)s->tokens_today);
    SAPP(",\"prompt\":{\"active\":%s,\"id\":",
         s->prompt_active ? "true" : "false");
    n += json_str(o + n, cap - n, s->prompt_id);
    SAPP("}");

    SAPP(",\"slots\":[");
    int first = 1;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        agent_slot_t *sl = &s->slots[i];
        if (!sl->in_use) continue;
        if (!first) SAPP(",");
        first = 0;
        SAPP("{\"kind\":");
        n += json_str(o + n, cap - n, sl->kind);
        SAPP(",\"session_id\":");
        n += json_str(o + n, cap - n, sl->session_id);
        SAPP(",\"status\":\"%s\"", status_str(sl->status));
        SAPP(",\"msg\":");
        n += json_str(o + n, cap - n, sl->msg);
        SAPP(",\"cwd\":");
        n += json_str(o + n, cap - n, sl->cwd);
        SAPP(",\"tokens\":%llu,\"tokens_today\":%llu,\"awaiting\":\"%s\"}",
             (unsigned long long)sl->tokens_cumulative,
             (unsigned long long)sl->tokens_today,
             awaiting_str(sl->awaiting_kind));
    }
    SAPP("]}");

    /* 确保末尾 NUL(SAPP 已维护 n <= cap-1,此处为防御性保障) */
    o[n < cap ? n : cap - 1] = '\0';

    #undef SAPP
    agent_state_unlock();
    return s_state;
}
