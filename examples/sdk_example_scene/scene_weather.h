/*
 * scene_weather.h — example plugin scene header. Mirrors the dashboard's
 * own main/scenes/scenes.h pattern of exposing one extern scene_t per
 * file so other components can grep / xref by symbol.
 */

#pragma once

#include "harness/scene_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

extern scene_t scene_weather;

#ifdef __cplusplus
}
#endif
