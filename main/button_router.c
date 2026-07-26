/*
 * button_router — see button_router.h.
 */

#include "button_router.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "harness/scene_framework.h"
#include "harness/console_protocol.h"
#include "harness/toast.h"

#include "agent_state.h"
#include "buttons.h"
#include "pwr_key.h"
#include "scenes/scenes.h"
#include "scene_trans.h"

static const char *TAG = "btn_router";

/* v4.3: PWR no longer darkens the panel — it locks the view to the
 * clock scene instead (see lock_clock_toggle). The screen-off API is
 * kept for its callers (scene_auto_switch_cb) but is now inert. */
bool button_router_screen_is_off(void) { return false; }
void button_router_screen_wake(void) {}

/* ── ambient actions (multi-state cycles) ────────────────────────── */

/* harness_toast posts via lv_async_call, which in LVGL 9 is
 * lv_timer_create on the GLOBAL timer list — it is NOT safe against a
 * concurrently running LVGL task, despite toast.h's "callable from any
 * task" claim. scene_prompt learned this the hard way (see the lock
 * comment in prompt_decide); calling it unlocked from the button task
 * corrupts the timer list and wedges the swdraw render thread in a
 * glyph loop (task-watchdog on IDLE1). Every router toast goes through
 * here. The display lock is recursive, so LVGL-task callers are fine. */
static void toast_locked(const char *text, uint32_t ms)
{
    bsp_display_lock(-1);
    harness_toast(text, ms);
    bsp_display_unlock();
}

/* BOOT: cycle the ambient views (dashboard ↔ weather). Clock is
 * v4.2's screensaver — the idle timer enters it, a key press leaves it,
 * so it is skipped here (from clock, BOOT lands on dashboard). v6.0:
 * the awaiting takeover scene is retired, so BOOT always just cycles —
 * no dismissal special case; the auto-switch pull is edge-triggered
 * and never re-grabs after the user keys away.
 * Runs entirely under the display lock: the scene registry reads race
 * scene_fw_show on the LVGL task otherwise. */
static void cycle_view(void)
{
    bsp_display_lock(-1);
    int n = scene_fw_count();
    int cur_idx = scene_fw_current_index();
    for (int step = 1; n > 0 && step <= n; ++step) {
        int idx = (cur_idx + step) % n;
        const scene_t *s = scene_fw_get(idx);
        if (!s) continue;
        if (strcmp(s->id, "clock") == 0) {
            continue;
        }
        if (idx == cur_idx) break;    /* nothing else to cycle to */
        scene_trans_switch(idx);
        char t[48];
        snprintf(t, sizeof(t), "view: %s",
                 s->display_name ? s->display_name : s->id);
        harness_toast(t, 1200);
        break;
    }
    bsp_display_unlock();
}

/* PWR: lock the panel to the clock view (v4.3 — replaces screen-off).
 * One press parks the display on the big clock and it STAYS there
 * through any amount of agent activity ("lock screen" semantics —
 * unlike the idle screensaver, which yields to fresh messages). A
 * second PWR press returns to the view it covered; BOOT hops back into
 * the ambient pair directly. Prompt is handled earlier in
 * button_router_press. (v6.0: the awaiting-takeover special case is
 * gone with the scene itself.) */
static int s_pre_lock_scene_idx = -1;

static void lock_clock_toggle(void)
{
    bsp_display_lock(-1);
    int clock_idx = scene_fw_find_by_id("clock");
    if (clock_idx < 0) {
        bsp_display_unlock();
        return;
    }
    int cur_idx = scene_fw_current_index();
    if (cur_idx == clock_idx) {
        int back = (s_pre_lock_scene_idx >= 0) ? s_pre_lock_scene_idx : 0;
        s_pre_lock_scene_idx = -1;
        scene_trans_switch(back);
    } else {
        s_pre_lock_scene_idx = cur_idx;
        scene_trans_switch(clock_idx);
        harness_toast("clock locked - PWR/BOOT to leave", 1500);
    }
    bsp_display_unlock();
}

/* USER/Key3: cycle the focused agent — auto → slot A → slot B → auto.
 * Pure state change; scene_dashboard renders the focused slot with the
 * single-agent detail cluster on its next tick (≤500 ms). */
static void cycle_focus(void)
{
    char toast[48];
    int  next = -1;

    agent_state_lock();
    agent_state_t *st = agent_state_get();
    if (st->slot_count < 2) {
        /* Focus is a fleet concept; with 0-1 agents the ambient view
         * already shows everything. */
        st->focused_slot = -1;
        agent_state_unlock();
        toast_locked("focus: auto (fleet needs 2+)", 1200);
        return;
    }
    int cur = st->focused_slot;
    for (int i = (cur < 0 ? 0 : cur + 1); i < AGENT_SLOT_MAX; ++i) {
        if (st->slots[i].in_use) { next = i; break; }
    }
    st->focused_slot = next;
    if (next >= 0) {
        /* position among live slots, 1-based, for "2/3" */
        int pos = 0, live = 0;
        for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
            if (!st->slots[i].in_use) continue;
            live++;
            if (i <= next) pos++;
        }
        /* toast font is Montserrat (ASCII only) — stick to kind + index,
         * the focused view itself shows the CJK-capable detail. */
        snprintf(toast, sizeof(toast), "focus: %d/%d %s", pos, live,
                 st->slots[next].kind[0] ? st->slots[next].kind : "agent");
    } else {
        snprintf(toast, sizeof(toast), "focus: auto");
    }
    agent_state_unlock();

    toast_locked(toast, 1200);
    console_send_evt("focus slot=%d", next);
}

/* ── press routing ───────────────────────────────────────────────── */

void button_router_press(button_router_key_t key)
{
    /* Any press is user activity — resets the clock-screensaver timer. */
    agent_state_touch_activity();

    /* (v5.2: the prompt takeover — and its BOOT=approve/USER=deny key
     * hijack — is retired; approvals happen in the terminal.) */

    /* If the screensaver owns the clock, this press takes it over —
     * consuming the flag atomically so the saver's "restore on
     * activity" can't race the key's own scene change (the press
     * already touched last_activity_ms above). PWR adopts the saver's
     * clock as a manual lock; BOOT/USER fall through and act normally
     * (cycle_view from clock lands on dashboard). */
    int covered = scene_saver_consume();
    if (covered >= 0 && key == ROUTER_KEY_PWR) {
        s_pre_lock_scene_idx = covered;
        toast_locked("clock locked - PWR/BOOT to leave", 1500);
        return;
    }

    switch (key) {
        case ROUTER_KEY_BOOT: cycle_view();         break;
        case ROUTER_KEY_USER: cycle_focus();        break;
        case ROUTER_KEY_PWR:  lock_clock_toggle();  break;
        default: break;
    }
}

/* ── glue ────────────────────────────────────────────────────────── */

static void on_boot(void *handle, void *usr)
{
    (void)handle; (void)usr;
    button_router_press(ROUTER_KEY_BOOT);
}

static void on_user(void *handle, void *usr)
{
    (void)handle; (void)usr;
    button_router_press(ROUTER_KEY_USER);
}

static void on_pwr(void *usr)
{
    (void)usr;
    button_router_press(ROUTER_KEY_PWR);
}

void button_router_init(void)
{
    buttons_set_handler(BUTTON_BOOT, on_boot, NULL);
    buttons_set_handler(BUTTON_USER, on_user, NULL);
    pwr_key_set_handler(on_pwr, NULL);
    ESP_LOGI(TAG, "router bound (BOOT=view, USER=focus, PWR=clock lock)");
}
