#pragma once
#include "harness/scene_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

extern scene_t scene_dashboard;
extern scene_t scene_idle;
extern scene_t scene_sessions;
extern scene_t scene_prompt;
extern scene_t scene_tokens;
extern scene_t scene_status;
extern scene_t scene_awaiting;   /* v2.3.0 takeover */

#ifdef __cplusplus
}
#endif
