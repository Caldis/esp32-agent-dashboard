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

static const char *TAG = "btn_router";

/* Screen-off state. s_saved_brightness survives the off phase so wake
 * restores what the user had, not a hardcoded 100%. */
static volatile bool s_screen_off;
static int           s_saved_brightness = 100;

/* ── screen off / wake ───────────────────────────────────────────── */

static void screen_set(bool off)
{
    /* brightness goes over the same QSPI panel IO the render task uses
     * for pixel data — serialise with the display lock. */
    bsp_display_lock(-1);
    if (off) {
        int b = bsp_display_brightness_get();
        if (b > 0) s_saved_brightness = b;
        bsp_display_brightness_set(0);
    } else {
        bsp_display_brightness_set(s_saved_brightness > 0 ? s_saved_brightness
                                                          : 100);
    }
    bsp_display_unlock();
    s_screen_off = off;
    console_send_evt("screen state=%s", off ? "off" : "on");
}

bool button_router_screen_is_off(void) { return s_screen_off; }

void button_router_screen_wake(void)
{
    if (s_screen_off) screen_set(false);
}

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

/* BOOT: next non-takeover scene. prompt/awaiting are entered by state,
 * never by cycling; while the AWAITING takeover owns the panel the view
 * is pinned (cycling away would just be yanked back by auto-switch).
 * Runs entirely under the display lock: the scene registry reads race
 * scene_fw_show on the LVGL task otherwise. */
static void cycle_view(void)
{
    bsp_display_lock(-1);
    const scene_t *cur = scene_fw_current();
    if (cur && strcmp(cur->id, "awaiting") == 0) {
        harness_toast("agent awaiting - view pinned", 1200);
        bsp_display_unlock();
        return;
    }
    int n = scene_fw_count();
    int cur_idx = scene_fw_current_index();
    for (int step = 1; n > 0 && step <= n; ++step) {
        int idx = (cur_idx + step) % n;
        const scene_t *s = scene_fw_get(idx);
        if (!s) continue;
        if (strcmp(s->id, "prompt") == 0 || strcmp(s->id, "awaiting") == 0) {
            continue;
        }
        if (idx == cur_idx) break;    /* nothing else to cycle to */
        scene_fw_show(idx);
        char t[48];
        snprintf(t, sizeof(t), "view: %s",
                 s->display_name ? s->display_name : s->id);
        harness_toast(t, 1200);
        break;
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
    /* A dark screen consumes the press that wakes it — no blind actions. */
    if (s_screen_off) {
        screen_set(false);
        return;
    }

    bool prompt = false;
    agent_state_lock();
    prompt = agent_state_get()->prompt_active;
    agent_state_unlock();

    if (prompt) {
        /* The prompt is the one required interaction — it keeps the old
         * BOOT=approve / USER=deny contract. PWR is ignored so the panel
         * can't go dark on top of a live countdown. */
        if (key == ROUTER_KEY_BOOT)      scene_prompt_decide("once");
        else if (key == ROUTER_KEY_USER) scene_prompt_decide("deny");
        return;
    }

    switch (key) {
        case ROUTER_KEY_BOOT: cycle_view();       break;
        case ROUTER_KEY_USER: cycle_focus();      break;
        case ROUTER_KEY_PWR:  screen_set(true);   break;
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
    ESP_LOGI(TAG, "router bound (BOOT=view, USER=focus, PWR=screen)");
}
