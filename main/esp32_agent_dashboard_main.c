/*
 * esp32_agent_dashboard_main.c — host-driven AI agent dashboard.
 *
 * Boot order:
 *   1. NVS (RF calibration storage, settings)
 *   2. BSP display + LVGL
 *   3. console_protocol_init + harness default commands
 *   4. agent_state_init + theme_init + load persisted config
 *   5. scenes registered under LVGL lock
 *   6. agent_commands_register — `dash` subcommand family
 *   7. buttons_init (BOOT/USER for the prompt scene)
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

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    bsp_display_start();

    console_protocol_init();
    harness_default_register();

    agent_state_init();
    theme_init();
    /* Load persisted theme / device name / owner / default scene from NVS. */
    agent_commands_load_config();

    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(scr, lv_color_hex(pal->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    scene_fw_init(scr);
    scene_fw_set_change_listener(on_scene_changed);

    /* Register dashboard FIRST so it's the default (index 0). */
    scene_fw_register(&scene_dashboard);
    scene_fw_register(&scene_idle);
    scene_fw_register(&scene_sessions);
    scene_fw_register(&scene_prompt);
    scene_fw_register(&scene_tokens);
    scene_fw_register(&scene_status);

    lv_timer_create(frame_cb, 33, NULL);
    lv_timer_create(heap_watchdog_cb, HEAP_WD_PERIOD_MS, NULL);

    bsp_display_unlock();

    agent_commands_register();

    if (!buttons_init()) {
        ESP_LOGW(TAG, "buttons_init failed — prompt scene needs button "
                      "support to send EVT decisions");
    }

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
