#pragma once
#include "lvgl.h"
#include "agent_state.h"
#include "scene_trans.h"

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
    int       conn_state;   /* cached CONN_* to avoid re-styling each tick */
} status_bar_t;

void status_bar_create(lv_obj_t *parent, status_bar_t *sb);
void status_bar_update(status_bar_t *sb, const agent_state_t *st);

/* Footer 四件套的规范转场演员（v6.2）。footer 是跨场景的共享组件，所以
 * 它的演员定义也只能有一处：每个带 footer 的场景都从这里取，姿态一致
 * 由构造保证——共享元素判定要求两侧姿态全等，手抄一份迟早会漂移，
 * 漂移的表现是 footer 又开始"飞出去再飞回来"。
 * 填满 out[STATUS_BAR_TRANS_ACTORS]，返回填入个数。 */
#define STATUS_BAR_TRANS_ACTORS 4
int status_bar_trans_actors(const status_bar_t *sb, trans_actor_t *out);

/* Format the host-synced wall clock as "HH:MM" ("--:--" until the host
 * pushes `dash time`). Shared by the status bar's top clock and
 * scene_clock's big face so the two can never disagree. Call with the
 * agent_state lock held (reads host_epoch/tz fields). */
void status_bar_format_time(char *buf, size_t cap, const agent_state_t *st);
