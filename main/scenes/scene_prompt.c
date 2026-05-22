/*
 * scene_prompt — full-screen permission prompt.
 *
 * Reads the prompt_active / prompt_tool / prompt_hint / prompt_id fields
 * from agent_state. Two big affordances at the bottom remind the user
 * which physical button does what:
 *
 *   [ BOOT ]   approve once       [ USER ]   deny
 *
 * On button press the bound callback fires `agent_prompt_decide()` which
 * (a) clears prompt_active, (b) emits an EVT line to the host, (c)
 * transitions back to scene_idle, (d) raises a toast confirming what
 * was sent.
 *
 * Auto-timeout: a 60 s lv_timer fires deny if no decision is made.
 */

#include "scenes.h"
#include "agent_state.h"
#include "buttons.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "harness/scene_framework.h"
#include "harness/console_protocol.h"
#include "harness/toast.h"
#include "esp_log.h"

#define TIMEOUT_MS    60000
#define ACCENT_HEX    0xF0E0A8

typedef struct {
    lv_obj_t   *title;          /* "Permission" */
    lv_obj_t   *tool;           /* tool name big */
    lv_obj_t   *hint;           /* tool hint short */
    lv_obj_t   *boot_chip;      /* "BOOT approve" left chip */
    lv_obj_t   *user_chip;      /* "USER deny" right chip */
    lv_obj_t   *timer_lbl;      /* "59s" countdown */
    lv_timer_t *tick;
    uint32_t    activated_ms;
    char        cached_id[AGENT_PROMPT_ID_MAX];
} prompt_state_t;

static prompt_state_t *s_active = NULL;

static lv_obj_t *make_chip(lv_obj_t *parent, const char *label,
                           lv_align_t align, int x, uint32_t colour)
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

    lv_obj_align(c, align, x, -50);
    return c;
}

static void prompt_decide(const char *decision)
{
    /* Snapshot id under lock, clear prompt_active, then emit EVT. */
    char id[AGENT_PROMPT_ID_MAX];
    bool had = false;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    if (s->prompt_active) {
        had = true;
        memcpy(id, s->prompt_id, sizeof(id));
        s->prompt_active = false;
        s->prompt_id[0] = '\0';
        s->prompt_tool[0] = '\0';
        s->prompt_hint[0] = '\0';
    }
    agent_state_unlock();

    if (!had) return;

    console_send_evt("permission id=%s decision=%s", id, decision);

    /* Toast for human feedback. */
    char toast_buf[64];
    snprintf(toast_buf, sizeof(toast_buf), "decision sent: %s", decision);
    harness_toast(toast_buf, 1500);

    /* Return to idle scene. */
    int idle_idx = scene_fw_find_by_id("idle");
    if (idle_idx >= 0) scene_fw_show(idle_idx);
}

static void on_boot(void *handle, void *usr)
{
    (void)handle; (void)usr;
    if (s_active == NULL) return;
    /* Only handle if we are currently the visible scene. */
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

static void prompt_tick(lv_timer_t *t)
{
    prompt_state_t *st = (prompt_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    /* Refresh display from agent_state. */
    char tool[AGENT_TOOL_MAX];
    char hint[AGENT_HINT_MAX];
    char id[AGENT_PROMPT_ID_MAX];
    bool active;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    active = s->prompt_active;
    memcpy(tool, s->prompt_tool, sizeof(tool));
    memcpy(hint, s->prompt_hint, sizeof(hint));
    memcpy(id,   s->prompt_id,   sizeof(id));
    agent_state_unlock();

    if (!active) {
        /* Host cleared the prompt out from under us — bail back to idle. */
        int idle_idx = scene_fw_find_by_id("idle");
        if (idle_idx >= 0) scene_fw_show(idle_idx);
        return;
    }

    /* If id changed, reset the timeout window. */
    if (strncmp(st->cached_id, id, sizeof(st->cached_id)) != 0) {
        memcpy(st->cached_id, id, sizeof(st->cached_id));
        st->activated_ms = lv_tick_get();
    }

    lv_label_set_text(st->tool, tool[0] ? tool : "?");
    lv_label_set_text(st->hint, hint);

    /* Countdown */
    uint32_t elapsed = lv_tick_get() - st->activated_ms;
    if (elapsed >= TIMEOUT_MS) {
        prompt_decide("deny");
        return;
    }
    uint32_t remaining = (TIMEOUT_MS - elapsed) / 1000u;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)remaining);
    lv_label_set_text(st->timer_lbl, buf);
}

static void prompt_init(scene_t *s, lv_obj_t *parent)
{
    prompt_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    s_active = st;

    /* "PERMISSION" top label. */
    st->title = lv_label_create(parent);
    lv_obj_set_style_text_font(st->title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->title, 4, 0);
    lv_obj_set_style_text_color(st->title, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_text_opa(st->title, LV_OPA_70, 0);
    lv_label_set_text(st->title, "PERMISSION");
    lv_obj_align(st->title, LV_ALIGN_CENTER, 0, -150);

    /* Tool name — big. */
    st->tool = lv_label_create(parent);
    lv_obj_set_style_text_font(st->tool, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(st->tool, lv_color_white(), 0);
    lv_obj_set_style_text_align(st->tool, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(st->tool, "?");
    lv_obj_align(st->tool, LV_ALIGN_CENTER, 0, -90);

    /* Hint — wrapped. */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(0xAAB6CC), 0);
    lv_obj_set_style_text_align(st->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->hint, 360);
    lv_label_set_long_mode(st->hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(st->hint, "");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, -20);

    /* Two chips. */
    st->boot_chip = make_chip(parent, "BOOT\napprove",
                               LV_ALIGN_BOTTOM_MID, -100, 0x9EE493);
    st->user_chip = make_chip(parent, "USER\ndeny",
                               LV_ALIGN_BOTTOM_MID, 100, 0xFF6B7E);

    /* Countdown timer in the corner. */
    st->timer_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->timer_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->timer_lbl, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_text_opa(st->timer_lbl, LV_OPA_60, 0);
    lv_label_set_text(st->timer_lbl, "60s");
    lv_obj_align(st->timer_lbl, LV_ALIGN_CENTER, 0, 60);

    st->tick = lv_timer_create(prompt_tick, 200, st);
    lv_timer_pause(st->tick);
}

static void prompt_on_show(scene_t *s)
{
    prompt_state_t *st = (prompt_state_t *)s->user_data;
    if (!st) return;
    /* Sync from state; treat show as the start of the timeout. */
    st->cached_id[0] = '\0';   /* force a tick reset */
    if (st->tick) {
        lv_timer_resume(st->tick);
        prompt_tick(st->tick);
    }
    /* Register button callbacks specific to this scene. The handlers
     * gate on scene_fw_current()->id so installing them once is safe. */
    buttons_set_handler(BUTTON_BOOT, on_boot, NULL);
    buttons_set_handler(BUTTON_USER, on_user, NULL);
}

static void prompt_on_hide(scene_t *s)
{
    prompt_state_t *st = (prompt_state_t *)s->user_data;
    if (st && st->tick) lv_timer_pause(st->tick);
    /* Leave handlers installed — they short-circuit on scene id. */
}

scene_t scene_prompt = {
    .id           = "prompt",
    .display_name = "III. Prompt",
    .accent       = LV_COLOR_MAKE(0xF0, 0xE0, 0xA8),
    .description  = "Full-screen permission prompt; BOOT approve, USER deny.",
    .tags         = "agent,prompt,interactive",
    .init         = prompt_init,
    .on_show      = prompt_on_show,
    .on_hide      = prompt_on_hide,
};
