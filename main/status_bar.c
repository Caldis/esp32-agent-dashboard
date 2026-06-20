#include "status_bar.h"

#include <stdio.h>
#include <time.h>

#include "lvgl.h"

#define COL_TEXT       0xF3EEE2
#define COL_TEXT_DIM   0x8A807A
#define COL_TEAL       0x2BB3B1

#define HEADER_Y        56   /* time, top-center */
#define FOOTER_Y       420
#define FOOTER_CAP_Y   442
#define FOOTER_LEFT_X  124
#define FOOTER_RIGHT_X 284

static void fmt_clock(char *buf, size_t cap, const agent_state_t *st)
{
    if (st->host_epoch_unix > 0) {
        uint32_t now = st->host_epoch_unix
                     + (lv_tick_get() - st->host_clock_received_ms) / 1000;
        int32_t tz_now = (int32_t)now + st->host_tz_offset_seconds;
        time_t tt = (time_t)tz_now;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(buf, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(buf, cap, "--:--");
    }
}

static void fmt_tokens(char *buf, size_t cap, uint64_t tok)
{
    if (tok < 1000) {
        snprintf(buf, cap, "%u", (unsigned)tok);
    } else if (tok < 100000) {
        snprintf(buf, cap, "%.1fk", (double)tok / 1000.0);
    } else {
        snprintf(buf, cap, "%uk", (unsigned)(tok / 1000));
    }
}

static lv_obj_t *mk(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                    int x, int y, const char *init)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, init);
    lv_obj_set_pos(l, x, y);
    return l;
}

void status_bar_create(lv_obj_t *parent, status_bar_t *sb)
{
    sb->time_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(sb->time_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(sb->time_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(sb->time_lbl, "--:--");
    lv_obj_align(sb->time_lbl, LV_ALIGN_TOP_MID, 0, HEADER_Y);

    sb->active_num = mk(parent, &lv_font_montserrat_28, COL_TEAL,
                        FOOTER_LEFT_X, FOOTER_Y - 12, "0");
    sb->active_cap = mk(parent, &lv_font_montserrat_12, COL_TEXT_DIM,
                        FOOTER_LEFT_X, FOOTER_CAP_Y, "active");
    sb->token_num  = mk(parent, &lv_font_montserrat_28, COL_TEXT,
                        FOOTER_RIGHT_X, FOOTER_Y - 12, "0");
    sb->token_cap  = mk(parent, &lv_font_montserrat_12, COL_TEXT_DIM,
                        FOOTER_RIGHT_X, FOOTER_CAP_Y, "tokens today");
}

void status_bar_update(status_bar_t *sb, const agent_state_t *st)
{
    char buf[16];
    fmt_clock(buf, sizeof(buf), st);
    lv_label_set_text(sb->time_lbl, buf);

    char left[16];
    snprintf(left, sizeof(left), "%d", st->running + st->waiting);
    lv_label_set_text(sb->active_num, left);

    fmt_tokens(buf, sizeof(buf), st->tokens_today);
    lv_label_set_text(sb->token_num, buf);
}
