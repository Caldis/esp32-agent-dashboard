idf_component_register(
    SRCS
        "esp32_agent_dashboard_main.c"
        "agent_state.c"
        "buttons.c"
        "theme.c"
        "tiny_json.c"
        "tool_icons.c"
        "harness/agent_commands.c"
        "harness/agent_snapshot_apply.c"
        "scenes/scene_dashboard.c"
        "scenes/scene_idle.c"
        "scenes/scene_sessions.c"
        "scenes/scene_prompt.c"
        "scenes/scene_tokens.c"
        "scenes/scene_status.c"
        "scenes/scene_awaiting.c"
        "push_banner.c"
        "anim/apple_ease.c"
        "cjk_font.c"
        "status_bar.c"
    EMBED_FILES
        # Subset SimHei (GB2312 + ASCII, ~2MB) rendered on-device via lv_tiny_ttf.
        # Gitignored (derived from a system font); regenerate fonts/zh.ttf then
        # copy to main/zh.ttf.
        "zh.ttf"
    INCLUDE_DIRS
        "."
        "scenes"
        "harness"
        "anim"
    REQUIRES
        lvgl__lvgl
        # Renamed from `aurora-harness` to `esp-harness-core` in
        # esp-harness v3.0 — same library, clearer name (it was never
        # Aurora-specific, just historical).
        esp-harness-core
        # Waveshare AMOLED 2.16 (the board this project targets) —
        # pulled from ${ESP_HARNESS_ROOT}/boards/ via EXTRA_COMPONENT_DIRS.
        esp32_s3_touch_amoled_2_16
        # Physical buttons (BOOT / USER) for the permission prompt scene.
        espressif__button
    PRIV_REQUIRES
        nvs_flash
        esp_timer
)
