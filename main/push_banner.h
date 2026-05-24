/*
 * push_banner — top-slide-down notification overlay for tool events.
 *
 * Shows "tool_name hint" in a translucent banner at the top of the
 * round display for `duration_ms`, then auto-dismisses. Slide-down
 * entry animation using apple_ease_out.
 *
 * Thread-safe: dispatches via lv_async_call. One banner at a time;
 * new calls replace the prior banner.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void push_banner_show(const char *tool, const char *hint, uint32_t duration_ms);
void push_banner_dismiss(void);

#ifdef __cplusplus
}
#endif
