/*
 * esp32_agent_dashboard_main.c — host-driven AI agent dashboard.
 *
 * Boot order:
 *   1. NVS (RF calibration storage, settings)
 *   2. BSP display + LVGL
 *   3. console_protocol_init + harness default commands
 *   4. agent_state_init (single shared snapshot)
 *   5. scenes registered under LVGL lock
 *   6. agent_commands_register — `dash` subcommand family
 *   7. buttons_init (BOOT/USER for the prompt scene)
 *
 * After boot the device sits on `scene_idle` until the host pushes
 * state via `dash snapshot ...`. See main/harness/agent_commands.c
 * for the full protocol.
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "harness/console_protocol.h"
#include "harness/scene_framework.h"
#include "harness/default_cmds.h"

#include "scenes/scenes.h"
#include "agent_state.h"
#include "buttons.h"
#include "harness/agent_commands.h"

static const char *TAG = "esp32_agent_dashboard";

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
    /* NVS — needed for RF cal + future settings storage. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Bring up display + LVGL via the BSP. */
    bsp_display_start();

    /* Console protocol + default commands (?ping/?help/?stat/scene/...). */
    console_protocol_init();
    harness_default_register();

    /* Shared state struct + mutex. Must exist before any scene tick
     * or `dash *` handler runs. */
    agent_state_init();

    /* Build the UI under the LVGL lock. */
    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    scene_fw_init(scr);
    scene_fw_set_change_listener(on_scene_changed);

    scene_fw_register(&scene_idle);
    scene_fw_register(&scene_sessions);
    scene_fw_register(&scene_prompt);
    scene_fw_register(&scene_tokens);
    scene_fw_register(&scene_status);

    /* Per-frame tick feeds ?stat's fps field. */
    lv_timer_create(frame_cb, 33, NULL);

    bsp_display_unlock();

    /* Dash command family — registers `dash` with subcommand dispatch. */
    agent_commands_register();

    /* Physical buttons for the prompt scene. Non-fatal if it fails;
     * the prompt scene will still render but won't capture decisions. */
    if (!buttons_init()) {
        ESP_LOGW(TAG, "buttons_init failed — prompt scene needs button "
                      "support to send EVT decisions");
    }

    ESP_LOGI(TAG, "ready, %d scene(s)", scene_fw_count());
}
