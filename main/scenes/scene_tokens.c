/*
 * scene_tokens — cumulative + today token counters with a sparkline of
 * the last N samples (set by `dash tokens` writes from the host).
 *
 * Sparkline is rendered via lv_line, with points spread evenly across
 * a 320 px wide band. Y is normalised against the max sample in the
 * window so the line uses the full vertical range even when the absolute
 * numbers are small.
 */

#include "scenes.h"
#include "agent_state.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define ACCENT_HEX     0x8FD9FF
#define SPARK_W        320
#define SPARK_H        120
#define SPARK_Y_OFFSET 60

typedef struct {
    lv_obj_t  *roman;
    lv_obj_t  *cum_value;
    lv_obj_t  *cum_lbl;
    lv_obj_t  *today_value;
    lv_obj_t  *today_lbl;
    lv_obj_t  *line;
    lv_point_precise_t points[AGENT_SPARK_SAMPLES];
    int        point_count;
    uint64_t   cached_cum;
    uint64_t   cached_today;
    int        cached_spark_count;
    int        cached_spark_head;
    lv_timer_t *timer;
} tokens_state_t;

static void format_int(uint64_t v, char *buf, size_t cap)
{
    /* Compact thousand suffixing: 1.2k / 3.4M. Helpful for token counts
     * that can easily reach 10^6+. */
    if (v < 1000) {
        snprintf(buf, cap, "%llu", (unsigned long long)v);
    } else if (v < 1000000ULL) {
        snprintf(buf, cap, "%.1fk", (double)v / 1000.0);
    } else {
        snprintf(buf, cap, "%.1fM", (double)v / 1000000.0);
    }
}

static void tokens_tick(lv_timer_t *t)
{
    tokens_state_t *st = (tokens_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    uint64_t cum, today;
    uint32_t samples[AGENT_SPARK_SAMPLES];
    int      sample_n;
    int      head;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    cum      = s->tokens_cumulative;
    today    = s->tokens_today;
    sample_n = s->spark_count;
    head     = s->spark_head;
    memcpy(samples, s->spark, sizeof(samples));
    agent_state_unlock();

    if (cum != st->cached_cum) {
        char buf[32]; format_int(cum, buf, sizeof(buf));
        lv_label_set_text(st->cum_value, buf);
        st->cached_cum = cum;
    }
    if (today != st->cached_today) {
        char buf[32]; format_int(today, buf, sizeof(buf));
        lv_label_set_text(st->today_value, buf);
        st->cached_today = today;
    }

    if (sample_n != st->cached_spark_count || head != st->cached_spark_head) {
        st->cached_spark_count = sample_n;
        st->cached_spark_head  = head;

        if (sample_n <= 1) {
            lv_line_set_points(st->line, NULL, 0);
            st->point_count = 0;
            return;
        }
        /* Walk the ring oldest-first. */
        uint32_t max_v = 1;
        for (int i = 0; i < sample_n; ++i) {
            int idx = (head - sample_n + i + AGENT_SPARK_SAMPLES) % AGENT_SPARK_SAMPLES;
            if (samples[idx] > max_v) max_v = samples[idx];
        }
        int x_left = -SPARK_W / 2;
        int x_step = (sample_n > 1) ? (SPARK_W / (sample_n - 1)) : SPARK_W;
        for (int i = 0; i < sample_n; ++i) {
            int idx = (head - sample_n + i + AGENT_SPARK_SAMPLES) % AGENT_SPARK_SAMPLES;
            uint32_t v = samples[idx];
            int y = -(int)(((uint64_t)v * (uint64_t)SPARK_H) / (uint64_t)max_v);
            st->points[i].x = x_left + i * x_step;
            st->points[i].y = SPARK_Y_OFFSET + y + SPARK_H / 2;
        }
        st->point_count = sample_n;
        lv_line_set_points(st->line, st->points, st->point_count);
    }
}

static void tokens_init(scene_t *s, lv_obj_t *parent)
{
    tokens_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->cached_cum = (uint64_t)-1;
    st->cached_today = (uint64_t)-1;
    st->cached_spark_count = -1;
    st->cached_spark_head  = -1;

    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "TOKENS");
    lv_obj_align(st->roman, LV_ALIGN_CENTER, 0, -170);

    /* Two big numbers side by side. */
    st->cum_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->cum_value, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(st->cum_value, lv_color_white(), 0);
    lv_label_set_text(st->cum_value, "0");
    lv_obj_align(st->cum_value, LV_ALIGN_CENTER, -80, -120);

    st->cum_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->cum_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(st->cum_lbl, lv_color_hex(0x6B7AA8), 0);
    lv_label_set_text(st->cum_lbl, "TOTAL");
    lv_obj_align(st->cum_lbl, LV_ALIGN_CENTER, -80, -90);

    st->today_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->today_value, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(st->today_value, lv_color_hex(ACCENT_HEX), 0);
    lv_label_set_text(st->today_value, "0");
    lv_obj_align(st->today_value, LV_ALIGN_CENTER, 80, -120);

    st->today_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->today_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(st->today_lbl, lv_color_hex(0x6B7AA8), 0);
    lv_label_set_text(st->today_lbl, "TODAY");
    lv_obj_align(st->today_lbl, LV_ALIGN_CENTER, 80, -90);

    /* Sparkline */
    st->line = lv_line_create(parent);
    lv_obj_set_style_line_color(st->line, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_line_width(st->line, 2, 0);
    lv_obj_set_style_line_opa(st->line, LV_OPA_90, 0);
    lv_obj_set_style_line_rounded(st->line, true, 0);
    lv_obj_align(st->line, LV_ALIGN_CENTER, 0, 0);

    st->timer = lv_timer_create(tokens_tick, 250, st);
    lv_timer_pause(st->timer);
    tokens_tick(st->timer);
}

static void tokens_on_show(scene_t *s)
{
    tokens_state_t *st = (tokens_state_t *)s->user_data;
    if (!st) return;
    st->cached_cum = (uint64_t)-1;
    st->cached_today = (uint64_t)-1;
    st->cached_spark_count = -1;
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
    .display_name = "IV. Tokens",
    .accent       = LV_COLOR_MAKE(0x8F, 0xD9, 0xFF),
    .description  = "Token totals (cumulative + today) with sparkline window.",
    .tags         = "agent,tokens",
    .init         = tokens_init,
    .on_show      = tokens_on_show,
    .on_hide      = tokens_on_hide,
};
