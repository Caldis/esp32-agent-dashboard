idf_component_register(
    SRCS
        "esp32_agent_dashboard_main.c"
        "agent_state.c"
        "buttons.c"
        "pwr_key.c"
        "button_router.c"
        # pet.c retired in v5.3 (source kept; the pulse ring is the one
        # status glyph device-wide)
        "theme.c"
        "tiny_json.c"
        "tool_icons.c"
        "harness/agent_commands.c"
        "harness/agent_snapshot_apply.c"
        "scenes/scene_dashboard.c"
        # scene_overview.c retired in v5.2 (source kept for reference)
        "scenes/scene_weather.c"
        "scenes/scene_clock.c"
        # scene_prompt.c retired in v5.2 (source kept for reference)
        # scene_awaiting.c retired in v6.0 (dashboard gold pose absorbed it)
        "anim/apple_ease.c"
        "anim/spring.c"
        "scene_trans.c"
        "cjk_font.c"
        "ui_type.c"
        "status_bar.c"
    EMBED_FILES
        # Subset SimHei (GB2312 + ASCII, ~2MB) rendered on-device via lv_tiny_ttf.
        # Gitignored (derived from a system font); regenerate fonts/zh.ttf then
        # copy to main/zh.ttf.
        "zh.ttf"
        # Clock face digits (M PLUS Rounded 1c Black subset "0-9:-", ~2.4KB,
        # OFL — committed). Regenerate with tools/make_clock_font.py.
        "clock_digits.ttf"
        # Consolas UI subsets (ASCII+symbols, ~10KB each). Gitignored
        # (system-font derivative); regenerate with tools/make_ui_font.py.
        "ui.ttf"
        "ui_bold.ttf"
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
        # pwr_key.c talks to the AXP2101 PMU over the BSP's I2C bus.
        esp_driver_i2c
)
