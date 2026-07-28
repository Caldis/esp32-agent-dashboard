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
#include "scene_flash.h"

static const char *TAG = "btn_router";

/* v4.3: PWR no longer darkens the panel — it locks the view to the
 * clock scene instead (see lock_clock_toggle). The screen-off API is
 * kept for its callers (scene_auto_switch_cb) but is now inert. */
bool button_router_screen_is_off(void) { return false; }
void button_router_screen_wake(void) {}

/* ── 直达式按键 (v6.6) ─────────────────────────────────────────────
 * 三个键、三个界面，一一对应，按下即到：
 *   BOOT = 总览 dashboard / USER = 天气 weather / PWR = 时间 clock
 *
 * 取代了 v4 的多态方案（BOOT 循环环境视图、PWR 在时钟与"上一个界面"之间
 * toggle、USER 轮换聚焦的 agent）。循环式导航要用户先知道自己在哪、再数
 * 还要按几下；直达没有这个心智负担，而设备恰好有三个键、三个界面。
 *
 * 已经在目标界面时不做场景切换，改为闪一圈边框高光（scene_flash）——
 * 按键必须有回应，否则用户无法区分"已经在这里"和"按键没生效"。
 *
 * 用 scene_trans_target() 而不是 scene_fw_current_index() 判断"是否已在
 * 目标"：转场是异步的，黑幕瞬切前当前场景仍是旧的，在转场窗口内连按会
 * 拿过时的现状做判断。（旧的 PWR toggle 正是栽在这上面：它在转场窗口里
 * 二次进入"我还在 clock"分支，而记忆的上一个场景已被消费成 -1，于是
 * fallback 到 0 —— 用户看到的"本该退回天气，却跳去总览"。）
 *
 * 代价：USER 键原来的"轮换聚焦 agent"没有了。焦点仍可由主机侧设置，
 * 只是不再有物理键入口——三个界面直达比它更常用。 */
static void go_scene(const char *id)
{
    bsp_display_lock(-1);
    int idx = scene_fw_find_by_id(id);
    if (idx < 0) { bsp_display_unlock(); return; }

    if (scene_trans_target() == idx) {
        scene_flash_ping();          /* 已经在这儿了 */
    } else {
        scene_trans_switch(idx);
    }
    bsp_display_unlock();
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
    /* 屏保持有时钟时，按键把它收回——原子消费，避免屏保的"有活动就
     * 恢复"与这次按键自己的场景切换打架。v6.6：不再有"PWR 收养屏保时钟
     * 作为手动锁"的特例，三个键各自直达即可。 */
    (void)scene_saver_consume();

    switch (key) {
        /* 物理排列（左->右）是 BOOT, PWR, USER —— 不是命名顺序。顶部的
         * nav_dots 按物理位置排，所以映射必须跟着物理走，否则指示点会
         * 指向错误的键。 */
        case ROUTER_KEY_BOOT: go_scene("dashboard"); break;  /* 左 */
        case ROUTER_KEY_PWR:  go_scene("clock");     break;  /* 中 */
        case ROUTER_KEY_USER: go_scene("weather");   break;  /* 右 */
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
