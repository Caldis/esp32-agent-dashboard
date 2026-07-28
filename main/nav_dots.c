/*
 * nav_dots — 见 nav_dots.h。
 */

#include "nav_dots.h"
#include "theme.h"

#include <string.h>

#include "lvgl.h"

#define SCREEN_W     466
#define DOT_N          3
#define DOT_D          8      /* 暗态直径 */
#define DOT_D_ON      10      /* 亮态略大一点，颜色之外再给一个形状线索 */
/* 三个键在屏幕水平方向的 20% / 50% / 80%，即距中心 -140 / 0 / +140
 * （3/10 屏宽）。点位直接对应物理按键的位置，所以间距不是审美选择，
 * 是映射关系——拉宽后三点的分布更接近手指在机身上的实际间距。
 * 边界检查：左点圆心 x=94，墨迹约 89..99。面板是圆角矩形，圆角半径 RING_RADIUS(60)，
 * x>60 的位置整条高度都可用，所以 y=8 处安全。 */
#define DOT_GAP      ((SCREEN_W * 3) / 10)
/* 尽可能上移：让出中间的连接状态指示器（conn_dot，y 24..40）。点在
 * 8..18，与它有 6px 净距。 */
#define DOT_Y          8

#define OPA_OFF       70
#define OPA_ON       255

/* 布局占用（v6.6）
 * 顶部这条带此前只有 status_bar 的 conn_dot（TOP_MID, y=24）。三个指示点
 * 要的正是同一段，中间那点会与它重叠，所以 conn_dot 被挪到同排偏左
 * （status_bar.c 的 CONN_DOT_DX）。
 * 纵向不会撞到时钟：顶钟在 y=56 起（48px），指示点 22..32 收在它上面；
 * 天气场景的地名/角钟都在 y>=64。
 * 横向 ±26 的跨度很窄，即使面板按圆形算（y=22 处半宽约 100px）也安全。 */
static const char *DOT_SCENE[DOT_N] = { "dashboard", "weather", "clock" };

static lv_obj_t *s_dot[DOT_N];
static int       s_active = -1;

static void style_dot(int i, bool on)
{
    const theme_palette_t *pal = theme_current();
    int d = on ? DOT_D_ON : DOT_D;
    lv_obj_set_size(s_dot[i], d, d);
    /* TOP_MID 锚的是上边缘，所以尺寸一变圆心就会下移。把差值的一半补回
     * y，让点【原地】长大——一排点里 1px 的上下错位是看得见的。 */
    lv_obj_align(s_dot[i], LV_ALIGN_TOP_MID, (i - 1) * DOT_GAP,
                 DOT_Y + (DOT_D_ON - d) / 2);
    lv_obj_set_style_bg_color(s_dot[i],
        lv_color_hex(on ? (pal ? pal->accent_claude : 0x2BB3B1)
                        : (pal ? pal->text_dim : 0x8A807A)), 0);
    lv_obj_set_style_bg_opa(s_dot[i], on ? OPA_ON : OPA_OFF, 0);
}

void nav_dots_init(void)
{
    if (s_dot[0]) return;

    for (int i = 0; i < DOT_N; ++i) {
        s_dot[i] = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_dot[i]);
        lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_SCROLLABLE);
        style_dot(i, false);   /* 位置也在这里设（尺寸变化要补 y） */
    }
    s_active = -1;
}

void nav_dots_set_scene(const char *scene_id)
{
    if (!s_dot[0]) return;

    int want = -1;
    for (int i = 0; i < DOT_N; ++i) {
        if (scene_id && strcmp(scene_id, DOT_SCENE[i]) == 0) { want = i; break; }
    }
    if (want == s_active) return;      /* 去重：不重绘没变的东西 */

    if (s_active >= 0) style_dot(s_active, false);
    if (want    >= 0) style_dot(want, true);
    s_active = want;
}
