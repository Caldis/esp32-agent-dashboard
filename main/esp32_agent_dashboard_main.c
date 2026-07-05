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

/* v2.3.0: auto-switch between AWAITING takeover and the active
 * non-awaiting scene. Remembers what the user was on before the
 * takeover so we restore them when awaiting clears.
 *
 * v3.0 fleet rule: the takeover only fires when EXACTLY ONE agent is
 * live. With 2+ agents a full-screen takeover would hide every other
 * agent's progress — the dashboard's fleet rows carry the awaiting
 * state (gold highlight) instead. */
static int s_pre_awaiting_scene_idx = -1;

/* v4.2 clock screensaver: after screensaver_min minutes with no
 * activity (key press / dash prompt / dash event / a snapshot that
 * actually changed state), glide to the clock scene. Fresh activity
 * while saving restores the covered view; a key press exits via
 * cycle_view (which skips clock). Manual `dash scene clock` visits are
 * NOT screensaver entries — no flag, so nothing yanks the user away. */
static bool     s_saver_active = false;
static int      s_pre_saver_scene_idx = -1;
static uint32_t s_saver_enter_activity_ms = 0;

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
    int awaiting_idx = scene_fw_find_by_id("awaiting");
    int current_idx = scene_fw_current_index();
    if (awaiting_idx < 0 || current_idx < 0) return;

    bool any_awaiting = false;
    bool prompt_active = false;
    int  slot_count = 0;
    int32_t  saver_min = 0;
    uint32_t last_activity_ms = 0;
    agent_state_lock();
    if (agent_state_most_recent_awaiting() != NULL) any_awaiting = true;
    prompt_active = agent_state_get()->prompt_active;
    slot_count = agent_state_get()->slot_count;
    saver_min = agent_state_get()->screensaver_min;
    last_activity_ms = agent_state_get()->last_activity_ms;
    agent_state_unlock();

    /* An active permission prompt is the higher-priority interactive scene: it
     * owns the physical BOOT/USER buttons. If we let the AWAITING takeover grab
     * the screen while a prompt is up, the prompt is hidden (its countdown
     * pauses, its buttons stop responding) yet AWAITING shows "BOOT approve /
     * USER deny" that do nothing — the device-side approval becomes dead. So
     * suppress the takeover while a prompt is active. */
    /* (v4.3: screen-off is gone — PWR locks to the clock instead — so
     * the wake call is inert; kept for the day a real screen-off
     * returns.) */
    if ((any_awaiting || prompt_active) && button_router_screen_is_off()) {
        button_router_screen_wake();
    }

    bool on_awaiting = (current_idx == awaiting_idx);
    bool want_takeover = any_awaiting && (slot_count <= 1);
    if (want_takeover && !on_awaiting && !prompt_active) {
        s_pre_awaiting_scene_idx = current_idx;
        bsp_display_lock(-1);
        scene_fw_show(awaiting_idx);
        bsp_display_unlock();
        return;
    } else if (!want_takeover && on_awaiting) {
        /* Either nothing is awaiting anymore, or a second agent appeared —
         * both mean the takeover must yield (to the previous scene / fleet). */
        int back = (s_pre_awaiting_scene_idx >= 0) ? s_pre_awaiting_scene_idx : 0;
        bsp_display_lock(-1);
        scene_fw_show(back);
        bsp_display_unlock();
        s_pre_awaiting_scene_idx = -1;
        return;
    }

    /* ── clock screensaver ─────────────────────────────────────────── */
    int clock_idx = scene_fw_find_by_id("clock");
    if (clock_idx < 0) return;
    bool on_clock = (current_idx == clock_idx);
    uint32_t now = lv_tick_get();

    if (s_saver_active && !on_clock) {
        /* A key press / takeover already moved us off the clock. */
        s_saver_active = false;
        s_pre_saver_scene_idx = -1;
    } else if (s_saver_active && on_clock
               && last_activity_ms != s_saver_enter_activity_ms) {
        /* New activity while saving — bring back what the clock
         * covered. Re-check the flag UNDER the display lock: a key
         * press may have consumed the saver (scene_saver_consume, also
         * under the lock) between our unlocked read above and here —
         * acting on the stale read would evict a freshly PWR-locked
         * clock. */
        bsp_display_lock(-1);
        if (s_saver_active && scene_fw_current_index() == clock_idx) {
            int back = (s_pre_saver_scene_idx >= 0) ? s_pre_saver_scene_idx : 0;
            s_saver_active = false;
            s_pre_saver_scene_idx = -1;
            scene_fw_show(back);
        }
        bsp_display_unlock();
    } else if (!s_saver_active && !on_clock
               && !any_awaiting && !prompt_active
               && !button_router_screen_is_off()
               && saver_min > 0
               && (now - last_activity_ms) >= (uint32_t)saver_min * 60000u) {
        bsp_display_lock(-1);
        s_pre_saver_scene_idx = scene_fw_current_index();
        s_saver_enter_activity_ms = last_activity_ms;
        s_saver_active = true;
        scene_fw_show(clock_idx);
        bsp_display_unlock();
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    bsp_display_start();

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
    scene_fw_register(&scene_overview);
    scene_fw_register(&scene_clock);
    scene_fw_register(&scene_prompt);
    /* v2.3.0 AWAITING takeover — the scene that fires when any agent
     * is blocking on user input. Not the default (entered automatically
     * by the auto_switch_cb timer when slots report awaiting state). */
    scene_fw_register(&scene_awaiting);

    lv_timer_create(frame_cb, 33, NULL);
    lv_timer_create(heap_watchdog_cb, HEAP_WD_PERIOD_MS, NULL);

    /* v2.3.0 auto-switch: poll agent_state every 500ms; if any slot is
     * AWAITING_* we switch to scene_awaiting; if no slot is awaiting and
     * we're currently on awaiting, switch back to the previously-active
     * scene (or default_scene). */
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
            scene_fw_show(idx);
            bsp_display_unlock();
        }
    }

    ESP_LOGI(TAG, "ready, %d scene(s), theme=%s",
             scene_fw_count(), theme_current_name());
}
