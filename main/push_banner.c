/*
 * push_banner.c — top-slide-down overlay. See push_banner.h.
 *
 * Creates a rounded-rect container on lv_layer_top() with tool name
 * (accent color) + hint (dim). Animates Y from off-screen to final
 * position, auto-removes after duration.
 */

#include "push_banner.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "anim/apple_ease.h"

#define BANNER_DEFAULT_MS  3000
#define BANNER_MAX_TEXT    96
#define BANNER_W           320
#define BANNER_H            40
#define BANNER_Y_FINAL      36
#define BANNER_Y_START     -10
#define BANNER_ANIM_MS     250

typedef struct {
    char     tool[48];
    char     hint[48];
    uint32_t duration_ms;
} banner_req_t;

static lv_obj_t   *s_container = NULL;
static lv_timer_t *s_timer     = NULL;

static void banner_purge(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_container) {
        lv_obj_delete(s_container);
        s_container = NULL;
    }
}

static void banner_expire_cb(lv_timer_t *t)
{
    (void)t;
    banner_purge();
}

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void banner_show_async(void *arg)
{
    banner_req_t *req = (banner_req_t *)arg;
    if (!req) return;

    banner_purge();

    lv_obj_t *c = lv_obj_create(lv_layer_top());
    lv_obj_set_size(c, BANNER_W, BANNER_H);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1C1814), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_90, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x2BB3B1), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_hor(c, 14, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(c, LV_LAYOUT_FLEX, 0);
    lv_obj_set_style_flex_flow(c, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(c, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_main_place(c, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, BANNER_Y_START);

    lv_obj_t *tool_lbl = lv_label_create(c);
    lv_obj_set_style_text_color(tool_lbl, lv_color_hex(0x2BB3B1), 0);
    lv_obj_set_style_text_font(tool_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(tool_lbl, req->tool);

    if (req->hint[0]) {
        lv_obj_t *hint_lbl = lv_label_create(c);
        lv_obj_set_style_text_color(hint_lbl, lv_color_hex(0x8A807A), 0);
        lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(hint_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(hint_lbl, 1);
        lv_label_set_text(hint_lbl, req->hint);
    }

    s_container = c;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c);
    lv_anim_set_values(&a, BANNER_Y_START, BANNER_Y_FINAL);
    lv_anim_set_time(&a, BANNER_ANIM_MS);
    lv_anim_set_path_cb(&a, apple_ease_out);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_start(&a);

    s_timer = lv_timer_create(banner_expire_cb, req->duration_ms, NULL);
    lv_timer_set_repeat_count(s_timer, 1);

    free(req);
}

static void banner_dismiss_async(void *arg)
{
    (void)arg;
    banner_purge();
}

void push_banner_show(const char *tool, const char *hint, uint32_t duration_ms)
{
    if (!tool) return;
    banner_req_t *req = (banner_req_t *)malloc(sizeof(*req));
    if (!req) return;
    strncpy(req->tool, tool, sizeof(req->tool) - 1);
    req->tool[sizeof(req->tool) - 1] = '\0';
    if (hint) {
        strncpy(req->hint, hint, sizeof(req->hint) - 1);
        req->hint[sizeof(req->hint) - 1] = '\0';
    } else {
        req->hint[0] = '\0';
    }
    req->duration_ms = duration_ms ? duration_ms : BANNER_DEFAULT_MS;
    lv_async_call(banner_show_async, req);
}

void push_banner_dismiss(void)
{
    lv_async_call(banner_dismiss_async, NULL);
}
