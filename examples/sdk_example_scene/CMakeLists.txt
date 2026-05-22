# Example plugin — `Weather` (scaffold for v1.1.0 plugin SDK).
#
# This file lets a user `idf.py build` this directory as an ESP-IDF
# component in their fork, e.g. by adding to the project root
# CMakeLists.txt before `project(...)`:
#
#     list(APPEND EXTRA_COMPONENT_DIRS examples/sdk_example_scene)
#
# Per PLUG1's hard rules, the dashboard's own main/CMakeLists.txt is
# NOT modified to include this component — F2 wires it in when
# v1.1.0 actually lands.
#
# REQUIRES rationale:
#   • lvgl__lvgl      — the scene uses LVGL APIs directly
#   • aurora-harness  — for harness/scene_framework.h + console_protocol.h
#   • main            — to pick up plugin_api.h + the plugin_loader's
#                       linker section attribute symbols. (`main` exposes
#                       its INCLUDE_DIRS via idf_component_register.)

idf_component_register(
    SRCS
        "scene_weather.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        lvgl__lvgl
        aurora-harness
    PRIV_REQUIRES
        main
)
