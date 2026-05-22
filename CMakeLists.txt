# Top-level ESP-IDF project file for esp32_agent_dashboard.
#
# aurora-harness vendoring strategy is set when this project was created
# (`esp-harness new --component-source <vendor|link|depend>`):
#
#   link: EXTRA_COMPONENT_DIRS points at the monorepo at
#         D:\Code\esp-harness. Lighter footprint than vendor; depends
#         on that path remaining valid.

cmake_minimum_required(VERSION 3.16)

# ESP_HARNESS_ROOT can be overridden by env var or -D on the
# build command; defaults to the path captured at scaffold time.
if(NOT DEFINED ESP_HARNESS_ROOT)
    if(DEFINED ENV{ESP_HARNESS_ROOT})
        set(ESP_HARNESS_ROOT "$ENV{ESP_HARNESS_ROOT}")
    else()
        set(ESP_HARNESS_ROOT "D:/Code/esp-harness")
    endif()
endif()
list(APPEND EXTRA_COMPONENT_DIRS
    ${ESP_HARNESS_ROOT}/components
    ${ESP_HARNESS_ROOT}/boards
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_agent_dashboard)
