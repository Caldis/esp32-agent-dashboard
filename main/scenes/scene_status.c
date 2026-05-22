/*
 * scene_status — device status: free heap, uptime, console + scene count.
 *
 * We deliberately leave battery / wifi as placeholders ("--") since the
 * dashboard project doesn't link those peripherals; if the host wants
 * to surface them later it can push them via `dash status` (not yet
 * implemented — out of scope per AGENT.md).
 */

#include "scenes.h"

#include <stdio.h>

#include "lvgl.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "harness/scene_framework.h"

#define ACCENT_HEX  0x9EE493

typedef struct {
    lv_obj_t  *roman;
    lv_obj_t  *heap_val,  *heap_lbl;
    lv_obj_t  *up_val,    *up_lbl;
    lv_obj_t  *bat_val,   *bat_lbl;
    lv_obj_t  *wifi_val,  *wifi_lbl;
    lv_timer_t *timer;
} status_state_t;

static lv_obj_t *make_row_lbl(lv_obj_t *parent, int x, int y, const char *txt,
                              uint32_t colour, const lv_font_t *font, lv_opa_t opa)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, x, y);
    return l;
}

static void status_tick(lv_timer_t *t)
{
    status_state_t *st = (status_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    /* Free heap in KB. */
    size_t free_kb = esp_get_free_heap_size() / 1024u;
    char buf[24];
    snprintf(buf, sizeof(buf), "%u kB", (unsigned)free_kb);
    lv_label_set_text(st->heap_val, buf);

    /* Uptime mm:ss or h:mm. */
    int64_t up_us = esp_timer_get_time();
    uint32_t up_s = (uint32_t)(up_us / 1000000ULL);
    if (up_s < 3600) {
        snprintf(buf, sizeof(buf), "%lu:%02lu",
                 (unsigned long)(up_s / 60), (unsigned long)(up_s % 60));
    } else {
        snprintf(buf, sizeof(buf), "%luh%02lum",
                 (unsigned long)(up_s / 3600),
                 (unsigned long)((up_s / 60) % 60));
    }
    lv_label_set_text(st->up_val, buf);
}

static void status_init(scene_t *s, lv_obj_t *parent)
{
    status_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    st->roman = make_row_lbl(parent, 0, -170, "STATUS",
                              ACCENT_HEX, &lv_font_montserrat_14, LV_OPA_70);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);

    /* Two rows of two columns. */
    st->heap_val = make_row_lbl(parent, -80, -70, "0 kB",
                                 0xFFFFFF, &lv_font_montserrat_22, LV_OPA_COVER);
    st->heap_lbl = make_row_lbl(parent, -80, -38, "HEAP",
                                 0x6B7AA8, &lv_font_montserrat_12, LV_OPA_80);

    st->up_val   = make_row_lbl(parent,  80, -70, "0:00",
                                 0xFFFFFF, &lv_font_montserrat_22, LV_OPA_COVER);
    st->up_lbl   = make_row_lbl(parent,  80, -38, "UPTIME",
                                 0x6B7AA8, &lv_font_montserrat_12, LV_OPA_80);

    st->bat_val  = make_row_lbl(parent, -80,  40, "--",
                                 0xFFFFFF, &lv_font_montserrat_22, LV_OPA_60);
    st->bat_lbl  = make_row_lbl(parent, -80,  72, "BATTERY",
                                 0x6B7AA8, &lv_font_montserrat_12, LV_OPA_80);

    st->wifi_val = make_row_lbl(parent,  80,  40, "--",
                                 0xFFFFFF, &lv_font_montserrat_22, LV_OPA_60);
    st->wifi_lbl = make_row_lbl(parent,  80,  72, "WIFI",
                                 0x6B7AA8, &lv_font_montserrat_12, LV_OPA_80);

    st->timer = lv_timer_create(status_tick, 500, st);
    lv_timer_pause(st->timer);
    status_tick(st->timer);
}

static void status_on_show(scene_t *s)
{
    status_state_t *st = (status_state_t *)s->user_data;
    if (st && st->timer) {
        lv_timer_resume(st->timer);
        status_tick(st->timer);
    }
}

static void status_on_hide(scene_t *s)
{
    status_state_t *st = (status_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_status = {
    .id           = "status",
    .display_name = "V. Status",
    .accent       = LV_COLOR_MAKE(0x9E, 0xE4, 0x93),
    .description  = "Device status: heap, uptime, battery and wifi placeholders.",
    .tags         = "agent,status,system",
    .init         = status_init,
    .on_show      = status_on_show,
    .on_hide      = status_on_hide,
};
