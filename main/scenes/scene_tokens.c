/*
 * scene_tokens — aggregate + per-agent token totals with sparkline.
 *
 * v1: shows the global "today / total" up top, then one sparkline per
 * agent below (each in its own accent colour) so you can see who's
 * burning the budget.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define SPARK_W        360
#define SPARK_H        60

typedef struct {
    lv_obj_t  *roman;
    lv_obj_t  *cum_value;
    lv_obj_t  *cum_lbl;
    lv_obj_t  *today_value;
    lv_obj_t  *today_lbl;
    lv_obj_t  *spark_lines[AGENT_SLOT_MAX];
    lv_point_precise_t pts[AGENT_SLOT_MAX][AGENT_SPARK_SAMPLES];
    lv_obj_t  *spark_labels[AGENT_SLOT_MAX];
    lv_timer_t *timer;
} tokens_state_t;

static void format_int(uint64_t v, char *buf, size_t cap)
{
    if (v < 1000) snprintf(buf, cap, "%llu", (unsigned long long)v);
    else if (v < 1000000ULL) snprintf(buf, cap, "%.1fk", (double)v / 1000.0);
    else snprintf(buf, cap, "%.1fM", (double)v / 1000000.0);
}

static void tokens_tick(lv_timer_t *t)
{
    tokens_state_t *st = (tokens_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    agent_slot_t snap[AGENT_SLOT_MAX];
    uint64_t cum, today;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    cum = s->tokens_cumulative;
    today = s->tokens_today;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) snap[i] = s->slots[i];
    agent_state_unlock();

    const theme_palette_t *pal = theme_current();
    char buf[32];
    format_int(cum, buf, sizeof(buf));
    lv_label_set_text(st->cum_value, buf);
    lv_obj_set_style_text_color(st->cum_value, lv_color_hex(pal->text), 0);

    format_int(today, buf, sizeof(buf));
    lv_label_set_text(st->today_value, buf);
    lv_obj_set_style_text_color(st->today_value, lv_color_hex(pal->accent_claude), 0);

    lv_obj_set_style_text_color(st->cum_lbl,   lv_color_hex(pal->text_dim), 0);
    lv_obj_set_style_text_color(st->today_lbl, lv_color_hex(pal->text_dim), 0);

    int idx_used = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        const agent_slot_t *sl = &snap[i];
        if (!sl->in_use || sl->spark_count < 2) {
            lv_line_set_points(st->spark_lines[i], NULL, 0);
            lv_label_set_text(st->spark_labels[i], "");
            continue;
        }
        int n = sl->spark_count;
        uint32_t maxv = 1;
        for (int j = 0; j < n; ++j) {
            int idx = (sl->spark_head - n + j + AGENT_SPARK_SAMPLES) % AGENT_SPARK_SAMPLES;
            if (sl->spark[idx] > maxv) maxv = sl->spark[idx];
        }
        int x_left = -SPARK_W / 2;
        int x_step = (n > 1) ? (SPARK_W / (n - 1)) : SPARK_W;
        int y_base = 40 + idx_used * (SPARK_H + 24);
        for (int j = 0; j < n; ++j) {
            int idx = (sl->spark_head - n + j + AGENT_SPARK_SAMPLES) % AGENT_SPARK_SAMPLES;
            uint32_t v = sl->spark[idx];
            int y = y_base + SPARK_H - (int)(((uint64_t)v * (uint64_t)SPARK_H) / (uint64_t)maxv);
            st->pts[i][j].x = x_left + j * x_step;
            st->pts[i][j].y = y;
        }
        uint32_t accent = theme_accent_for_kind(sl->kind);
        lv_obj_set_style_line_color(st->spark_lines[i], lv_color_hex(accent), 0);
        lv_line_set_points(st->spark_lines[i], st->pts[i], n);

        lv_label_set_text(st->spark_labels[i], sl->kind);
        lv_obj_set_style_text_color(st->spark_labels[i], lv_color_hex(accent), 0);
        lv_obj_align(st->spark_labels[i], LV_ALIGN_CENTER, -SPARK_W / 2,
                     y_base - 12);
        lv_obj_clear_flag(st->spark_labels[i], LV_OBJ_FLAG_HIDDEN);

        idx_used++;
    }
}

static void tokens_init(scene_t *s, lv_obj_t *parent)
{
    tokens_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    const theme_palette_t *pal = theme_current();

    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(pal->text_dim), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "TOKENS");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 16);

    st->cum_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->cum_value, &lv_font_montserrat_22, 0);
    lv_label_set_text(st->cum_value, "0");
    lv_obj_align(st->cum_value, LV_ALIGN_TOP_MID, -90, 50);

    st->cum_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->cum_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(st->cum_lbl, "TOTAL");
    lv_obj_align(st->cum_lbl, LV_ALIGN_TOP_MID, -90, 80);

    st->today_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->today_value, &lv_font_montserrat_22, 0);
    lv_label_set_text(st->today_value, "0");
    lv_obj_align(st->today_value, LV_ALIGN_TOP_MID, 90, 50);

    st->today_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->today_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(st->today_lbl, "TODAY");
    lv_obj_align(st->today_lbl, LV_ALIGN_TOP_MID, 90, 80);

    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        st->spark_lines[i] = lv_line_create(parent);
        lv_obj_set_style_line_width(st->spark_lines[i], 2, 0);
        lv_obj_set_style_line_opa(st->spark_lines[i], LV_OPA_90, 0);
        lv_obj_set_style_line_rounded(st->spark_lines[i], true, 0);
        lv_obj_align(st->spark_lines[i], LV_ALIGN_CENTER, 0, 0);

        st->spark_labels[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(st->spark_labels[i], &lv_font_montserrat_12, 0);
        lv_label_set_text(st->spark_labels[i], "");
    }

    st->timer = lv_timer_create(tokens_tick, 250, st);
    lv_timer_pause(st->timer);
    /* Defer first tick until on_show — see scene_sessions.c for the
     * stack-size rationale. */
}

static void tokens_on_show(scene_t *s)
{
    tokens_state_t *st = (tokens_state_t *)s->user_data;
    if (!st) return;
    if (st->timer) {
        lv_timer_resume(st->timer);
        tokens_tick(st->timer);
    }
}

static void tokens_on_hide(scene_t *s)
{
    tokens_state_t *st = (tokens_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_tokens = {
    .id           = "tokens",
    .display_name = "Tokens",
    .accent       = LV_COLOR_MAKE(0x5C, 0xD0, 0xD9),
    .description  = "Token totals plus per-agent sparkline.",
    .tags         = "agent,tokens",
    .init         = tokens_init,
    .on_show      = tokens_on_show,
    .on_hide      = tokens_on_hide,
};
