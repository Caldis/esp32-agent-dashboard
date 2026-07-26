/*
 * agent_commands — single `dash` console command with subcommand dispatch.
 *
 *   dash snapshot <json>   — full state push (v1: {agents:[...], totals:{...}}
 *                            also accepts v0 flat shape)
 *   dash prompt   <json>   — set prompt_active + switch scene
 *   dash event    <json>   — one-shot transcript line (per agent)
 *   dash tokens   <json>   — per-agent token counters + sparkline sample
 *   dash idle              — switch back to scene_idle (legacy alias)
 *   dash scene    <id>     — switch to any scene by id (host remote / tests)
 *   dash config   <json>   — set device_name / owner / theme / default_scene
 *                            (persisted to NVS namespace "dashcfg")
 *   dash time     <json>   — set epoch_unix + tz_offset_seconds
 *   dash health            — replies HEALTH payload block with device internals
 *   dash btn <boot|user|pwr> — simulate a physical key press through the
 *                            button router (same code path as real keys)
 *
 * (dash push — the v2.7.0 per-tool banner — was removed in v3.1: it
 * flashed Read/Edit noise the fleet activity line already carries.)
 *
 * v4 scene contract: environment scenes (dashboard / overview / clock)
 * switch ONLY by explicit request — the physical BOOT key or a host
 * `dash idle` / `dash scene`. Snapshots never yank the view around
 * (the v3 idle↔dashboard auto-switch is gone). The two takeovers stay
 * state-driven: prompt_set switches in (remembering the covered scene,
 * see scene_prompt_note_origin), prompt_clear restores it; awaiting is
 * handled by scene_auto_switch_cb in esp32_agent_dashboard_main.c.
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
#include "agent_snapshot_apply.h"
#include "../agent_state.h"
#include "../cjk_font.h"
#include "../theme.h"
#include "../tiny_json.h"
#include "../button_router.h"
#include "../scenes/scenes.h"
#include "../scene_trans.h"

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
    scene_trans_switch(idx);
    bsp_display_unlock();
    return true;
}

/* (v5.2: switch_to_prompt removed — the on-device prompt takeover is
 * retired; approvals happen in the terminal.) */

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

/* Cheap digest of the agent-visible state. Compared across a snapshot
 * apply to tell "something actually happened" (agents came/went, status
 * or awaiting flipped, transcript advanced, tokens moved) from the
 * bridge's 10s keepalive re-pushing identical state — only the former
 * counts as activity for the clock screensaver. Lock held. */
static uint32_t state_fingerprint_locked(void)
{
    const agent_state_t *s = agent_state_get();
    uint32_t h = (uint32_t)(s->slot_count * 131 + s->running * 31
                            + s->waiting * 17)
               + (s->prompt_active ? 7u : 0u);
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!s->slots[i].in_use) continue;
        h = h * 33 + s->slots[i].entry_seq;
        h = h * 33 + (uint32_t)s->slots[i].status;
        h = h * 33 + (uint32_t)s->slots[i].awaiting_kind;
        h = h * 33 + (uint32_t)s->slots[i].tokens_today;
    }
    return h;
}

static int cmd_snapshot(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    agent_state_lock();
    uint32_t fp_before = state_fingerprint_locked();
    agent_state_unlock();

    agent_snapshot_apply_result_t result;
    if (!agent_snapshot_apply_json(json, end, &result)) {
        console_reply_err("snapshot apply failed");
        return 0;
    }

    agent_state_lock();
    bool changed = (state_fingerprint_locked() != fp_before);
    agent_state_unlock();
    if (changed) agent_state_touch_activity();

    for (int i = 0; i < result.removed_count; ++i) {
        console_send_evt("agent_removed kind=%s session_id=%s",
                         result.removed_kind[i], result.removed_sid[i]);
    }
    for (int i = 0; i < result.added_count; ++i) {
        console_send_evt("agent_added kind=%s session_id=%s",
                         result.added_kind[i], result.added_sid[i]);
    }
    if (result.dropped_count > 0) {
        console_send_evt("agent_dropped count=%d reason=slot_full",
                         result.dropped_count);
    }

    /* v5.2: snapshots drive NO scene changes at all any more — the
     * prompt takeover is retired (approvals happen in the terminal) and
     * environment scenes were already manual since v4. */

    console_reply_ok("{\"applied\":true,\"agents\":%d,\"dropped\":%d}",
                     result.total_now, result.dropped_count);
    return 0;
}

/* ── dash prompt ─────────────────────────────────────────────────── */

/* v5.2: the on-device prompt takeover is retired — approvals happen in
 * the terminal. The verb stays a wire-compatible no-op (the bridge may
 * still push prompts; erroring would just spam its logs) and keeps the
 * health counter ticking. prompt_active must never go true (see the
 * matching note in agent_snapshot_apply.c). */
static int cmd_prompt(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    char id[AGENT_PROMPT_ID_MAX] = {0};
    tj_object_get_string(json, end, "id", id, sizeof(id));
    if (id[0] == '\0') { console_reply_err("prompt id required"); return 0; }

    agent_state_lock();
    agent_state_get()->prompts_received++;
    agent_state_unlock();

    console_reply_ok("{\"prompt\":\"%s\",\"takeover\":\"retired\"}", id);
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

    agent_state_touch_activity();
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
    /* v5.2: the overview scene (wire id "idle") is retired; keep the
     * verb as a dashboard alias so old tooling doesn't start erroring. */
    if (!switch_scene("idle") && !switch_scene("dashboard")) {
        console_reply_err("idle scene missing");
        return 0;
    }
    console_reply_ok("{\"scene\":\"idle\"}");
    return 0;
}

/* ── dash scene ──────────────────────────────────────────────────── */

/* Generic host-side scene switch (`dash scene clock`). argv[2] is the
 * scene id, not JSON. Counts as a manual switch under the v4 contract —
 * used by tests and remote control, same standing as a BOOT press. */
static int cmd_scene(const console_args_t *a)
{
    if (a->argc < 3 || a->argv[2] == NULL || a->argv[2][0] == '\0') {
        console_reply_err("scene needs an id");
        return 0;
    }
    if (!switch_scene(a->argv[2])) {
        console_reply_err("unknown scene: %s", a->argv[2]);
        return 0;
    }
    console_reply_ok("{\"scene\":\"%s\"}", a->argv[2]);
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
    uint8_t sv = 0;
    if (nvs_get_u8(h, "saver_min", &sv) == ESP_OK) {
        s->screensaver_min = sv;
    }
    uint8_t om = 0;
    if (nvs_get_u8(h, "offline_min", &om) == ESP_OK) {
        s->offline_clock_min = om;
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
    double saver_min = 0;
    double offline_min = 0;
    bool has_dev   = tj_object_get_string(json, end, "device_name",     device_name,   sizeof(device_name));
    bool has_own   = tj_object_get_string(json, end, "owner",           owner,         sizeof(owner));
    bool has_theme = tj_object_get_string(json, end, "theme",           theme_buf,     sizeof(theme_buf));
    bool has_def   = tj_object_get_string(json, end, "default_scene",   default_scene, sizeof(default_scene));
    bool has_mr    = tj_object_get_string(json, end, "motion_reduced",  motion_buf,    sizeof(motion_buf));
    bool has_saver = tj_object_get_double(json, end, "screensaver_min", &saver_min);
    bool has_offl  = tj_object_get_double(json, end, "offline_clock_min", &offline_min);
    bool motion_reduced_value = false;
    /* minutes, clamped to the u8 NVS slot; 0 disables the screensaver */
    if (has_saver) {
        if (saver_min < 0)   saver_min = 0;
        if (saver_min > 255) saver_min = 255;
    }
    /* minutes, same u8 clamp; 0 disables the offline→clock fallback */
    if (has_offl) {
        if (offline_min < 0)   offline_min = 0;
        if (offline_min > 255) offline_min = 255;
    }

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    if (has_dev) { strncpy(s->device_name,   device_name,   sizeof(s->device_name) - 1);   s->device_name[sizeof(s->device_name)-1]='\0'; }
    if (has_own) { strncpy(s->owner,         owner,         sizeof(s->owner) - 1);         s->owner[sizeof(s->owner)-1]='\0'; }
    if (has_def) { strncpy(s->default_scene, default_scene, sizeof(s->default_scene) - 1); s->default_scene[sizeof(s->default_scene)-1]='\0'; }
    if (has_mr)  {
        motion_reduced_value = (strcmp(motion_buf, "true") == 0 || strcmp(motion_buf, "1") == 0);
        s->motion_reduced = motion_reduced_value;
    }
    if (has_saver) s->screensaver_min = (int32_t)saver_min;
    if (has_offl)  s->offline_clock_min = (int32_t)offline_min;
    bool theme_ok = true;
    if (has_theme) theme_ok = theme_set_by_name(theme_buf);
    agent_state_unlock();

    if (has_dev)   persist_string("device_name",   device_name);
    if (has_own)   persist_string("owner",         owner);
    if (has_theme && theme_ok) persist_string("theme", theme_buf);
    if (has_def)   persist_string("default_scene", default_scene);
    if (has_mr)    persist_u8("motion_red", motion_reduced_value ? 1 : 0);
    if (has_saver) persist_u8("saver_min", (uint8_t)saver_min);
    if (has_offl)  persist_u8("offline_min", (uint8_t)offline_min);

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

/* ── dash weather ────────────────────────────────────────────────── */

/* v4.9: weather push from the bridge. Compact shape (wire budget):
 *   {"loc":"深圳·福田","t":31,"c":95,
 *    "days":[{"w":4,"c":80,"lo":26,"hi":33}, ×5]}
 * days[] is the fixed five-day window (yesterday, today, +3); anything
 * other than exactly WEATHER_DAYS entries is rejected so the scene never
 * renders a half-filled strip. Weather is background data — deliberately
 * NOT an activity touch (it must not hold the screensaver open). */
static int cmd_weather(const console_args_t *a)
{
    const char *end = NULL;
    const char *json = json_arg(a, &end);
    if (!json) return 0;

    char loc[WEATHER_LOC_MAX] = {0};
    double cur_t = 0, cur_c = 0;
    bool has_t = tj_object_get_double(json, end, "t", &cur_t);
    bool has_c = tj_object_get_double(json, end, "c", &cur_c);
    tj_object_get_string(json, end, "loc", loc, sizeof(loc));

    weather_day_t days[WEATHER_DAYS];
    int n = 0;
    tj_span_t arr;
    if (tj_object_find(json, end, "days", &arr) && tj_value_is_array(arr)) {
        const char *cursor = NULL;
        tj_span_t it;
        while (n < WEATHER_DAYS && tj_array_next(arr, cursor, &it)) {
            cursor = it.end;
            if (!tj_value_is_object(it)) continue;
            double w = 0, dc = 0, lo = 0, hi = 0;
            tj_object_get_double(it.begin, it.end, "w",  &w);
            tj_object_get_double(it.begin, it.end, "c",  &dc);
            tj_object_get_double(it.begin, it.end, "lo", &lo);
            tj_object_get_double(it.begin, it.end, "hi", &hi);
            days[n].wday = (uint8_t)(((int)w % 7 + 7) % 7);
            days[n].code = (int16_t)dc;
            days[n].t_lo = (int8_t)lo;
            days[n].t_hi = (int8_t)hi;
            n++;
        }
    }
    if (!has_t || !has_c || n != WEATHER_DAYS) {
        console_reply_err("weather needs t,c,days[%d] (got %d)",
                          WEATHER_DAYS, n);
        return 0;
    }

    agent_state_lock();
    weather_state_t *w = &agent_state_get()->weather;
    if (loc[0]) cjk_utf8_lcpy(w->loc, loc, sizeof(w->loc));
    w->cur_temp = (int16_t)cur_t;
    w->cur_code = (int16_t)cur_c;
    memcpy(w->days, days, sizeof(days));
    w->valid = true;
    w->received_ms = lv_tick_get();
    agent_state_unlock();

    console_reply_ok("{\"weather\":\"applied\",\"days\":%d}", n);
    return 0;
}

/* ── dash health ─────────────────────────────────────────────────── */

static int cmd_health(const console_args_t *a)
{
    (void)a;
    char buf[640];
    char dn[AGENT_DEVICE_NAME_MAX], own[AGENT_OWNER_MAX];
    int agent_count;
    uint32_t snaps, dropped, prompts, decisions;
    uint32_t last_snap_ms;
    bool ever;

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    strncpy(dn,  s->device_name, sizeof(dn));  dn[sizeof(dn)-1]   = '\0';
    strncpy(own, s->owner,       sizeof(own)); own[sizeof(own)-1] = '\0';
    agent_count = s->slot_count;
    snaps = s->snapshots_received;
    dropped = s->snapshot_dropped_agents;
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
        "\"snapshot_dropped_agents\":%u,\"decisions_sent\":%u,"
        "\"connection_age_s\":%u,\"agent_count\":%d}",
        dn, own, scene_id, theme_current_name(),
        (unsigned)up_s, (unsigned)heap_free, (unsigned)heap_min,
        (unsigned)snaps, (unsigned)prompts, (unsigned)dropped,
        (unsigned)decisions, (unsigned)conn_age_s, agent_count);

    if (n < 0) { console_reply_err("snprintf"); return 0; }

    char meta[64];
    snprintf(meta, sizeof(meta), "fmt=json bytes=%d", n);
    console_reply_ok("payload follows tag=HEALTH");
    console_begin_payload("HEALTH", meta);
    console_write_raw(buf, (size_t)n);
    console_end_payload("HEALTH");
    return 0;
}

/* ── dash btn ───────────────────────────────────────────────────── */

static int cmd_btn(const console_args_t *a)
{
    if (a->argc < 3 || a->argv[2] == NULL) {
        console_reply_err("btn needs boot|user|pwr");
        return 0;
    }
    const char *k = a->argv[2];
    button_router_key_t key;
    if      (strcmp(k, "boot") == 0) key = ROUTER_KEY_BOOT;
    else if (strcmp(k, "user") == 0) key = ROUTER_KEY_USER;
    else if (strcmp(k, "pwr")  == 0) key = ROUTER_KEY_PWR;
    else {
        console_reply_err("unknown key: %s (boot|user|pwr)", k);
        return 0;
    }
    /* Console task is a FreeRTOS task like the button tasks; the router
     * takes the display lock where needed, so this is the SAME contract
     * as a physical press. */
    button_router_press(key);
    console_reply_ok("{\"btn\":\"%s\"}", k);
    return 0;
}

/* ── Single command + sub-dispatch ────────────────────────────────── */

static int cmd_dash(const console_args_t *a)
{
    if (a->argc < 2) {
        console_reply_err("dash needs a subcommand "
                          "(snapshot|prompt|event|tokens|idle|scene|config|time|weather|health|btn)");
        return 0;
    }
    const char *sub = a->argv[1];
    if      (strcmp(sub, "snapshot") == 0) return cmd_snapshot(a);
    else if (strcmp(sub, "prompt")   == 0) return cmd_prompt(a);
    else if (strcmp(sub, "event")    == 0) return cmd_event(a);
    else if (strcmp(sub, "tokens")   == 0) return cmd_tokens(a);
    else if (strcmp(sub, "idle")     == 0) return cmd_idle(a);
    else if (strcmp(sub, "scene")    == 0) return cmd_scene(a);
    else if (strcmp(sub, "config")   == 0) return cmd_config(a);
    else if (strcmp(sub, "time")     == 0) return cmd_time(a);
    else if (strcmp(sub, "weather")  == 0) return cmd_weather(a);
    else if (strcmp(sub, "health")   == 0) return cmd_health(a);
    else if (strcmp(sub, "btn")      == 0) return cmd_btn(a);
    console_reply_err("unknown dash subcommand: %s", sub);
    return 0;
}

static const console_cmd_t s_cmd_dash = {
    "dash",
    cmd_dash,
    "dash <snapshot|prompt|event|tokens|idle|scene|config|time|weather|health|btn> [json|id]"
};

void agent_commands_register(void)
{
    console_protocol_register(&s_cmd_dash);
    ESP_LOGI(TAG, "dash command registered");
}
