/*
 * esp32_agent_dashboard_main.c — host-driven AI agent dashboard.
 *
 * Boot order:
 *   1. NVS (RF calibration storage, settings)
 *   2. BSP display + LVGL
 *   3. agent_state_init + theme_init + load persisted config
 *   4. harness default commands + agent_commands_register (`dash` family)
 *   5. console_protocol_init — listen LAST, after every command exists
 *      (register-then-listen: see comment in app_main)
 *   6. scenes registered under LVGL lock
 *   7. buttons_init + pwr_key_init + button_router_init — three-key
 *      mode switching (BOOT=view, USER=focus, PWR=screen); an active
 *      prompt overrides to BOOT=approve / USER=deny
 *   8. heap watchdog timer (emits EVT: low_heap if free < 50 KB)
 *
 * After boot the device sits on the default scene (configurable via
 * `dash config '{"default_scene":"dashboard"}'`) until the host pushes
 * state via `dash snapshot ...`. See main/harness/agent_commands.c for
 * the full protocol.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "harness/console_protocol.h"
#include "harness/scene_framework.h"
#include "harness/default_cmds.h"

#include "scenes/scenes.h"
#include "scene_trans.h"
#include "perf_mon.h"
#include "agent_state.h"
#include "buttons.h"
#include "pwr_key.h"
#include "button_router.h"
#include "theme.h"
#include "harness/agent_commands.h"

static const char *TAG = "esp32_agent_dashboard";

/* Heap watchdog: emit EVT once if free heap drops below threshold.
 * Debounced — at most once per 60 s. */
#define HEAP_LOW_THRESHOLD     (50 * 1024)
#define HEAP_WD_PERIOD_MS      5000u
#define HEAP_EVT_DEBOUNCE_MS   60000u

static uint32_t s_last_heap_evt_ms = 0;

static void heap_watchdog_cb(lv_timer_t *t)
{
    (void)t;
    size_t free = esp_get_free_heap_size();
    if (free >= HEAP_LOW_THRESHOLD) return;
    uint32_t now = lv_tick_get();
    if (s_last_heap_evt_ms != 0 && (now - s_last_heap_evt_ms) < HEAP_EVT_DEBOUNCE_MS) {
        return;
    }
    s_last_heap_evt_ms = now;
    console_send_evt("low_heap free=%u", (unsigned)free);
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    harness_record_frame();
}

static void on_scene_changed(int idx, const scene_t *current)
{
    if (current) {
        ESP_LOGI(TAG, "scene_changed idx=%d id=%s", idx, current->id);
        console_send_evt("scene_changed idx=%d id=%s", idx, current->id);
    }
}

/* v6.0: the AWAITING takeover scene is retired (user call: it and the
 * dashboard's gold pose were two near-identical gold pages a key press
 * flipped between). "Takeover" is now a PULL: on the rising edge of
 * effective-awaiting (any slot awaiting AND the host link alive) the
 * display switches to the dashboard, whose gold pose carries the
 * greeting word + project chip. One-shot: if the user keys away
 * afterwards nothing re-grabs — only the next genuine turn-return (a
 * fresh rising edge) pulls again. This subsumes the v4.9 dismissal
 * flag AND the pre-takeover scene restore (there is no takeover to
 * restore from; when a round clears the dashboard just re-renders in
 * place). Edging on EFFECTIVE awaiting keeps the two old suppressions
 * for free: host_lost drops the signal (stale frozen slots can't
 * pull), and the reconnect force-push raises it again — the v4.7
 * "reconnect re-fires from the fresh snapshot" contract. */
static bool s_prev_eff_awaiting = false;

/* v4.2 clock screensaver: after screensaver_min minutes with no
 * activity (key press / dash prompt / dash event / a snapshot that
 * actually changed state), glide to the clock scene.
 *
 * v4.6: agent activity while saving NO LONGER yanks the user back to the
 * covered view. The clock now reacts to task start/end in place — a
 * transient push card (scene_clock's push subsystem) surfaces the event
 * for a few seconds, then the pure face returns. The saver clock only
 * leaves on a real interrupt: a key press (exits via cycle_view, which
 * skips clock) or an AWAITING takeover (handled above, higher priority).
 * Manual `dash scene clock` visits are NOT screensaver entries — no
 * flag, so nothing yanks the user away.
 *
 * v4.7: a second, faster road into the same saver state — when the host
 * link is lost for offline_clock_min minutes (see host_lost below), the
 * device retreats to the clock instead of parading stale agent data.
 * The status bar's red dot stays visible on the clock face as the quiet
 * disconnect hint. */
static bool     s_saver_active = false;
static int      s_pre_saver_scene_idx = -1;

int scene_saver_consume(void)
{
    bsp_display_lock(-1);
    int covered = -1;
    if (s_saver_active) {
        covered = s_pre_saver_scene_idx;
        s_saver_active = false;
        s_pre_saver_scene_idx = -1;
    }
    bsp_display_unlock();
    return covered;
}

void scene_auto_switch_cb(lv_timer_t *t)
{
    (void)t;
    int dash_idx = scene_fw_find_by_id("dashboard");
    int current_idx = scene_fw_current_index();
    if (dash_idx < 0 || current_idx < 0) return;

    bool any_awaiting = false;
    int32_t  saver_min = 0;
    int32_t  offline_min = 0;
    uint32_t last_activity_ms = 0;
    uint32_t last_snapshot_ms = 0;
    bool     ever_received = false;
    agent_state_lock();
    if (agent_state_most_recent_awaiting() != NULL) any_awaiting = true;
    saver_min = agent_state_get()->screensaver_min;
    offline_min = agent_state_get()->offline_clock_min;
    last_activity_ms = agent_state_get()->last_activity_ms;
    last_snapshot_ms = agent_state_get()->last_snapshot_ms;
    ever_received = agent_state_get()->ever_received;
    agent_state_unlock();

    /* v4.7 offline fallback: the bridge keepalives every 10 s, so a
     * snapshot stream silent for offline_min minutes means the host is
     * genuinely gone (status_bar's red dot fired long ago at 12 s).
     * Once lost, every agent slot is frozen history — treat the device
     * as idle and let the clock take over below. Requires ever_received:
     * a cold boot with no host yet keeps the configured default scene
     * (the clock would be a useless "--:--" without host time anyway). */
    uint32_t now = lv_tick_get();
    bool host_lost = ever_received && offline_min > 0
                  && (now - last_snapshot_ms) >= (uint32_t)offline_min * 60000u;

    /* (v4.3: screen-off is gone — PWR locks to the clock instead — so
     * the wake call is inert; kept for the day a real screen-off
     * returns. v5.2: the prompt takeover is retired, so awaiting is the
     * only interactive takeover left.) */
    if (any_awaiting && button_router_screen_is_off()) {
        button_router_screen_wake();
    }

    /* v6.0 pull-to-dashboard. Any agent count: the destination is the
     * dashboard itself, so the old v3.0 "suppress with 2+ agents" gate
     * (a full-screen takeover would hide the fleet) is moot — with 2+
     * live the pull lands on the fleet rows and the gold row carries
     * the state. */
    bool eff_awaiting = any_awaiting && !host_lost;
    bool pull = eff_awaiting && !s_prev_eff_awaiting;
    s_prev_eff_awaiting = eff_awaiting;
    if (pull && current_idx != dash_idx) {
        bsp_display_lock(-1);
        scene_trans_switch(dash_idx);
        bsp_display_unlock();
        return;
    }

    /* ── clock screensaver + offline fallback ──────────────────────── */
    int clock_idx = scene_fw_find_by_id("clock");
    if (clock_idx < 0) return;
    bool on_clock = (current_idx == clock_idx);

    /* Two roads into the saver clock, same machinery once inside:
     *  - idle:    saver_min of no activity while everything is healthy;
     *  - offline: host_lost AND offline_min of no activity. The second
     *    activity clause keeps a key press meaningful while offline —
     *    the user gets offline_min to poke at the (stale, red-dotted)
     *    views before the clock reclaims the screen. Unlike the idle
     *    road it ignores any_awaiting: those slots are frozen history. */
    if (s_saver_active && !on_clock) {
        /* A key press / takeover already moved us off the clock. */
        s_saver_active = false;
        s_pre_saver_scene_idx = -1;
    } else if (!s_saver_active && !on_clock
               && !button_router_screen_is_off()
               && ((!any_awaiting && saver_min > 0
                    && (now - last_activity_ms) >= (uint32_t)saver_min * 60000u)
                   || (host_lost
                    && (now - last_activity_ms) >= (uint32_t)offline_min * 60000u))) {
        bsp_display_lock(-1);
        s_pre_saver_scene_idx = scene_fw_current_index();
        s_saver_active = true;
        scene_trans_switch(clock_idx);
        bsp_display_unlock();
    }
    /* NB: while s_saver_active && on_clock we intentionally do nothing on
     * fresh activity — scene_clock's push subsystem surfaces task
     * start/end in place and returns to the face. The saver only ends
     * via the !on_clock branch above (key press / takeover moved us). */
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    lv_display_t *disp = bsp_display_start();

    agent_state_init();
    theme_init();
    /* Load persisted theme / device name / owner / default scene from NVS. */
    agent_commands_load_config();

    /* Register-then-listen: EVERY console command must exist before
     * console_protocol_init() spawns the reader task. The bridge pushes
     * `dash config`/`dash time` the instant it (re)connects; when that
     * burst lands mid-boot the lines sit in the USB-CDC RX buffer and are
     * drained the moment the console task starts. With the old order
     * (console first, `dash` registered a few calls later) those drained
     * lines deterministically bounced with "unknown command: dash" — and
     * since the bridge re-pushes config/time only on reconnect (not on
     * keepalive), the device sat on the default name/theme and a --:--
     * clock until the next reconnect. The dash handlers only depend on
     * agent_state + theme (initialised above), not on scenes (a snapshot
     * arriving pre-scene-registration simply skips the auto-switch). */
    harness_default_register();
    agent_commands_register();
    /* ?perf — real render/flush timing. Registered here so it obeys the
     * same register-then-listen rule as every other command. */
    perf_mon_init(disp);
    console_protocol_init();

    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(scr, lv_color_hex(pal->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    scene_fw_init(scr);
    scene_fw_set_change_listener(on_scene_changed);

    /* Register dashboard FIRST so it's the default (index 0). BOOT
     * cycles environment scenes in registration order (dashboard →
     * overview → …), skipping the takeovers. */
    scene_fw_register(&scene_dashboard);
    /* v5.2: scene_overview (wire id "idle") is RETIRED — with one agent
     * it showed a lone "1", and its fleet rollup belongs in a dashboard
     * summary row. The BOOT cycle is now just dashboard ↔ weather
     * (clock stays saver/lock-only). `dash idle` aliases to dashboard
     * for wire compat; an NVS default_scene of "idle" falls back to
     * index 0 naturally. Source kept at scenes/scene_overview.c
     * (unregistered, out of the build). */
    scene_fw_register(&scene_weather);
    scene_fw_register(&scene_clock);
    /* v5.2: scene_prompt is RETIRED (approvals happen in the terminal;
     * the panel is a display, not an input device — user's words).
     * Source kept at scenes/scene_prompt.c, out of the build. */
    /* v6.0: scene_awaiting is RETIRED (the dashboard gold pose is the
     * "your turn" view; auto_switch pulls the display to the dashboard
     * instead). Source kept at scenes/scene_awaiting.c, out of the
     * build — same convention as prompt/overview/pet. */

    lv_timer_create(frame_cb, 33, NULL);
    lv_timer_create(heap_watchdog_cb, HEAP_WD_PERIOD_MS, NULL);

    /* auto-switch: poll agent_state every 500ms; v6.0 — a fresh
     * awaiting rising edge pulls the display to the dashboard (one-
     * shot); also runs the clock screensaver + offline fallback. */
    extern void scene_auto_switch_cb(lv_timer_t *t);
    lv_timer_create(scene_auto_switch_cb, 500, NULL);

    bsp_display_unlock();

    if (!buttons_init()) {
        ESP_LOGW(TAG, "buttons_init failed — prompt scene needs button "
                      "support to send EVT decisions");
    }
    /* PWR (AXP2101) is best-effort: if the PMU doesn't answer, the other
     * two keys still route. */
    pwr_key_init();
    button_router_init();

    /* If config picked a non-default starting scene, honour it. */
    char start_scene[AGENT_DEFAULT_SCENE_MAX];
    agent_state_lock();
    strncpy(start_scene, agent_state_get()->default_scene, sizeof(start_scene));
    start_scene[sizeof(start_scene)-1] = '\0';
    agent_state_unlock();
    if (start_scene[0]) {
        int idx = scene_fw_find_by_id(start_scene);
        if (idx >= 0) {
            bsp_display_lock(-1);
            scene_trans_switch(idx);
            bsp_display_unlock();
        }
    }

    ESP_LOGI(TAG, "ready, %d scene(s), theme=%s",
             scene_fw_count(), theme_current_name());
}
