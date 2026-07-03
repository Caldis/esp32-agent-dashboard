/*
 * scene_prompt — full-screen permission prompt.
 *
 * v1 changes:
 *   • Tool name pulses with a gentle scale animation (1.00 → 1.04 → 1.00
 *     over 1.5 s) so the device visibly demands attention.
 *   • Countdown turns red in the last 10 s.
 *   • Shows an agent-kind badge (e.g. "claude-code") tinted in the
 *     per-agent accent.
 *   • Emits `EVT: permission ... session_id=...` so the bridge can route
 *     the decision.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "buttons.h"
#include "cjk_font.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "harness/scene_framework.h"
#include "harness/console_protocol.h"
#include "harness/toast.h"
#include "esp_log.h"

#define TIMEOUT_MS         60000u
#define DANGER_WINDOW_MS   10000u
#define PULSE_PERIOD_MS    1500u
#define REPLY_TIMEOUT_MS   120000u

typedef struct {
    lv_obj_t   *title;
    lv_obj_t   *badge;          /* "claude-code" agent kind badge */
    lv_obj_t   *tool;
    lv_obj_t   *hint;
    lv_obj_t   *boot_chip;
    lv_obj_t   *user_chip;
    lv_obj_t   *timer_lbl;
    lv_timer_t *tick;
    uint32_t    activated_ms;
    uint32_t    pulse_t0_ms;
    char        cached_id[AGENT_PROMPT_ID_MAX];
} prompt_state_t;

static prompt_state_t *s_active = NULL;

static lv_obj_t *make_chip(lv_obj_t *parent, const char *label,
                           lv_align_t align, int x, int y, uint32_t colour)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 170, 56);
    lv_obj_set_style_radius(c, 28, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(colour), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_30, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(c);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, label);
    lv_obj_center(l);

    lv_obj_align(c, align, x, y);
    return c;
}

static void prompt_decide(const char *decision)
{
    char id[AGENT_PROMPT_ID_MAX];
    char sid[AGENT_SESSION_ID_MAX];
    bool had = false;
    bool is_reply = false;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    if (s->prompt_active) {
        had = true;
        is_reply = s->prompt_mode_reply;
        memcpy(id,  s->prompt_id,         sizeof(id));
        memcpy(sid, s->prompt_session_id, sizeof(sid));
        s->prompt_active = false;
        s->prompt_mode_reply = false;
        s->prompt_id[0] = '\0';
        s->prompt_tool[0] = '\0';
        s->prompt_hint[0] = '\0';
        s->prompt_agent_kind[0] = '\0';
        s->prompt_session_id[0] = '\0';
        s->decisions_sent++;
    }
    agent_state_unlock();
    if (!had) return;

    if (is_reply) {
        const char *choice = (strcmp(decision, "once") == 0) ? "0" : "1";
        console_send_evt("reply id=%s choice=%s", id, choice);
    } else if (sid[0]) {
        console_send_evt("permission id=%s decision=%s session_id=%s",
                         id, decision, sid);
    } else {
        console_send_evt("permission id=%s decision=%s", id, decision);
    }

    char toast_buf[64];
    snprintf(toast_buf, sizeof(toast_buf), "%s",
             is_reply ? "copied to clipboard" : decision);

    /* prompt_decide runs on the BUTTON task (BOOT/USER callbacks) as well as the
     * LVGL task (prompt_tick timeout). Both harness_toast (lv_async_call) and
     * scene_fw_show mutate the LVGL object tree, which is only safe under the
     * display lock. The lock is recursive, so re-taking it from the LVGL-task
     * path (which already holds it) is fine. Without this, a physical button
     * press raced the render task and could corrupt the widget tree. */
    bsp_display_lock(-1);
    harness_toast(toast_buf, 1500);
    int home_idx = scene_fw_find_by_id("dashboard");
    if (home_idx < 0) home_idx = scene_fw_find_by_id("idle");
    if (home_idx >= 0) scene_fw_show(home_idx);
    bsp_display_unlock();
}

static void on_boot(void *handle, void *usr)
{
    (void)handle; (void)usr;
    if (s_active == NULL) return;
    const scene_t *cur = scene_fw_current();
    if (cur == NULL || strcmp(cur->id, "prompt") != 0) return;
    prompt_decide("once");
}

static void on_user(void *handle, void *usr)
{
    (void)handle; (void)usr;
    if (s_active == NULL) return;
    const scene_t *cur = scene_fw_current();
    if (cur == NULL || strcmp(cur->id, "prompt") != 0) return;
    prompt_decide("deny");
}

/* Triangle-wave pulse → returns scale in 256-fixed-point.
 * 1.00 → 1.04 → 1.00 over PULSE_PERIOD_MS. */
static int32_t pulse_scale(uint32_t phase_ms)
{
    uint32_t p = phase_ms % PULSE_PERIOD_MS;
    uint32_t half = PULSE_PERIOD_MS / 2;
    /* tri 0..1.0 */
    uint32_t tri256 = (p < half) ? (p * 256u) / half
                                 : ((PULSE_PERIOD_MS - p) * 256u) / half;
    /* scale = 256 + tri * (1.04 - 1.00) = 256 + (tri256 * 10) / 256 */
    return 256 + (int32_t)((tri256 * 10u) / 256u);
}

static void prompt_tick(lv_timer_t *t)
{
    prompt_state_t *st = (prompt_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    char tool[AGENT_TOOL_MAX];
    char hint[AGENT_HINT_MAX];
    char id[AGENT_PROMPT_ID_MAX];
    char kind[AGENT_KIND_MAX];
    bool active;
    bool is_reply;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    active = s->prompt_active;
    is_reply = s->prompt_mode_reply;
    memcpy(tool, s->prompt_tool,       sizeof(tool));
    memcpy(hint, s->prompt_hint,       sizeof(hint));
    memcpy(id,   s->prompt_id,         sizeof(id));
    memcpy(kind, s->prompt_agent_kind, sizeof(kind));
    agent_state_unlock();

    if (!active) {
        int home_idx = scene_fw_find_by_id("dashboard");
        if (home_idx < 0) home_idx = scene_fw_find_by_id("idle");
        if (home_idx >= 0) scene_fw_show(home_idx);
        return;
    }

    const theme_palette_t *pal = theme_current();

    if (strncmp(st->cached_id, id, sizeof(st->cached_id)) != 0) {
        memcpy(st->cached_id, id, sizeof(st->cached_id));
        st->activated_ms = lv_tick_get();
        st->pulse_t0_ms  = st->activated_ms;
    }

    if (is_reply) {
        lv_label_set_text(st->title, "QUICK REPLY");
        lv_obj_set_style_text_color(st->title, lv_color_hex(0x2BB3B1), 0);
        lv_label_set_text(st->tool, "pick one:");
        lv_label_set_text(st->hint, "");
        lv_obj_set_style_text_color(st->tool, lv_color_hex(pal->text_dim), 0);
        lv_obj_set_style_transform_scale(st->tool, 256, 0);
        lv_obj_t *bl = lv_obj_get_child(st->boot_chip, 0);
        lv_obj_t *ul = lv_obj_get_child(st->user_chip, 0);
        if (bl) { char b[AGENT_HINT_MAX + 8]; snprintf(b, sizeof(b), "BOOT\n%s", tool); lv_label_set_text(bl, b); }
        if (ul) { char u[AGENT_HINT_MAX + 8]; snprintf(u, sizeof(u), "USER\n%s", hint); lv_label_set_text(ul, u); }
    } else {
        lv_label_set_text(st->title, "PERMISSION");
        lv_obj_set_style_text_color(st->title, lv_color_hex(pal->warning), 0);
        lv_label_set_text(st->tool, tool[0] ? tool : "?");
        lv_label_set_text(st->hint, hint);
        lv_obj_set_style_text_color(st->tool, lv_color_hex(pal->text), 0);
        lv_obj_set_style_text_color(st->hint, lv_color_hex(pal->text_dim), 0);
        lv_obj_t *bl = lv_obj_get_child(st->boot_chip, 0);
        lv_obj_t *ul = lv_obj_get_child(st->user_chip, 0);
        if (bl) lv_label_set_text(bl, "BOOT\napprove");
        if (ul) lv_label_set_text(ul, "USER\ndeny");
    }

    /* Agent badge */
    if (kind[0]) {
        lv_obj_clear_flag(st->badge, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(st->badge, kind);
        uint32_t accent = theme_accent_for_kind(kind);
        lv_obj_set_style_text_color(st->badge, lv_color_hex(accent), 0);
    } else {
        lv_obj_add_flag(st->badge, LV_OBJ_FLAG_HIDDEN);
    }

    /* Pulse — apply transform scale to the tool label. */
    uint32_t now = lv_tick_get();
    int32_t scale = pulse_scale(now - st->pulse_t0_ms);
    lv_obj_set_style_transform_scale(st->tool, scale, 0);
    lv_obj_set_style_transform_pivot_x(st->tool, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(st->tool, lv_pct(50), 0);

    /* Countdown */
    uint32_t elapsed = now - st->activated_ms;
    uint32_t timeout = is_reply ? REPLY_TIMEOUT_MS : TIMEOUT_MS;
    if (elapsed >= timeout) {
        if (is_reply) {
            agent_state_lock();
            agent_state_get()->prompt_active = false;
            agent_state_get()->prompt_mode_reply = false;
            agent_state_unlock();
            int home = scene_fw_find_by_id("dashboard");
            if (home < 0) home = scene_fw_find_by_id("idle");
            if (home >= 0) scene_fw_show(home);
        } else {
            prompt_decide("deny");
        }
        return;
    }
    uint32_t remaining = (timeout - elapsed) / 1000u;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)remaining);
    lv_label_set_text(st->timer_lbl, buf);
    if (TIMEOUT_MS - elapsed <= DANGER_WINDOW_MS) {
        lv_obj_set_style_text_color(st->timer_lbl, lv_color_hex(pal->danger), 0);
        lv_obj_set_style_text_opa(st->timer_lbl, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_text_color(st->timer_lbl, lv_color_hex(pal->warning), 0);
        lv_obj_set_style_text_opa(st->timer_lbl, LV_OPA_70, 0);
    }
}

static void prompt_init(scene_t *s, lv_obj_t *parent)
{
    prompt_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    s_active = st;

    const theme_palette_t *pal = theme_current();

    st->title = lv_label_create(parent);
    lv_obj_set_style_text_font(st->title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->title, 4, 0);
    lv_obj_set_style_text_color(st->title, lv_color_hex(pal->warning), 0);
    lv_obj_set_style_text_opa(st->title, LV_OPA_80, 0);
    lv_label_set_text(st->title, "PERMISSION");
    lv_obj_align(st->title, LV_ALIGN_CENTER, 0, -160);

    /* Agent badge below title. */
    st->badge = lv_label_create(parent);
    lv_obj_set_style_text_font(st->badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(st->badge, 1, 0);
    lv_label_set_text(st->badge, "");
    lv_obj_align(st->badge, LV_ALIGN_CENTER, 0, -130);

    /* tool + hint carry host-supplied text (tool name, command preview, and in
     * reply mode the chosen option strings) that can be Chinese — use the CJK
     * font so it isn't rendered as garbage boxes. */
    const lv_font_t *fp22 = cjk_font(22);
    const lv_font_t *fp14 = cjk_font(14);
    st->tool = lv_label_create(parent);
    lv_obj_set_style_text_font(st->tool, fp22 ? fp22 : &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(st->tool, lv_color_white(), 0);
    lv_obj_set_style_text_align(st->tool, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(st->tool, "?");
    lv_obj_align(st->tool, LV_ALIGN_CENTER, 0, -80);

    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, fp14 ? fp14 : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(st->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->hint, 360);
    lv_label_set_long_mode(st->hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(st->hint, "");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, -10);

    st->boot_chip = make_chip(parent, "BOOT\napprove",
                              LV_ALIGN_CENTER, -100, 110, pal->success);
    st->user_chip = make_chip(parent, "USER\ndeny",
                              LV_ALIGN_CENTER, 100, 110, pal->danger);

    st->timer_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->timer_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->timer_lbl, lv_color_hex(pal->warning), 0);
    lv_label_set_text(st->timer_lbl, "60s");
    lv_obj_align(st->timer_lbl, LV_ALIGN_CENTER, 0, 60);

    /* Bump tick rate so the pulse looks smooth. */
    st->tick = lv_timer_create(prompt_tick, 33, st);
    lv_timer_pause(st->tick);
}

static void prompt_on_show(scene_t *s)
{
    prompt_state_t *st = (prompt_state_t *)s->user_data;
    if (!st) return;
    st->cached_id[0] = '\0';
    st->pulse_t0_ms = lv_tick_get();
    if (st->tick) {
        lv_timer_resume(st->tick);
        prompt_tick(st->tick);
    }
    buttons_set_handler(BUTTON_BOOT, on_boot, NULL);
    buttons_set_handler(BUTTON_USER, on_user, NULL);
}

static void prompt_on_hide(scene_t *s)
{
    prompt_state_t *st = (prompt_state_t *)s->user_data;
    if (st && st->tick) lv_timer_pause(st->tick);
}

scene_t scene_prompt = {
    .id           = "prompt",
    .display_name = "Prompt",
    .accent       = LV_COLOR_MAKE(0xFF, 0xC8, 0x57),
    .description  = "Full-screen permission prompt with pulse and per-agent badge.",
    .tags         = "agent,prompt,interactive",
    .init         = prompt_init,
    .on_show      = prompt_on_show,
    .on_hide      = prompt_on_hide,
};
