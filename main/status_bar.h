#pragma once
#include "lvgl.h"
#include "agent_state.h"

/* Shared status bar — top time + bottom active/token, identical across every
 * scene. Each scene owns one on its root: call status_bar_create() in init()
 * and status_bar_update() in tick(). One definition = one consistent style and
 * placement everywhere (vs each scene drawing its own header/footer). */
typedef struct {
    lv_obj_t *time_lbl;     /* "HH:MM" top-center, 48pt */
    lv_obj_t *active_num;   /* active count, footer-left, teal 28pt */
    lv_obj_t *active_cap;   /* "active" caption */
    lv_obj_t *token_num;    /* tokens today, footer-right, 28pt */
    lv_obj_t *token_cap;    /* "tokens today" caption */
    lv_obj_t *conn_lbl;     /* connection health: hidden when healthy, shows
                             * "waiting for host" / "host disconnected" when the
                             * snapshot stream (incl. 10s keepalive) goes stale */
    int       conn_state;   /* cached CONN_* to avoid re-setting text each tick */
} status_bar_t;

void status_bar_create(lv_obj_t *parent, status_bar_t *sb);
void status_bar_update(status_bar_t *sb, const agent_state_t *st);

/* Format the host-synced wall clock as "HH:MM" ("--:--" until the host
 * pushes `dash time`). Shared by the status bar's top clock and
 * scene_clock's big face so the two can never disagree. Call with the
 * agent_state lock held (reads host_epoch/tz fields). */
void status_bar_format_time(char *buf, size_t cap, const agent_state_t *st);
