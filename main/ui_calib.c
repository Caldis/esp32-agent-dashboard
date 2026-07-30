/*
 * ui_calib — 面板可见区原点的标定尺 (v7.3)。
 *
 * ui_screen.h 里的 UI_VIS_INSET 一直是个【猜】：可见区在 LVGL 480 空间
 * 里的起点 a 从未量准，只夹逼出 a ∈ [1,6]，而贴边元素（辉光）按 a=0
 * 的几何摆放。后果可推导也已被用户看见：
 *
 *   一圈方框摆在 LVGL [k, k+465]，可见区是 [a, a+465]
 *     k <  a → 左/上那两条【出屏】看不见，右/下与边缘差 (a-k)
 *     k >  a → 右/下那两条被切，左/上与边缘差 (k-a)
 *     k == a → 四条边同时贴住物理边缘
 *
 * 所以"哪个 k 画出完整一圈"就是 a——这是屏幕自己能回答的问题，只需要
 * 一双眼睛看一次。截图回答不了：`?dump` 读的是 LVGL 帧缓冲，物理上哪
 * 一列先点亮不在帧缓冲里。
 *
 * `?vis 1` 同时画四个候选（不同色，2px 粗、间隔 3px），一眼定 a；
 * `?vis 0` 收起。x/y 两轴分别成立——若 a_x != a_y，会看到某色只有左右
 * 两条贴边、另一色只有上下两条，同样能读出来。
 *
 * 量准之后：把 a 写进 ui_screen.h 的 UI_VIS_ORG，贴边元素改用
 * (UI_VIS_ORG + inset) 定位，这把尺子留着——面板批次换了可以复量。
 */

#include "ui_calib.h"
#include "ui_screen.h"

#include "harness/console_protocol.h"
#include "bsp/esp-bsp.h"

#include "lvgl.h"

/* 候选原点：0/3/6/9。步长 3 = 夹逼区间 [1,6] 的分辨率，同时保证两条
 * 相邻色带之间还留 1px 黑缝，肉眼分得开。 */
static const struct { int16_t k; uint32_t rgb; const char *name; } CAND[] = {
    { 0, 0xFF3020, "red"    },
    { 3, 0x30FF40, "green"  },
    { 6, 0x40A0FF, "blue"   },
    { 9, 0xFFFFFF, "white"  },
};
#define CAND_N (sizeof(CAND) / sizeof(CAND[0]))
#define CALIB_W 2      /* 边框粗细 */

static lv_obj_t *s_box[CAND_N];

static void calib_build(void)
{
    if (s_box[0]) return;
    for (unsigned i = 0; i < CAND_N; ++i) {
        lv_obj_t *o = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(o);
        /* 尺寸恒为可见区大小；变的只是原点——这正是被测量的那个未知数。 */
        lv_obj_set_size(o, UI_VIS_W, UI_VIS_W);
        lv_obj_set_pos(o, CAND[i].k, CAND[i].k);
        lv_obj_set_style_radius(o, UI_VIS_RADIUS, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(CAND[i].rgb), 0);
        lv_obj_set_style_border_width(o, CALIB_W, 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        s_box[i] = o;
    }
}

void ui_calib_show(bool on)
{
    calib_build();
    for (unsigned i = 0; i < CAND_N; ++i) {
        if (on) lv_obj_clear_flag(s_box[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_box[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* console task 不在 LVGL task 上——建对象/改 flag 必须持显示锁。 */
static int cmd_vis(const console_args_t *args)
{
    bool on = (args->argc >= 2) && (args->argv[1][0] != '0');
    bsp_display_lock(-1);
    ui_calib_show(on);
    bsp_display_unlock();
    console_reply_ok("{\"calib\":%d,\"candidates\":\"0=red,3=green,6=blue,9=white\"}",
                     on ? 1 : 0);
    return 0;
}

static const console_cmd_t s_cmd_vis = { "?vis", cmd_vis,
    "visible-origin calibration frames: ?vis 1|0 "
    "(the colour forming a COMPLETE ring is the panel origin)" };

void ui_calib_init(void)
{
    console_protocol_register(&s_cmd_vis);
}
