idf_component_register(
    SRCS
        "esp32_agent_dashboard_main.c"
        "agent_state.c"
        "buttons.c"
        "theme.c"
        "tiny_json.c"
        "tool_icons.c"
        "harness/agent_commands.c"
        "scenes/scene_dashboard.c"
        "scenes/scene_idle.c"
        "scenes/scene_sessions.c"
        "scenes/scene_prompt.c"
        "scenes/scene_tokens.c"
        "scenes/scene_status.c"
    INCLUDE_DIRS
        "."
        "scenes"
        "harness"
    REQUIRES
        lvgl__lvgl
        aurora-harness
        # Waveshare AMOLED 2.16 (Aurora's board) — pulled from
        # ${ESP_HARNESS_ROOT}/boards/ via EXTRA_COMPONENT_DIRS.
        esp32_s3_touch_amoled_2_16
        # Physical buttons (BOOT / USER) for the permission prompt scene.
        espressif__button
    PRIV_REQUIRES
        nvs_flash
        esp_timer
)
