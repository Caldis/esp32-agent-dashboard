/*
 * scene_prompt — full-screen permission prompt.
 *
 *   • Tool name at TITLE tier (52 px) is the decision object — the old
 *     per-frame transform_scale pulse is gone: scaling a big tiny_ttf
 *     label re-renders it through an intermediate layer every frame
 *     (the scene_clock plan-A lesson, 9-15 fps) and starved ?dump.
 *   • Countdown turns red in the last 10 s.
 *   • Shows an agent-kind badge (e.g. "claude-code") tinted in the
 *     per-agent accent.
 *   • Emits `EVT: permission ... session_id=...` so the bridge can route
 *     the decision.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "ui_type.h"

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
    char        cached_id[AGENT_PROMPT_ID_MAX];
} prompt_state_t;

static lv_obj_t *make_chip(lv_obj_t *parent, const char *label,
                           lv_align_t align, int x, int y, uint32_t colour)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 190, 84);   /* two LABEL lines + breathing room */
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(colour), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_30, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(c);
    /* Fixed size + DOT: reply-mode chips carry option strings that can
     * be long CJK — they must truncate inside the chip, not overflow. */
    lv_obj_set_size(l, 174, 2 * ui_type_line(UI_T_LABEL));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, ui_type(UI_T_LABEL), 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, label);
    lv_obj_center(l);

    lv_obj_align(c, align, x, y);
    return c;
}

/* v4: the scene the prompt takeover covered, restored on every exit
 * path (decision, external clear, timeout). -1 = nothing noted; fall
 * back to the default scene (index 0). Written under the display lock
 * (see scenes.h contract). */
static int s_pre_prompt_scene_idx = -1;

void scene_prompt_note_origin(void)
{
    int cur = scene_fw_current_index();
    const scene_t *s = scene_fw_get(cur);
    if (!s) return;
    /* Never note a takeover as origin: prompt-over-prompt keeps the
     * earlier note; prompt-over-awaiting falls back to the default and
     * lets the awaiting auto-switch reclaim from there. */
    if (strcmp(s->id, "prompt") == 0 || strcmp(s->id, "awaiting") == 0) return;
    s_pre_prompt_scene_idx = cur;
}

void scene_prompt_return_home(void)
{
    /* Idempotence guard: exits race (prompt_tick keeps firing during the
     * scene_fw_show crossfade, and the console task's prompt_clear path
     * checks-then-calls without owning the tick). Only the call that
     * finds us still ON the prompt scene restores; a second call would
     * see the consumed (-1) note and bounce to index 0. */
    const scene_t *cur = scene_fw_current();
    if (!cur || strcmp(cur->id, "prompt") != 0) return;
    int idx = s_pre_prompt_scene_idx;
    s_pre_prompt_scene_idx = -1;
    if (idx < 0 || idx >= scene_fw_count()) idx = 0;
    scene_fw_show(idx);
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
    scene_prompt_return_home();
    bsp_display_unlock();
}

/* Public entry for the button router — the physical BOOT/USER semantics
 * live in button_router.c now; this scene only owns the decision I/O.
 * prompt_decide already no-ops when no prompt is active, so a stray call
 * can't double-fire a decision. */
void scene_prompt_decide(const char *decision)
{
    prompt_decide(decision);
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
        /* Cleared externally (snapshot prompt:null raced us) — go back
         * to wherever the user was. Runs on the LVGL task. */
        scene_prompt_return_home();
        return;
    }

    const theme_palette_t *pal = theme_current();

    if (strncmp(st->cached_id, id, sizeof(st->cached_id)) != 0) {
        memcpy(st->cached_id, id, sizeof(st->cached_id));
        st->activated_ms = lv_tick_get();
    }

    if (is_reply) {
        lv_label_set_text(st->title, "QUICK REPLY");
        lv_obj_set_style_text_color(st->title, lv_color_hex(0x2BB3B1), 0);
        lv_label_set_text(st->tool, "pick one:");
        lv_label_set_text(st->hint, "");
        lv_obj_set_style_text_color(st->tool, lv_color_hex(pal->text_dim), 0);
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

    /* Countdown */
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - st->activated_ms;
    uint32_t timeout = is_reply ? REPLY_TIMEOUT_MS : TIMEOUT_MS;
    if (elapsed >= timeout) {
        if (is_reply) {
            agent_state_lock();
            agent_state_get()->prompt_active = false;
            agent_state_get()->prompt_mode_reply = false;
            agent_state_unlock();
            scene_prompt_return_home();
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

    const theme_palette_t *pal = theme_current();

    /* v4.4 top-anchored stack: eyebrow → badge → tool (the decision
     * object, TITLE) → hint (BODY, ≤2 lines) → chips → countdown. */
    st->title = lv_label_create(parent);
    lv_obj_set_style_text_font(st->title, ui_type_bold(UI_T_LABEL), 0);
    lv_obj_set_style_text_letter_space(st->title, 4, 0);
    lv_obj_set_style_text_color(st->title, lv_color_hex(pal->warning), 0);
    lv_obj_set_style_text_opa(st->title, LV_OPA_80, 0);
    lv_label_set_text(st->title, "PERMISSION");
    lv_obj_align(st->title, LV_ALIGN_TOP_MID, 0, 36);

    /* Agent badge below title. */
    st->badge = lv_label_create(parent);
    lv_obj_set_style_text_font(st->badge, ui_type(UI_T_CAPTION), 0);
    lv_obj_set_style_text_letter_space(st->badge, 1, 0);
    lv_label_set_text(st->badge, "");
    lv_obj_align(st->badge, LV_ALIGN_TOP_MID, 0, 76);

    /* tool + hint carry host-supplied text (tool name, command preview, and in
     * reply mode the chosen option strings) that can be Chinese — ui_type
     * chains to the CJK font so it isn't rendered as garbage boxes. */
    st->tool = lv_label_create(parent);
    lv_obj_set_size(st->tool, UI_CONTENT_W, ui_type_line(UI_T_TITLE));
    lv_label_set_long_mode(st->tool, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(st->tool, ui_type_bold(UI_T_TITLE), 0);
    lv_obj_set_style_text_color(st->tool, lv_color_white(), 0);
    lv_obj_set_style_text_align(st->tool, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(st->tool, "?");
    lv_obj_align(st->tool, LV_ALIGN_TOP_MID, 0, 114);

    st->hint = lv_label_create(parent);
    lv_obj_set_size(st->hint, UI_CONTENT_W, 2 * ui_type_line(UI_T_BODY));
    lv_label_set_long_mode(st->hint, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(st->hint, ui_type(UI_T_BODY), 0);
    lv_obj_set_style_text_align(st->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(st->hint, "");
    lv_obj_align(st->hint, LV_ALIGN_TOP_MID, 0, 192);

    st->boot_chip = make_chip(parent, "BOOT\napprove",
                              LV_ALIGN_TOP_MID, -103, 296, pal->success);
    st->user_chip = make_chip(parent, "USER\ndeny",
                              LV_ALIGN_TOP_MID, 103, 296, pal->danger);

    st->timer_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->timer_lbl, ui_type(UI_T_LABEL), 0);
    lv_obj_set_style_text_color(st->timer_lbl, lv_color_hex(pal->warning), 0);
    lv_label_set_text(st->timer_lbl, "60s");
    lv_obj_align(st->timer_lbl, LV_ALIGN_TOP_MID, 0, 404);

    /* 200 ms: countdown label updates once a second; nothing here
     * animates per-frame any more. */
    st->tick = lv_timer_create(prompt_tick, 200, st);
    lv_timer_pause(st->tick);
}

static void prompt_on_show(scene_t *s)
{
    prompt_state_t *st = (prompt_state_t *)s->user_data;
    if (!st) return;
    st->cached_id[0] = '\0';
    if (st->tick) {
        lv_timer_resume(st->tick);
        prompt_tick(st->tick);
    }
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
    .description  = "Full-screen permission prompt with per-agent badge.",
    .tags         = "agent,prompt,interactive",
    .init         = prompt_init,
    .on_show      = prompt_on_show,
    .on_hide      = prompt_on_hide,
};
