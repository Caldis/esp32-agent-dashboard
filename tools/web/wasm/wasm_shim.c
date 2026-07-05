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
#include "theme.h"            /* 经 -I main 及 -I main/harness;拉替身 lvgl.h */
#include "button_router.h"    /* 经 -I main;dash btn 桩需要签名 */
#include "harness/console_protocol.h"
#include "harness/scene_framework.h"
#include "scenes/scenes.h"      /* 校验 scene_prompt_note_origin/return_home 桩签名 */

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
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 1); if (!e) return ESP_FAIL;
    strncpy(e->sval, val, sizeof(e->sval) - 1); e->sval[sizeof(e->sval)-1] = 0;
    e->is_u8 = 0; s_nvs_dirty = 1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 0); if (!e || e->is_u8) return ESP_FAIL;
    size_t n = strlen(e->sval); if (len && *len <= n) return ESP_FAIL;
    strcpy(out, e->sval); if (len) *len = n + 1; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 1); if (!e) return ESP_FAIL;
    e->u8 = v; e->is_u8 = 1; s_nvs_dirty = 1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    (void)h; nvs_ent_t *e = nvs_slot(key, 0); if (!e || !e->is_u8) return ESP_FAIL;
    *out = e->u8; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void      nvs_close(nvs_handle_t h) { (void)h; }

/* ── theme 桩 ───────────────────────────────────────────── */
static char s_theme[16] = "noir";
bool theme_set_by_name(const char *name) {
    if (!name) return false;
    if (strcmp(name, "noir") && strcmp(name, "lab") && strcmp(name, "mono")) return false;
    strncpy(s_theme, name, sizeof(s_theme) - 1); s_theme[sizeof(s_theme)-1] = 0;
    return true;
}
const char *theme_current_name(void) { return s_theme; }

/* ── button router 桩 → 信号队列(dash btn 是渲染/路由,数据层只发信号) ── */
static void signals_push(const char *line);  /* fwd */
void button_router_press(button_router_key_t key) {
    const char *name = (key == ROUTER_KEY_BOOT) ? "boot"
                     : (key == ROUTER_KEY_USER) ? "user" : "pwr";
    char buf[32];
    snprintf(buf, sizeof(buf), "btn key=%s", name);
    signals_push(buf);
}
bool button_router_screen_is_off(void) { return false; }
void button_router_screen_wake(void) {}

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
    size_t worst = strlen(line) * 2 + 4;   /* 每字符最多转义 2 字节 + 引号/逗号 */
    if (s_sig_len + worst >= SIG_CAP) return;          /* 满了静默丢弃(测试不应触达) */
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
/* 与固件 app_main 注册顺序一致(v4):index 0 = dashboard(默认/回退)。 */
static scene_t s_scenes[] = {
    {"dashboard"}, {"idle"}, {"clock"}, {"prompt"}, {"awaiting"},
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

/* v4 prompt 起源记录/恢复 — 语义镜像 main/scenes/scene_prompt.c,
 * 让数据层测试能覆盖「prompt 退出恢复先前场景」契约。 */
static int s_pre_prompt_scene = -1;
void scene_prompt_note_origin(void) {
    if (s_cur_scene < 0) return;
    const char *id = s_scenes[s_cur_scene].id;
    if (strcmp(id, "prompt") == 0 || strcmp(id, "awaiting") == 0) return;
    s_pre_prompt_scene = s_cur_scene;
}
void scene_prompt_return_home(void) {
    /* 幂等守卫与固件一致:不在 prompt 上时不动作。 */
    if (s_cur_scene < 0 || strcmp(s_scenes[s_cur_scene].id, "prompt") != 0) return;
    int idx = s_pre_prompt_scene;
    s_pre_prompt_scene = -1;
    if (idx < 0 || idx >= s_scene_count) idx = 0;
    scene_fw_show(idx);
}
