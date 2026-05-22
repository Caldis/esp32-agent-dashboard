idf_component_register(
    SRCS
        "esp32_agent_dashboard_main.c"
        "scenes/scene_hello.c"
    INCLUDE_DIRS
        "."
        "scenes"
    REQUIRES
        lvgl__lvgl
        aurora-harness
        # Waveshare AMOLED 2.16 (Aurora's board) — pulled from
        # ${ESP_HARNESS_ROOT}/boards/ via EXTRA_COMPONENT_DIRS.
        # Replace with your own BSP if targeting a different board.
        esp32_s3_touch_amoled_2_16
    PRIV_REQUIRES
        nvs_flash
        esp_timer
)
