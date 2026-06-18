# Web Dev Tools — 第 1 步:WASM 数据层(原生一致性)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把固件真实的数据处理 C 代码(`agent_state.c` / `agent_commands.c` / `agent_snapshot_apply.c` / `tiny_json.c`)用普通 `cc` 编译成宿主机动态库,经一个 C API 喂 `dash` 命令、读回 `agent_state`,用 Python 测试断言其行为与固件一致——验证"web 与 ESP 同数据链路/同 bug"在工程上可行(不碰浏览器、不需要 emsdk)。

**Architecture:** 数据层 4 个纯 C 文件**原样引用**(不复制)。所有固件平台依赖(FreeRTOS / LVGL / ESP / NVS / BSP / esp-harness console & scene)由 `tools/web/wasm/shim_include/` 的**替身头** + `wasm_shim.c` 的**桩实现**满足。新增 `wasm_api.c` 暴露 `dash_init / dash_feed_line / state_json / current_scene / drain_signals`。`shim` 的 `console_protocol_register` 捕获命令表,`dash_feed_line` 用 G-7 tokeniser + 最小分发器重建 runloop。Python 经 `ctypes` 加载库做一致性断言。

**Tech Stack:** C11、`cc`/`gcc`/`clang`(宿主原生)、Python 3.12 `ctypes`、bash 构建脚本。无第三方库。

## Global Constraints

- **不修改** `main/` 下任何固件 C 源(只引用)。如发现必须改,停下来上报。
- **不引入第三方依赖**(沿用项目"CI 只装 pyserial"的极简原则)。本步连 pyserial 都不需要。
- **不需要 emsdk**:本步只用宿主原生编译。
- C 源**引用而非复制**:数据层 `.c` 用 `main/` 原文件路径;只有平台依赖的**头/桩**是新写的。
- 跨平台:产物名按平台为 `libdash_datalayer.{so,dll,dylib}`;脚本与测试都不得硬编码单一扩展名。
- 字段上限以 `main/agent_state.h` 为准(`AGENT_SLOT_MAX=4`、`AGENT_MSG_MAX=128`、`AGENT_KIND_MAX=16`、`AGENT_SESSION_ID_MAX=32` 等),测试断言必须用这些 verbatim 值。
- 新文件全部位于 `tools/web/wasm/`;构建产物写入 `tools/web/wasm/build/` 并加入 `.gitignore`,不入库。

---

### Task 1: 替身头 + shim 桩 + 构建脚本 → 能编译出动态库

**Files:**
- Create: `tools/web/wasm/shim_include/lvgl.h`
- Create: `tools/web/wasm/shim_include/freertos/FreeRTOS.h`
- Create: `tools/web/wasm/shim_include/freertos/semphr.h`
- Create: `tools/web/wasm/shim_include/esp_err.h`
- Create: `tools/web/wasm/shim_include/esp_log.h`
- Create: `tools/web/wasm/shim_include/esp_system.h`
- Create: `tools/web/wasm/shim_include/esp_heap_caps.h`
- Create: `tools/web/wasm/shim_include/esp_timer.h`
- Create: `tools/web/wasm/shim_include/nvs.h`
- Create: `tools/web/wasm/shim_include/nvs_flash.h`
- Create: `tools/web/wasm/shim_include/bsp/esp-bsp.h`
- Create: `tools/web/wasm/shim_include/harness/console_protocol.h`(从 esp-harness 复制,无 LVGL 依赖)
- Create: `tools/web/wasm/shim_include/harness/scene_framework.h`(最小替身,不拉 LVGL)
- Create: `tools/web/wasm/wasm_shim.c`
- Create: `tools/web/wasm/wasm_api.c`(本任务仅含最小 `dash_init`)
- Create: `tools/web/wasm/build_native.sh`
- Modify: `.gitignore`(追加 `tools/web/wasm/build/`)

**Interfaces:**
- Produces:
  - 替身头满足 `main/{agent_state,tiny_json}.c`、`main/harness/{agent_snapshot_apply,agent_commands}.c` 的全部非标准 include。
  - `wasm_shim.c` 实现:`lv_tick_get`、FreeRTOS 信号量桩、`esp_*`/`bsp_*` 桩、内存版 `nvs_*`、`console_reply_ok/err`/`console_send_evt`/`console_begin_payload`/`console_write_raw`/`console_end_payload`/`console_protocol_register`、`scene_fw_find_by_id/show/current`、`theme_set_by_name`/`theme_current_name`、`push_banner_show`/`push_banner_dismiss`。
  - shim 内部供 `wasm_api.c` 使用的接口(声明在 `wasm_shim.c` 顶部、由 `wasm_api.c` extern):
    - `const char *shim_last_reply(void);` 返回最近一次 `console_reply_ok/err` 的正文。
    - `int shim_last_reply_is_err(void);`
    - `const console_cmd_t *shim_find_cmd(const char *name);` 查注册表。
    - `const char *shim_current_scene_id(void);` 当前场景 id(无则 `""`)。
    - `const char *shim_drain_signals_json(void);` 取走并清空信号队列,返回 JSON 数组字符串。
  - `build_native.sh` 产出 `tools/web/wasm/build/libdash_datalayer.{so,dll,dylib}`。

- [ ] **Step 1: 创建系统/平台替身头(组 A)**

`tools/web/wasm/shim_include/lvgl.h`:
```c
#pragma once
#include <stdint.h>
/* 最小 LVGL 替身:数据层只用到 lv_color_t / lv_color_hex(被 theme.h 的
 * static inline 引用但数据层不调用)与 lv_tick_get。 */
typedef struct { uint16_t full; } lv_color_t;
typedef void lv_obj_t;
static inline lv_color_t lv_color_hex(uint32_t hex) { (void)hex; lv_color_t c = {0}; return c; }
uint32_t lv_tick_get(void);   /* 由 wasm_shim.c 实现 */
```

`tools/web/wasm/shim_include/freertos/FreeRTOS.h`:
```c
#pragma once
#ifndef portMAX_DELAY
#define portMAX_DELAY 0xffffffffu
#endif
```

`tools/web/wasm/shim_include/freertos/semphr.h`:
```c
#pragma once
#include <stdint.h>
typedef void * SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t h, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t h);
```

`tools/web/wasm/shim_include/esp_err.h`:
```c
#pragma once
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK   0
#define ESP_FAIL -1
#endif
#define ESP_ERR_NVS_NOT_FOUND 0x1102
```

`tools/web/wasm/shim_include/esp_log.h`:
```c
#pragma once
/* 静默桩:数据层日志不参与一致性。 */
#define ESP_LOGE(tag, ...) do {} while (0)
#define ESP_LOGW(tag, ...) do {} while (0)
#define ESP_LOGI(tag, ...) do {} while (0)
#define ESP_LOGD(tag, ...) do {} while (0)
#define ESP_LOGV(tag, ...) do {} while (0)
```

`tools/web/wasm/shim_include/esp_system.h`:
```c
#pragma once
#include <stddef.h>
size_t esp_get_free_heap_size(void);
size_t esp_get_minimum_free_heap_size(void);
```

`tools/web/wasm/shim_include/esp_heap_caps.h`:
```c
#pragma once
/* 数据层未直接使用 heap_caps_* 接口;占位以满足 include。 */
```

`tools/web/wasm/shim_include/esp_timer.h`:
```c
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
```

- [ ] **Step 2: 创建 NVS / BSP 替身头(组 B)**

`tools/web/wasm/shim_include/nvs.h`:
```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);
```

`tools/web/wasm/shim_include/nvs_flash.h`:
```c
#pragma once
#include "esp_err.h"
esp_err_t nvs_flash_init(void);   /* 数据层不调用;占位满足 include */
```

`tools/web/wasm/shim_include/bsp/esp-bsp.h`:
```c
#pragma once
void bsp_display_lock(int timeout_ms);
void bsp_display_unlock(void);
```

- [ ] **Step 3: 创建 harness 替身头(组 C)**

`tools/web/wasm/shim_include/harness/console_protocol.h` — 直接复制 esp-harness 的真实头(它只依赖 `<stdarg.h>/<stddef.h>/<stdint.h>`,无 LVGL)。内容与 `D:\Code\esp-harness\components\esp-harness-core\include\harness\console_protocol.h` 完全一致(`console_args_t` / `console_cmd_t` / `console_handler_fn` / `console_protocol_init/register` / `console_reply_ok/err` / `console_send_evt` / `console_begin_payload/write_raw/end_payload`、宏 `CONSOLE_MAX_ARGS 8` / `CONSOLE_MAX_LINE 1024` / `CONSOLE_MAX_COMMANDS 32`)。在文件顶部加一行注释:`/* COPIED VERBATIM from esp-harness-core; interface only, no LVGL dep. */`

`tools/web/wasm/shim_include/harness/scene_framework.h`(最小替身,**不** include lvgl):
```c
#pragma once
/* 最小 scene 替身:数据层只用 id-based 查找/切换/读当前 id。
 * 真实 scene_t 含 LVGL 字段;此替身仅保留 id。 */
typedef struct scene { const char *id; } scene_t;
int            scene_fw_find_by_id(const char *id);
void           scene_fw_show(int idx);
const scene_t *scene_fw_current(void);
```

- [ ] **Step 4: 创建 `wasm_shim.c`(全部桩 + 捕获)**

`tools/web/wasm/wasm_shim.c`:
```c
/* wasm_shim.c — 用宿主可运行的桩满足固件平台依赖,并捕获
 * console reply / evt / scene 切换 / push banner 供 wasm_api 读取。
 * 与渲染无关;这是 ESP↔web 的契约面在数据层一侧的实现。 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* include 替身头,让编译器校验每个桩签名与固件期望一致 */
#include "lvgl.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "theme.h"            /* 经 -I main;拉替身 lvgl.h */
#include "push_banner.h"      /* 经 -I main;零 LVGL */
#include "harness/console_protocol.h"
#include "harness/scene_framework.h"

/* ── 时间 / 信号量 / 系统桩 ─────────────────────────────── */
static uint32_t s_tick = 0;
uint32_t lv_tick_get(void) { return ++s_tick; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }  /* 非 NULL 即可 */
int xSemaphoreTake(SemaphoreHandle_t h, uint32_t t) { (void)h; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t h) { (void)h; return 1; }

size_t  esp_get_free_heap_size(void) { return 84200; }
size_t  esp_get_minimum_free_heap_size(void) { return 78400; }
int64_t esp_timer_get_time(void) { return (int64_t)s_tick * 1000; }

void bsp_display_lock(int t) { (void)t; }
void bsp_display_unlock(void) {}

/* ── 内存版 NVS ─────────────────────────────────────────── */
#define NVS_MAX 32
typedef struct { char key[24]; char sval[64]; uint8_t u8; int is_u8; int used; } nvs_ent_t;
static nvs_ent_t s_nvs[NVS_MAX];
static int s_nvs_dirty = 0;   /* 是否写过任何键(决定 READONLY 是否成功) */

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)ns; *out = 1;
    /* 复现固件:首次(从未写过)以 READONLY 打开失败 → 触发默认配置路径。 */
    if (mode == NVS_READONLY && !s_nvs_dirty) return ESP_FAIL;
    return ESP_OK;
}
static nvs_ent_t *nvs_slot(const char *key, int create) {
    for (int i = 0; i < NVS_MAX; ++i)
        if (s_nvs[i].used && strcmp(s_nvs[i].key, key) == 0) return &s_nvs[i];
    if (!create) return NULL;
    for (int i = 0; i < NVS_MAX; ++i)
        if (!s_nvs[i].used) {
            s_nvs[i].used = 1;
            strncpy(s_nvs[i].key, key, sizeof(s_nvs[i].key) - 1);
            return &s_nvs[i];
        }
    return NULL;
}
esp_err_t nvs_set_str(uint32_t h, const char *key, const char *val) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 1); if (!e) return ESP_FAIL;
    strncpy(e->sval, val, sizeof(e->sval) - 1); e->sval[sizeof(e->sval)-1] = 0;
    e->is_u8 = 0; s_nvs_dirty = 1; return ESP_OK;
}
esp_err_t nvs_get_str(uint32_t h, const char *key, char *out, size_t *len) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 0); if (!e || e->is_u8) return ESP_FAIL;
    size_t n = strlen(e->sval); if (len && *len <= n) return ESP_FAIL;
    strcpy(out, e->sval); if (len) *len = n + 1; return ESP_OK;
}
esp_err_t nvs_set_u8(uint32_t h, const char *key, uint8_t v) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 1); if (!e) return ESP_FAIL;
    e->u8 = v; e->is_u8 = 1; s_nvs_dirty = 1; return ESP_OK;
}
esp_err_t nvs_get_u8(uint32_t h, const char *key, uint8_t *out) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 0); if (!e || !e->is_u8) return ESP_FAIL;
    *out = e->u8; return ESP_OK;
}
esp_err_t nvs_commit(uint32_t h) { (void)h; return ESP_OK; }
void      nvs_close(uint32_t h) { (void)h; }

/* ── theme 桩 ───────────────────────────────────────────── */
static char s_theme[16] = "noir";
bool theme_set_by_name(const char *name) {
    if (!name) return false;
    if (strcmp(name, "noir") && strcmp(name, "lab") && strcmp(name, "mono")) return false;
    strncpy(s_theme, name, sizeof(s_theme) - 1); s_theme[sizeof(s_theme)-1] = 0;
    return true;
}
const char *theme_current_name(void) { return s_theme; }

/* ── push banner 桩 → 信号队列 ──────────────────────────── */
static void signals_push(const char *line);  /* fwd */
void push_banner_show(const char *tool, const char *hint, uint32_t dur) {
    char buf[160];
    snprintf(buf, sizeof(buf), "push tool=%s hint=%s dur=%u",
             tool ? tool : "", hint ? hint : "", (unsigned)dur);
    signals_push(buf);
}
void push_banner_dismiss(void) { signals_push("push_dismiss"); }

/* ── console reply 捕获 ─────────────────────────────────── */
static char s_reply[CONSOLE_MAX_LINE];
static int  s_reply_is_err = 0;
void console_reply_ok(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_reply, sizeof(s_reply), fmt, ap); va_end(ap); s_reply_is_err = 0;
}
void console_reply_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_reply, sizeof(s_reply), fmt, ap); va_end(ap); s_reply_is_err = 1;
}
const char *shim_last_reply(void) { return s_reply; }
int shim_last_reply_is_err(void) { return s_reply_is_err; }

/* payload(HEALTH 等):拼进 reply 末尾,便于测试观测。 */
void console_begin_payload(const char *tag, const char *meta) {
    size_t n = strlen(s_reply);
    snprintf(s_reply + n, sizeof(s_reply) - n, " |%s_BEGIN %s|", tag ? tag : "", meta ? meta : "");
}
void console_write_raw(const char *data, size_t len) {
    size_t n = strlen(s_reply);
    if (n + len < sizeof(s_reply)) { memcpy(s_reply + n, data, len); s_reply[n + len] = 0; }
}
void console_end_payload(const char *tag) {
    size_t n = strlen(s_reply);
    snprintf(s_reply + n, sizeof(s_reply) - n, " |%s_END|", tag ? tag : "");
}

/* ── EVT → 信号队列 ─────────────────────────────────────── */
#define SIG_CAP 4096
static char s_signals[SIG_CAP];
static size_t s_sig_len = 0;
static void signals_push(const char *line) {
    /* 以 JSON 字符串元素累积:"...","..." */
    size_t need = strlen(line) + 4;
    if (s_sig_len + need >= SIG_CAP) return;          /* 满了静默丢弃(测试不应触达) */
    if (s_sig_len) s_signals[s_sig_len++] = ',';
    s_signals[s_sig_len++] = '"';
    for (const char *p = line; *p; ++p) {             /* 最小转义 */
        if (*p == '"' || *p == '\\') s_signals[s_sig_len++] = '\\';
        s_signals[s_sig_len++] = *p;
    }
    s_signals[s_sig_len++] = '"';
    s_signals[s_sig_len] = 0;
}
void console_send_evt(const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    signals_push(buf);
}
static char s_signals_out[SIG_CAP + 2];
const char *shim_drain_signals_json(void) {
    snprintf(s_signals_out, sizeof(s_signals_out), "[%s]", s_signals);
    s_sig_len = 0; s_signals[0] = 0;
    return s_signals_out;
}

/* ── 命令注册表 ─────────────────────────────────────────── */
static const console_cmd_t *s_cmds[CONSOLE_MAX_COMMANDS];
static int s_cmd_count = 0;
void console_protocol_register(const console_cmd_t *cmd) {
    if (s_cmd_count < CONSOLE_MAX_COMMANDS) s_cmds[s_cmd_count++] = cmd;
}
const console_cmd_t *shim_find_cmd(const char *name) {
    for (int i = 0; i < s_cmd_count; ++i)
        if (strcmp(s_cmds[i]->name, name) == 0) return s_cmds[i];
    return NULL;
}

/* ── scene 桩 ───────────────────────────────────────────── */
static scene_t s_scenes[] = {
    {"idle"}, {"dashboard"}, {"sessions"}, {"prompt"},
    {"tokens"}, {"status"}, {"awaiting"},
};
static const int s_scene_count = (int)(sizeof(s_scenes) / sizeof(s_scenes[0]));
static int s_cur_scene = -1;
int scene_fw_find_by_id(const char *id) {
    for (int i = 0; i < s_scene_count; ++i)
        if (strcmp(s_scenes[i].id, id) == 0) return i;
    return -1;
}
void scene_fw_show(int idx) {
    if (idx < 0 || idx >= s_scene_count) return;
    s_cur_scene = idx;
    char buf[48]; snprintf(buf, sizeof(buf), "scene_changed id=%s", s_scenes[idx].id);
    signals_push(buf);
}
const scene_t *scene_fw_current(void) {
    return (s_cur_scene < 0) ? NULL : &s_scenes[s_cur_scene];
}
const char *shim_current_scene_id(void) {
    return (s_cur_scene < 0) ? "" : s_scenes[s_cur_scene].id;
}
```

- [ ] **Step 5: 创建最小 `wasm_api.c`(仅 `dash_init`)**

`tools/web/wasm/wasm_api.c`:
```c
/* wasm_api.c — JS/ctypes ↔ 数据层的契约。第 1 步先建最小入口;
 * state_json / dash_feed_line / drain_signals 在后续任务补全。 */
#include "agent_state.h"
#include "agent_commands.h"

void dash_init(void) {
    agent_state_init();
    agent_commands_register();     /* 经 shim 捕获命令表 */
    agent_commands_load_config();  /* 设默认 device_name="DASHBOARD" 等 */
}
```

- [ ] **Step 6: 创建 `build_native.sh`(跨平台探测编译器与产物名)**

`tools/web/wasm/build_native.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"   # 仓库根
OUT="$HERE/build"
mkdir -p "$OUT"

# 选编译器
CC="${CC:-}"
if [ -z "$CC" ]; then
  for c in cc gcc clang; do command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }; done
fi
[ -n "$CC" ] || { echo "ERROR: no C compiler (cc/gcc/clang). Install one (MinGW/clang on Windows)." >&2; exit 1; }

# 选产物扩展名
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXT="dll" ;;
  Darwin)               EXT="dylib" ;;
  *)                    EXT="so" ;;
esac
LIB="$OUT/libdash_datalayer.$EXT"

"$CC" -shared -fPIC -std=c11 -g -O0 \
  -I "$HERE/shim_include" \
  -I "$ROOT/main" \
  -I "$ROOT/main/harness" \
  "$ROOT/main/agent_state.c" \
  "$ROOT/main/tiny_json.c" \
  "$ROOT/main/harness/agent_snapshot_apply.c" \
  "$ROOT/main/harness/agent_commands.c" \
  "$HERE/wasm_shim.c" \
  "$HERE/wasm_api.c" \
  -o "$LIB"

echo "built $LIB"
```

- [ ] **Step 7: 追加 `.gitignore`**

在 `.gitignore` 末尾追加:
```
tools/web/wasm/build/
```

- [ ] **Step 8: 运行构建,验证编译链接通过**

Run:
```bash
chmod +x tools/web/wasm/build_native.sh && bash tools/web/wasm/build_native.sh
```
Expected: 打印 `built .../build/libdash_datalayer.<ext>`,退出码 0,无编译/链接错误。
若报"undefined reference":说明数据层用到的某符号未在 shim 覆盖——按报错符号名补到 `wasm_shim.c`,不得改 `main/`。

- [ ] **Step 9: Commit**

```bash
git add tools/web/wasm/shim_include tools/web/wasm/wasm_shim.c tools/web/wasm/wasm_api.c tools/web/wasm/build_native.sh .gitignore
git commit -m "feat(web/wasm): shim + native build of firmware data layer compiles"
```

---

### Task 2: `dash_init` + `state_json`,Python 经 ctypes 断言空状态

**Files:**
- Modify: `tools/web/wasm/wasm_api.c`
- Create: `tools/web/test_wasm_datalayer.py`

**Interfaces:**
- Consumes: `dash_init()`(Task 1)、shim 的 `shim_current_scene_id()`。
- Produces:
  - `const char *state_json(void)` — 把 `agent_state` 关键字段序列化为 JSON(指向静态缓冲)。第 1 步覆盖核心字段:`scene` / `device_name` / `owner` / `totals{total,running,waiting,tokens,tokens_today}` / `prompt{active,id}` / `slots[]{kind,session_id,status,msg,cwd,tokens,tokens_today,awaiting}`。(渲染所需的 entries/spark/awaiting options 等在后续步骤补;此处为可行性验证的最小集,非占位。)
  - `tools/web/test_wasm_datalayer.py` 的 `load_lib()` helper(后续任务复用):定位并 `ctypes.CDLL` 加载产物、声明 `restype`。

- [ ] **Step 1: 写失败测试(加载库 + 空状态断言)**

`tools/web/test_wasm_datalayer.py`:
```python
"""一致性测试:宿主原生编译的固件数据层,经 ctypes 喂 dash 命令、读 state_json。
不需要 emsdk / pyserial。运行:python tools/web/test_wasm_datalayer.py"""
import ctypes
import json
import platform
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WASM = HERE / "wasm"
BUILD = WASM / "build"


def _lib_path() -> Path:
    ext = {"Windows": "dll", "Darwin": "dylib"}.get(platform.system(), "so")
    return BUILD / f"libdash_datalayer.{ext}"


def load_lib() -> ctypes.CDLL:
    lib_path = _lib_path()
    if not lib_path.exists():
        subprocess.run(["bash", str(WASM / "build_native.sh")], check=True)
    lib = ctypes.CDLL(str(lib_path))
    lib.dash_init.restype = None
    lib.state_json.restype = ctypes.c_char_p
    return lib


def state(lib) -> dict:
    return json.loads(lib.state_json().decode("utf-8"))


def test_empty_state():
    lib = load_lib()
    lib.dash_init()
    s = state(lib)
    assert s["device_name"] == "DASHBOARD", s
    assert s["totals"]["total"] == 0, s
    assert s["slots"] == [], s
    assert s["prompt"]["active"] is False, s
    print("ok test_empty_state")


if __name__ == "__main__":
    test_empty_state()
    print("ALL PASS")
```

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/web/test_wasm_datalayer.py`
Expected: FAIL —— `AttributeError: ... 'state_json'`(库尚未导出该符号)。

- [ ] **Step 3: 实现 `state_json`**

在 `tools/web/wasm/wasm_api.c` 增加(顶部补 `#include <stdio.h>`、`#include <string.h>`、`#include "agent_snapshot_apply.h"` 暂不需要;并 `extern const char *shim_current_scene_id(void);`):
```c
#include <stdio.h>
#include <string.h>

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
```

- [ ] **Step 4: 重新构建并运行测试**

Run: `bash tools/web/wasm/build_native.sh && python tools/web/test_wasm_datalayer.py`
Expected: PASS —— 打印 `ok test_empty_state` 和 `ALL PASS`。

- [ ] **Step 5: Commit**

```bash
git add tools/web/wasm/wasm_api.c tools/web/test_wasm_datalayer.py
git commit -m "feat(web/wasm): state_json export + empty-state ctypes consistency test"
```

---

### Task 3: `dash_feed_line`(G-7 tokenise + 分发 + reply 捕获)+ `dash idle`

**Files:**
- Modify: `tools/web/wasm/wasm_api.c`
- Modify: `tools/web/test_wasm_datalayer.py`

**Interfaces:**
- Consumes: shim 的 `shim_find_cmd()`、`shim_last_reply()`、`shim_last_reply_is_err()`、`shim_current_scene_id()`;`console_args_t`(来自 `harness/console_protocol.h`)。
- Produces:
  - `int dash_feed_line(const char *line)` — 用 G-7 tokeniser 切分,组 `console_args_t`,查 `dash` 命令并调用;返回 `0` 成功分发、`-1` 未识别。
  - `const char *last_reply(void)`、`int last_reply_is_err(void)`、`const char *current_scene(void)` 导出。

- [ ] **Step 1: 写失败测试(`dash idle` → scene/idle)**

在 `tools/web/test_wasm_datalayer.py` 增加:
```python
def _decl_feed(lib):
    lib.dash_feed_line.argtypes = [ctypes.c_char_p]
    lib.dash_feed_line.restype = ctypes.c_int
    lib.last_reply.restype = ctypes.c_char_p
    lib.current_scene.restype = ctypes.c_char_p

def feed(lib, line: str) -> int:
    return lib.dash_feed_line(line.encode("utf-8"))

def test_dash_idle():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    rc = feed(lib, 'dash idle')
    assert rc == 0, rc
    assert lib.current_scene().decode() == "idle", lib.current_scene()
    assert b'"scene":"idle"' in lib.last_reply(), lib.last_reply()
    print("ok test_dash_idle")
```
并在 `__main__` 末尾(`ALL PASS` 前)加 `test_dash_idle()`。

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/web/test_wasm_datalayer.py`
Expected: FAIL —— `AttributeError: ... 'dash_feed_line'`。

- [ ] **Step 3: 实现 `dash_feed_line` + G-7 tokeniser**

在 `tools/web/wasm/wasm_api.c` 增加(顶部补 `#include "harness/console_protocol.h"`、`extern` shim 接口):
```c
#include "harness/console_protocol.h"

extern const char *shim_last_reply(void);
extern int         shim_last_reply_is_err(void);
extern const console_cmd_t *shim_find_cmd(const char *name);
extern const char *shim_current_scene_id(void);

const char *last_reply(void)     { return shim_last_reply(); }
int         last_reply_is_err(void) { return shim_last_reply_is_err(); }
const char *current_scene(void)  { return shim_current_scene_id(); }

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
```

- [ ] **Step 4: 声明导出并重新构建运行**

确认 `test_wasm_datalayer.py::load_lib` 末尾无需改;新增符号在测试里通过 `_decl_feed` 声明。
Run: `bash tools/web/wasm/build_native.sh && python tools/web/test_wasm_datalayer.py`
Expected: PASS —— 含 `ok test_dash_idle`。

- [ ] **Step 5: Commit**

```bash
git add tools/web/wasm/wasm_api.c tools/web/test_wasm_datalayer.py
git commit -m "feat(web/wasm): dash_feed_line G-7 tokeniser + dispatch; dash idle test"
```

---

### Task 4: snapshot 一致性(真实 `agent_snapshot_apply.c` + `tiny_json.c` 跑通)

**Files:**
- Modify: `tools/web/test_wasm_datalayer.py`

**Interfaces:**
- Consumes: `dash_feed_line`、`state_json`(已具备)。无新导出。

- [ ] **Step 1: 写测试 —— 喂双 agent v1 snapshot,断言 state_json**

在 `tools/web/test_wasm_datalayer.py` 增加(使用 `PROTOCOL.md` 的 v1 形状):
```python
def test_snapshot_two_agents():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    snap = (
        '{"agents":['
        '{"kind":"claude-code","session_id":"cc_abc","status":"running",'
        '"cwd":"D:\\\\Code\\\\x","msg":"editing main.c","tokens":84502,"tokens_today":21200},'
        '{"kind":"codex","session_id":"cx_xyz","status":"idle",'
        '"cwd":"D:\\\\Code\\\\y","msg":"(stop)","tokens":12300,"tokens_today":12300}'
        '],"totals":{"total":2,"running":1,"waiting":0,"tokens":96802,"tokens_today":33500}}'
    )
    rc = feed(lib, 'dash snapshot "' + snap + '"')
    assert rc == 0, rc
    s = state(lib)
    assert s["totals"]["total"] == 2, s
    assert len(s["slots"]) == 2, s
    kinds = {sl["kind"] for sl in s["slots"]}
    assert kinds == {"claude-code", "codex"}, kinds
    cc = next(sl for sl in s["slots"] if sl["kind"] == "claude-code")
    assert cc["session_id"] == "cc_abc", cc
    assert cc["status"] == "running", cc
    assert cc["msg"] == "editing main.c", cc
    assert cc["tokens"] == 84502, cc
    print("ok test_snapshot_two_agents")
```
在 `__main__` 加 `test_snapshot_two_agents()`。

- [ ] **Step 2: 运行测试**

Run: `python tools/web/test_wasm_datalayer.py`
Expected: PASS —— 含 `ok test_snapshot_two_agents`。
说明:本任务不写实现;若失败,说明数据层经 shim 行为与预期不符,定位到 `agent_snapshot_apply.c`/`tiny_json.c`/`state_json` 序列化,不改 `main/`(必要时只改 `state_json`/shim)。

- [ ] **Step 3: Commit**

```bash
git add tools/web/test_wasm_datalayer.py
git commit -m "test(web/wasm): v1 two-agent snapshot consistency"
```

---

### Task 5: 边界行为锁(同 bug 护栏 —— msg 截断 + slot 溢出)

**Files:**
- Modify: `tools/web/test_wasm_datalayer.py`

**Interfaces:**
- Consumes: `dash_feed_line`、`state_json`。无新导出。

- [ ] **Step 1: 写测试 —— 超长 msg 截断到 `AGENT_MSG_MAX-1`**

在 `tools/web/test_wasm_datalayer.py` 增加:
```python
AGENT_MSG_MAX = 128          # verbatim from main/agent_state.h
AGENT_SLOT_MAX = 4           # verbatim from main/agent_state.h

def test_msg_truncation():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    long_msg = "x" * 300
    snap = ('{"agents":[{"kind":"claude-code","session_id":"cc1",'
            '"status":"running","msg":"' + long_msg + '"}],'
            '"totals":{"total":1,"running":1,"waiting":0}}')
    feed(lib, 'dash snapshot "' + snap + '"')
    s = state(lib)
    msg = s["slots"][0]["msg"]
    assert len(msg) == AGENT_MSG_MAX - 1, (len(msg), AGENT_MSG_MAX)
    assert set(msg) == {"x"}, "truncated content should be all x"
    print("ok test_msg_truncation")
```
在 `__main__` 加 `test_msg_truncation()`。

- [ ] **Step 2: 运行(确认与固件截断语义一致)**

Run: `python tools/web/test_wasm_datalayer.py`
Expected: PASS —— `ok test_msg_truncation`。
若 `len(msg)` 不等于 127:这正是"web 与固件同 bug/同语义"的探针;核对 `agent_state.h` 的 `AGENT_MSG_MAX` 与写入路径,更新断言为真实 verbatim 值(不得改 `main/`)。

- [ ] **Step 3: 写测试 —— 5 个 agent 触发 slot 溢出(prune 到 4)**

在 `tools/web/test_wasm_datalayer.py` 增加:
```python
def test_slot_overflow():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    agents = ",".join(
        '{"kind":"other","session_id":"s%d","status":"running","msg":"m%d"}' % (i, i)
        for i in range(5)
    )
    snap = '{"agents":[' + agents + '],"totals":{"total":5,"running":5,"waiting":0}}'
    rc = feed(lib, 'dash snapshot "' + snap + '"')
    assert rc == 0, rc
    s = state(lib)
    assert len(s["slots"]) == AGENT_SLOT_MAX, len(s["slots"])
    assert b'"dropped":1' in lib.last_reply(), lib.last_reply()
    print("ok test_slot_overflow")
```
在 `__main__` 加 `test_slot_overflow()`。

- [ ] **Step 4: 运行测试**

Run: `python tools/web/test_wasm_datalayer.py`
Expected: PASS —— `ok test_slot_overflow`。
说明:`cmd_snapshot` 的 reply 形如 `{"applied":true,"agents":4,"dropped":1}`;断言 `dropped` 与 `AGENT_SLOT_MAX` 一致。若 reply 中 dropped 数不同,核对 `agent_snapshot_apply.c` 的 `dropped_count` 语义并据实更新断言。

- [ ] **Step 5: Commit**

```bash
git add tools/web/test_wasm_datalayer.py
git commit -m "test(web/wasm): boundary parity — msg truncation + slot overflow prune"
```

---

### Task 6: `drain_signals`(EVT / scene 信号捕获)

**Files:**
- Modify: `tools/web/wasm/wasm_api.c`
- Modify: `tools/web/test_wasm_datalayer.py`

**Interfaces:**
- Consumes: shim 的 `shim_drain_signals_json()`。
- Produces: `const char *drain_signals(void)` 导出(返回 JSON 数组字符串,取走即清空)。

- [ ] **Step 1: 写失败测试 —— snapshot 新增 agent 产 `agent_added`,prompt 切场景**

在 `tools/web/test_wasm_datalayer.py` 增加:
```python
def test_signals():
    lib = load_lib()
    _decl_feed(lib)
    lib.drain_signals.restype = ctypes.c_char_p
    lib.dash_init()
    snap = ('{"agents":[{"kind":"codex","session_id":"cx1",'
            '"status":"running","msg":"go"}],'
            '"totals":{"total":1,"running":1,"waiting":0}}')
    feed(lib, 'dash snapshot "' + snap + '"')
    sigs = json.loads(lib.drain_signals().decode())
    assert any("agent_added" in x and "codex" in x for x in sigs), sigs
    # drain 清空
    assert json.loads(lib.drain_signals().decode()) == [], "signals should clear after drain"
    prompt = '{"id":"req1","tool":"Bash"}'
    feed(lib, 'dash prompt "' + prompt + '"')
    sigs = json.loads(lib.drain_signals().decode())
    assert any("scene_changed" in x and "prompt" in x for x in sigs), sigs
    print("ok test_signals")
```
在 `__main__` 加 `test_signals()`。

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/web/test_wasm_datalayer.py`
Expected: FAIL —— `AttributeError: ... 'drain_signals'`。

- [ ] **Step 3: 实现 `drain_signals` 导出**

在 `tools/web/wasm/wasm_api.c` 增加:
```c
extern const char *shim_drain_signals_json(void);
const char *drain_signals(void) { return shim_drain_signals_json(); }
```

- [ ] **Step 4: 重新构建并运行**

Run: `bash tools/web/wasm/build_native.sh && python tools/web/test_wasm_datalayer.py`
Expected: PASS —— 含 `ok test_signals` 与 `ALL PASS`。

- [ ] **Step 5: Commit**

```bash
git add tools/web/wasm/wasm_api.c tools/web/test_wasm_datalayer.py
git commit -m "feat(web/wasm): drain_signals export + EVT/scene signal test"
```

---

## 验收(第 1 步可行性闸门)

全部 6 个任务完成后:
- `bash tools/web/wasm/build_native.sh` 在本机(Windows)与 Linux 均能产出动态库。
- `python tools/web/test_wasm_datalayer.py` 全绿:空状态、`dash idle`、双 agent snapshot、msg 截断、slot 溢出、信号捕获。
- **结论判定**:若全绿 → §12 风险 1/2 证伪,WASM 同源方案可行,继续 spec §13 第 2 步(emcc 出浏览器 `.wasm` + 最小 `app.js`)。若 `agent_commands.c` 分发无法干净复用,或边界行为无法与固件对齐 → 停下,带着具体报错回到设计讨论(对应 spec §12 风险 2 的"退化/重议"分支)。

## 后续步骤(不在本计划内,供衔接)

spec §13 第 2–6 步:emcc 出 `.wasm`;`DevicePusher` fan-out;`WebDeviceSink` + `serve.py`(SSE/POST)+ `cmd_serve --web`;前端渲染 + dev 面板 + 按键/注入;文档更新。每步另起计划。
