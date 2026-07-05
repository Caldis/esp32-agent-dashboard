# Top-level ESP-IDF project file for esp32_agent_dashboard.
#
# esp-harness consumption strategy:
#
#   link: EXTRA_COMPONENT_DIRS points at a sibling esp-harness checkout.
#         Set ESP_HARNESS_ROOT explicitly if your checkout lives elsewhere.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ESP_HARNESS_ROOT)
    if(DEFINED ENV{ESP_HARNESS_ROOT})
        set(ESP_HARNESS_ROOT "$ENV{ESP_HARNESS_ROOT}")
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../esp-harness/components")
        get_filename_component(ESP_HARNESS_ROOT
            "${CMAKE_CURRENT_LIST_DIR}/../esp-harness"
            ABSOLUTE)
    else()
        message(FATAL_ERROR
            "ESP_HARNESS_ROOT is not set and no sibling ../esp-harness "
            "checkout was found. Set ESP_HARNESS_ROOT to the esp-harness "
            "repository root before building.")
    endif()
endif()
list(APPEND EXTRA_COMPONENT_DIRS
    ${ESP_HARNESS_ROOT}/components
    ${ESP_HARNESS_ROOT}/boards
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_agent_dashboard)
