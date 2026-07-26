/*
 * scene_weather — 天气 + 时钟合体环境场景 (v4.9).
 *
 * BOOT 循环的第三站 (dashboard → overview → weather)。上下两区：
 *   · 时钟副区 — 顶部 48px 小钟 (直接复用 status_bar 的 time_lbl 槽位)
 *   · 天气主区 — 左侧 150px 线条插画 + 右侧 HERO 大温度/天气词，
 *     底部五天条带 (昨天 / 今天 / 明天 / 周X / 周X)
 *
 * 插画语言：复古未来主义线稿 (retro-futurism line art) — vaporwave 条纹
 * 太阳、弧线云、斜线雨、折线闪电、十字小星，细线 2-3px，amber 日光 +
 * 米白云雨 + teal 雨水/地平线三色，落在纯黑 AMOLED 上。全部由
 * lv_line / lv_arc / border-ring 矢量对象构成，无位图。
 *
 * 占比切换（"天气放大、时钟缩小"）：整点/15/30/45 的刻钟时刻起 30 秒
 * (由主机同步时钟推导的声明式状态机: min%15==0 && sec<30)，时钟放大 —
 * plan-B 零缩放变形（CLAUDE.md 红线：大 tiny_ttf 标签严禁每帧
 * size/transform_scale/widget-opa 动画）：
 *   · 顶部小钟 text_opa 淡出，135px 大钟从小钟槽位滑下 + text_opa 淡入
 *     (与 scene_clock 的 entrance/retreat 同一手法，实测满帧率)；
 *   · 天气主区整体 y 下沉 24px + 全子树逐对象 opa 淡出 — 过渡开始时
 *     递归快照每个对象的 text/line/arc/border/bg opa 作为基准值，
 *     动画只乘一个 0..255 因子，个体明暗层次（昨天列减淡等）不丢失。
 * 30 秒后反向。motion_reduced 直接跳到目标姿态。
 *
 * 天气数据由 bridge 经 `dash weather` 推送（昨天+今天+未来3天，
 * 见 agent_commands.c cmd_weather / tools/claude_buddy_bridge.py）。
 * 未收到数据时主区显示「等待天气数据」占位。
 */

#include "scenes.h"
#include "agent_state.h"
#include "status_bar.h"
#include "cjk_font.h"
#include "ui_type.h"
#include "anim/apple_ease.h"
#include "anim/spring.h"
#include "scene_trans.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#define SCREEN_W     466

/* ── palette (repo family colours) ─────────────────────────────────── */
#define COL_TEXT     0xF3EEE2   /* 米白 — 云 / 雪 / 文字 */
#define COL_DIM      0x8A807A   /* 次级 */
#define COL_MUTE     0x5A514A   /* 装饰 */
#define COL_TEAL     0x2BB3B1   /* 雨水 / 地平线 / 今天列 */
#define COL_SUN      0xE0A030   /* amber — 太阳 / 闪电 (同 conn amber 家族) */

/* ── weather-major 布局 ──────────────────────────────────────────────
 * 本场景隐藏 footer chrome (active/tokens — agent 域信息，dashboard/
 * overview 已有)：环境挂钟定位下砍掉一个层级，把 60px 全部还给留白。
 * 纵向节奏：chrome(112) →26→ 当前组(插画140 ∥ 温度+词) →31→ 五日条带
 * →48→ 屏底。对齐：插画顶=温度行框顶、插画底≈词墨底（两列成块）；
 * 五列在 28px 页边距内均分（列距 82）。 */
#define PAGE_MARGIN   36        /* 页左右边距（条带用 28） */
#define LOC_Y         70        /* 左上角地点标签，与顶钟光学同行 */
#define ILLUS_X       36        /* 大插画区 */
#define ILLUS_Y      138
#define ILLUS_SZ     140
#define TEMP_DX       74        /* 右列中心 = 插画右缘..右边距的中点 */
#define TEMP_Y       136        /* HERO 88, 行框 136..242 — 顶对齐插画 */
#define COND_Y       232        /* BODY 36, 墨底 ≈ 插画底 */
#define STRIP_NAME_Y 306        /* 五天条带三行 */
#define STRIP_ICON_Y 340
#define STRIP_ICON_SZ 44
#define STRIP_TEMP_Y 392
static const int STRIP_DX[WEATHER_DAYS] = { -164, -82, 0, 82, 164 };

/* ── clock-major 布局 ──────────────────────────────────────────────── */
#define CLOCK_PX     135
/* 与 scene_clock 相同的三标签边贴边偏移: (2×660+286)/2 × px / 1000 */
#define COLON_DX     (((1320 + 286) / 2 * CLOCK_PX) / 1000)
#define FACE_GRP_W   SCREEN_W
#define FACE_GRP_H   320
#define FACE_Y        30        /* 大钟数字在 face_grp 内的 y */
#define DATE_Y       225        /* "7月25日 周六" */
#define MINI_Y       272        /* "31° 多云 · 深圳·福田" */
/* 大钟墨迹中心(rest) ≈ 屏幕 184；退到顶部小钟槽位中心 85 → 偏移 -99 */
#define ENTRY_Y      (-99)

/* ── 动效时序 ──────────────────────────────────────────────────────── */
#define TICK_MS       500
#define MORPH_OUT_MS  440       /* 退场 (weather 淡出 / face 退回) */
#define MORPH_IN_MS   550       /* 入场 */
#define MORPH_LAG_MS   90       /* 入场比退场晚一拍，先清场再登台 */
#define WX_SINK_PX     24       /* 天气组淡出时的下沉距离 */
#define BLINK_MS     1000       /* 大钟冒号 0.5Hz 呼吸闪 */
#define QUARTER_HOLD_S 30       /* 刻钟放大保持秒数 */
#define BREATH_MS    3000       /* 插画 accent 呼吸周期 (16 步进) */

#define MODE_WEATHER 0
#define MODE_CLOCK   1

/* ── 矢量图标 ──────────────────────────────────────────────────────── */
/* lv_line 不拷贝点数组 — 每个图标槽自带持久点存储。 */
#define ICON_MAX_PTS 48

typedef struct {
    lv_obj_t          *root;
    lv_point_precise_t pts[ICON_MAX_PTS];
    int                pt_used;
} wx_icon_t;

/* 过渡淡化表。每项 = 对象 + 样式通道 + 权威基准 opa；动画因子 0..255
 * 乘基准回写。基准 NEVER 从屏幕当前值快照——0 是 base×f/255 的吸收态，
 * 任何时序让快照落在透明瞬间对象就永久锁死（首测踩过）。权威值在对象
 * 创建时编码进 user_data（wx_mark），表重建只读 user_data，屏幕值只是
 * 输出。图标重建只发生在过渡窗口外 (trans_until_ms 门)，表内指针在
 * 动画期内恒有效。 */
#define FADE_MAX 96
enum { FADE_TEXT, FADE_LINE, FADE_ARC, FADE_BORDER, FADE_BG };

typedef struct {
    lv_obj_t *obj;
    uint8_t   ch;      /* FADE_* */
    uint8_t   base;    /* 权威 opa（来自 wx_mark） */
} fade_ent_t;

/* 把 (通道, 权威 opa) 编进 user_data：0x10000 标志位 | ch<<8 | base。
 * 只有带标志的对象进 fade 表；容器等自然被跳过。 */
#define WX_MARK_FLAG 0x10000u

static void wx_mark(lv_obj_t *o, uint8_t ch, uint8_t base)
{
    lv_obj_set_user_data(o,
        (void *)(uintptr_t)(WX_MARK_FLAG | ((uint32_t)ch << 8) | base));
}

typedef struct {
    status_bar_t sb;

    /* 天气主区 */
    lv_obj_t  *wx_grp;
    lv_obj_t  *loc_lbl;                     /* 左上角 "深圳·福田" */
    lv_obj_t  *temp_lbl;                    /* HERO "31°" */
    lv_obj_t  *cond_lbl;                    /* BODY "多云" */
    lv_obj_t  *wait_lbl;                    /* 占位 "等待天气数据" */
    wx_icon_t  big_icon;
    wx_icon_t  day_icons[WEATHER_DAYS];
    lv_obj_t  *day_name[WEATHER_DAYS];
    lv_obj_t  *day_temp[WEATHER_DAYS];

    /* 动画 accent（仅大插画）— tick 低频步进 opa，16 步/3s 周期。
     * 每条带相位偏移 + 波形模式：BREATH=三角波呼吸（射线旋转闪烁、
     * 雨丝流动、雪花交替、雾线波动），FLASH=爆闪（闪电：短亮长暗）。
     * 写回通道由对象 wx_mark 里的 FADE_* 决定（弧线云也能呼吸）。 */
    struct {
        lv_obj_t *o;
        uint8_t   base;
        uint8_t   phase;    /* 0..15，步进相位偏移 */
        uint8_t   mode;     /* ACC_* */
    } accent[12];
    int        accent_n;
    int        breath_step;

    /* 时钟主区 */
    lv_obj_t  *face_grp;
    lv_obj_t  *hh, *colon, *mm;
    lv_obj_t  *date_lbl, *mini_lbl;

    /* 过渡 */
    fade_ent_t fade[FADE_MAX];
    int        fade_n;
    uint32_t   trans_until_ms;              /* 过渡窗口右沿 (lv_tick) */

    lv_timer_t *timer;
    int        mode;                        /* 目标态 MODE_* */
    bool       motion_ok;
    uint32_t   mode_since_ms;
    uint32_t   wx_stamp;                    /* 已渲染的 weather.received_ms */
    bool       wx_drawn;                    /* 已画过一次真实数据 */
    /* v5.1 时间锚点变形：右上角小钟 (26px) ↔ 共识姿态 (48px 顶部中央)
     * 的同实体位移+档位缩放。 */
    int        wtime_px;
    int32_t    wtime_v;       /* 0=rest(右上26) .. 1000=consensus(中央48) */

    /* 缓存与各自 compose 缓冲 1:1 同宽 — snprintf("%s") 在 -Werror
     * format-truncation 下必须能证明放得下。 */
    char       cached_hhmm[16];
    char       cached_date[64];
    char       cached_mini[96];
    int        colon_on;
} weather_scene_t;

/* ════ 小工具 ═══════════════════════════════════════════════════════ */

static lv_obj_t *mk_box(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, w, h);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    return g;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

/* ════ 复古未来线稿图标 ═════════════════════════════════════════════ */

typedef enum {
    WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_RAIN, WX_SNOW, WX_THUNDER
} wx_class_t;

static wx_class_t wx_classify(int code)
{
    if (code <= 1)                                    return WX_CLEAR;
    if (code == 2)                                    return WX_PARTLY;
    if (code == 3)                                    return WX_CLOUDY;
    if (code == 45 || code == 48)                     return WX_FOG;
    if (code >= 95)                                   return WX_THUNDER;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
                                                      return WX_SNOW;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
                                                      return WX_RAIN;
    return WX_CLOUDY;
}

/* 天气词（全部 GB2312，设备字体子集内） */
static const char *wx_word(int code)
{
    switch (wx_classify(code)) {
    case WX_CLEAR:   return "晴";
    case WX_PARTLY:  return "多云";
    case WX_CLOUDY:  return "阴";
    case WX_FOG:     return "雾";
    case WX_SNOW:
        if (code == 85 || code == 86) return "阵雪";
        return (code == 71) ? "小雪" : (code == 73) ? "中雪" : "大雪";
    case WX_THUNDER: return (code == 95) ? "雷阵雨" : "雷暴";
    case WX_RAIN:
        if (code >= 51 && code <= 57) return "毛毛雨";
        if (code == 80 || code == 81) return "阵雨";
        if (code == 61)               return "小雨";
        if (code == 63)               return "中雨";
        return "大雨";
    }
    return "多云";
}

static const char *WDAY_NAME[7] = { "周日", "周一", "周二", "周三",
                                    "周四", "周五", "周六" };

static lv_point_precise_t *icon_pts(wx_icon_t *ic, int n)
{
    if (ic->pt_used + n > ICON_MAX_PTS) return NULL;
    lv_point_precise_t *p = &ic->pts[ic->pt_used];
    ic->pt_used += n;
    return p;
}

/* opa 缩放：dim=255 原样；昨天列传 ~115 整体减淡。 */
static uint8_t sc_opa(uint8_t opa, uint8_t dim)
{
    return (uint8_t)(((int)opa * dim) / 255);
}

/* 折线：xy 为 100 单位设计空间的点对数组，按 size 缩放。 */
static lv_obj_t *ic_polyline(wx_icon_t *ic, const int *xy, int n_pts, int size,
                             uint32_t col, int w, uint8_t opa)
{
    lv_point_precise_t *p = icon_pts(ic, n_pts);
    if (!p) return NULL;
    for (int i = 0; i < n_pts; ++i) {
        p[i].x = xy[2 * i]     * size / 100;
        p[i].y = xy[2 * i + 1] * size / 100;
    }
    lv_obj_t *ln = lv_line_create(ic->root);
    lv_line_set_points(ln, p, n_pts);
    lv_obj_set_style_line_color(ln, lv_color_hex(col), 0);
    lv_obj_set_style_line_width(ln, w, 0);
    lv_obj_set_style_line_opa(ln, opa, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    wx_mark(ln, FADE_LINE, opa);
    return ln;
}

static lv_obj_t *ic_seg(wx_icon_t *ic, int x0, int y0, int x1, int y1, int size,
                        uint32_t col, int w, uint8_t opa)
{
    int xy[4] = { x0, y0, x1, y1 };
    return ic_polyline(ic, xy, 2, size, col, w, opa);
}

/* 整圆轮廓 — border 圆比 0..360 arc 便宜。 */
static lv_obj_t *ic_ring(wx_icon_t *ic, int cx, int cy, int r, int size,
                         uint32_t col, int w, uint8_t opa)
{
    int R = r * size / 100;
    lv_obj_t *o = mk_box(ic->root, 2 * R, 2 * R);
    lv_obj_set_pos(o, cx * size / 100 - R, cy * size / 100 - R);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(o, w, 0);
    lv_obj_set_style_border_opa(o, opa, 0);
    wx_mark(o, FADE_BORDER, opa);
    return o;
}

/* 圆弧段。LVGL 角度：0°=3点钟方向，顺时针增。 */
static lv_obj_t *ic_arc(wx_icon_t *ic, int cx, int cy, int r, int a0, int a1,
                        int size, uint32_t col, int w, uint8_t opa)
{
    int R = r * size / 100;
    lv_obj_t *a = lv_arc_create(ic->root);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, 2 * R, 2 * R);
    lv_obj_set_pos(a, cx * size / 100 - R, cy * size / 100 - R);
    lv_arc_set_bg_angles(a, (lv_value_precise_t)a0, (lv_value_precise_t)a1);
    lv_obj_set_style_arc_color(a, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    wx_mark(a, FADE_ARC, opa);
    return a;
}

enum { ACC_BREATH = 0, ACC_FLASH };

/* 记一个动画 accent（仅大插画）。 */
static void note_accent(weather_scene_t *sc, lv_obj_t *o, uint8_t base,
                        uint8_t phase, uint8_t mode)
{
    if (!o || !sc || sc->accent_n >= (int)(sizeof sc->accent / sizeof sc->accent[0]))
        return;
    sc->accent[sc->accent_n].o = o;
    sc->accent[sc->accent_n].base = base;
    sc->accent[sc->accent_n].phase = phase;
    sc->accent[sc->accent_n].mode = mode;
    sc->accent_n++;
}

/* 按对象 wx_mark 里的通道写 opa（accent 步进用——线/弧/描边通用）。 */
static void wx_set_opa_by_mark(lv_obj_t *o, lv_opa_t v)
{
    uintptr_t ud = (uintptr_t)lv_obj_get_user_data(o);
    if (!(ud & WX_MARK_FLAG)) return;
    switch ((ud >> 8) & 0xFF) {
    case FADE_LINE:   lv_obj_set_style_line_opa(o, v, LV_PART_MAIN);   break;
    case FADE_ARC:    lv_obj_set_style_arc_opa(o, v, LV_PART_MAIN);    break;
    case FADE_BORDER: lv_obj_set_style_border_opa(o, v, LV_PART_MAIN); break;
    default: break;
    }
}

/* 云朵轮廓：左右两个顶弧 + 底线 — 开放式线稿。cx/cy/r 为设计单位。 */
static void ic_cloud(wx_icon_t *ic, int cx, int cy, int r, int size,
                     uint32_t col, int w, uint8_t opa)
{
    ic_arc(ic, cx - r * 55 / 100, cy,             r * 58 / 100, 140, 330,
           size, col, w, opa);
    ic_arc(ic, cx + r * 35 / 100, cy - r * 30 / 100, r * 78 / 100, 210,  40,
           size, col, w, opa);
    ic_seg(ic, cx - r * 115 / 100, cy + r * 60 / 100,
               cx + r * 115 / 100, cy + r * 60 / 100, size, col, w, opa);
}

/* 复古条纹太阳：外圈 + 下半内部条纹 + 长短相间射线。 */
static void ic_retro_sun(weather_scene_t *sc, wx_icon_t *ic, int cx, int cy,
                         int r, int size, uint8_t dim, bool is_big)
{
    int w = is_big ? 3 : 2;
    ic_ring(ic, cx, cy, r, size, COL_SUN, w, sc_opa(230, dim));

    lv_obj_t *o;
    if (is_big) {
        /* 下半条纹 (vaporwave 签名)：三条弦线，向下渐淡 + 慢速流光。 */
        o = ic_seg(ic, cx - r * 85 / 100, cy + r * 30 / 100,
                   cx + r * 85 / 100, cy + r * 30 / 100, size, COL_SUN, 2,
                   sc_opa(190, dim));
        note_accent(sc, o, 190, 1, ACC_BREATH);
        o = ic_seg(ic, cx - r * 70 / 100, cy + r * 55 / 100,
                   cx + r * 70 / 100, cy + r * 55 / 100, size, COL_SUN, 2,
                   sc_opa(150, dim));
        note_accent(sc, o, 150, 4, ACC_BREATH);
        o = ic_seg(ic, cx - r * 48 / 100, cy + r * 78 / 100,
                   cx + r * 48 / 100, cy + r * 78 / 100, size, COL_SUN, 2,
                   sc_opa(110, dim));
        note_accent(sc, o, 110, 7, ACC_BREATH);
    }

    /* 射线：四正向长、四斜向短（小图标只画四正向）。相位按顺时针
     * 每根错开，呼吸叠出"旋转闪烁"的复古霓虹感。 */
    int r1 = r * 130 / 100, r2 = r * 170 / 100, r3 = r * 155 / 100;
    o = ic_seg(ic, cx + r1, cy, cx + r2, cy, size, COL_SUN, w, sc_opa(230, dim));
    if (is_big) note_accent(sc, o, 230, 0, ACC_BREATH);
    o = ic_seg(ic, cx - r1, cy, cx - r2, cy, size, COL_SUN, w, sc_opa(230, dim));
    if (is_big) note_accent(sc, o, 230, 8, ACC_BREATH);
    o = ic_seg(ic, cx, cy - r1, cx, cy - r2, size, COL_SUN, w, sc_opa(230, dim));
    if (is_big) note_accent(sc, o, 230, 12, ACC_BREATH);
    o = ic_seg(ic, cx, cy + r1, cx, cy + r2, size, COL_SUN, w, sc_opa(230, dim));
    if (is_big) note_accent(sc, o, 230, 4, ACC_BREATH);
    if (is_big) {
        int d1 = r1 * 707 / 1000, d2 = r3 * 707 / 1000;
        o = ic_seg(ic, cx + d1, cy - d1, cx + d2, cy - d2, size, COL_SUN, 2, sc_opa(180, dim));
        note_accent(sc, o, 180, 14, ACC_BREATH);
        o = ic_seg(ic, cx - d1, cy - d1, cx - d2, cy - d2, size, COL_SUN, 2, sc_opa(180, dim));
        note_accent(sc, o, 180, 10, ACC_BREATH);
        o = ic_seg(ic, cx + d1, cy + d1, cx + d2, cy + d2, size, COL_SUN, 2, sc_opa(180, dim));
        note_accent(sc, o, 180, 2, ACC_BREATH);
        o = ic_seg(ic, cx - d1, cy + d1, cx - d2, cy + d2, size, COL_SUN, 2, sc_opa(180, dim));
        note_accent(sc, o, 180, 6, ACC_BREATH);
    }
}

/* 重建一个图标槽。sc 仅大图标需要（accent 登记）；小图标传 NULL。 */
static void wx_icon_build(weather_scene_t *sc, wx_icon_t *ic, int code,
                          int size, uint8_t dim)
{
    bool is_big = (sc != NULL);
    lv_obj_clean(ic->root);
    ic->pt_used = 0;
    if (is_big) sc->accent_n = 0;

    int w  = is_big ? 3 : 2;
    lv_obj_t *o;

    switch (wx_classify(code)) {
    case WX_CLEAR:
        ic_retro_sun(sc, ic, 50, 44, 20, size, dim, is_big);
        break;

    case WX_PARTLY:
        /* 小太阳缩在右上，云在左前。 */
        if (is_big) {
            ic_retro_sun(sc, ic, 64, 32, 13, size, dim, true);
        } else {
            ic_ring(ic, 64, 32, 14, size, COL_SUN, w, sc_opa(230, dim));
            ic_seg(ic, 64, 10, 64, 4, size, COL_SUN, w, sc_opa(220, dim));
            ic_seg(ic, 86, 32, 92, 32, size, COL_SUN, w, sc_opa(220, dim));
        }
        ic_cloud(ic, 40, 62, 16, size, COL_TEXT, w, sc_opa(240, dim));
        break;

    case WX_CLOUDY:
        if (is_big) {
            /* 后云缓慢呼吸——阴天的"云层流动"暗示。 */
            uint32_t before = lv_obj_get_child_count(ic->root);
            ic_cloud(ic, 66, 36, 13, size, COL_DIM, 2, sc_opa(200, dim));
            uint32_t after = lv_obj_get_child_count(ic->root);
            for (uint32_t k = before; k < after; ++k)
                note_accent(sc, lv_obj_get_child(ic->root, (int32_t)k),
                            200, 0, ACC_BREATH);
        }
        ic_cloud(ic, 46, 60, 22, size, COL_TEXT, w, sc_opa(240, dim));
        break;

    case WX_FOG:
        ic_cloud(ic, 50, 42, 17, size, COL_DIM, w, sc_opa(220, dim));
        /* 雾线相位错开——层层流动。 */
        o = ic_seg(ic, 26, 74, 74, 74, size, COL_TEXT, 2, sc_opa(150, dim));
        if (is_big) note_accent(sc, o, 150, 0, ACC_BREATH);
        o = ic_seg(ic, 32, 84, 68, 84, size, COL_TEXT, 2, sc_opa(115, dim));
        if (is_big) note_accent(sc, o, 115, 5, ACC_BREATH);
        if (is_big) {
            o = ic_seg(ic, 40, 94, 60, 94, size, COL_TEXT, 2, sc_opa(85, dim));
            note_accent(sc, o, 85, 10, ACC_BREATH);
        }
        break;

    case WX_RAIN:
        ic_cloud(ic, 50, 42, 17, size, COL_TEXT, w, sc_opa(240, dim));
        /* 雨丝相位错开呼吸——雨幕的下落流动感。 */
        o = ic_seg(ic, 36, 70, 30, 84, size, COL_TEAL, w, sc_opa(220, dim));
        if (is_big) note_accent(sc, o, 220, 0, ACC_BREATH);
        o = ic_seg(ic, 52, 70, 46, 84, size, COL_TEAL, w, sc_opa(220, dim));
        if (is_big) note_accent(sc, o, 220, 6, ACC_BREATH);
        o = ic_seg(ic, 68, 70, 62, 84, size, COL_TEAL, w, sc_opa(220, dim));
        if (is_big) note_accent(sc, o, 220, 11, ACC_BREATH);
        if (is_big) {
            o = ic_seg(ic, 44, 88, 40, 97, size, COL_TEAL, 2, sc_opa(150, dim));
            note_accent(sc, o, 150, 3, ACC_BREATH);
            o = ic_seg(ic, 60, 88, 56, 97, size, COL_TEAL, 2, sc_opa(150, dim));
            note_accent(sc, o, 150, 9, ACC_BREATH);
        }
        break;

    case WX_SNOW:
        ic_cloud(ic, 50, 42, 17, size, COL_TEXT, w, sc_opa(240, dim));
        {
            /* 三朵雪花按朵错相位——纷落闪烁。 */
            static const int fx[3] = { 36, 54, 68 };
            static const int fy[3] = { 78, 88, 76 };
            static const uint8_t fp[3] = { 0, 6, 11 };
            int n = is_big ? 3 : 2;
            for (int i = 0; i < n; ++i) {
                o = ic_seg(ic, fx[i] - 5, fy[i], fx[i] + 5, fy[i], size,
                           COL_TEXT, 2, sc_opa(200, dim));
                if (is_big) note_accent(sc, o, 200, fp[i], ACC_BREATH);
                o = ic_seg(ic, fx[i], fy[i] - 5, fx[i], fy[i] + 5, size,
                           COL_TEXT, 2, sc_opa(200, dim));
                if (is_big) note_accent(sc, o, 200, fp[i], ACC_BREATH);
            }
        }
        break;

    case WX_THUNDER:
        ic_cloud(ic, 50, 40, 17, size, COL_TEXT, w, sc_opa(240, dim));
        {
            /* 闪电走爆闪波形：短亮长暗，真实的"打闪"节奏。 */
            int bolt[8] = { 54, 62, 42, 79, 52, 79, 40, 97 };
            o = ic_polyline(ic, bolt, 4, size, COL_SUN, w, sc_opa(240, dim));
            if (is_big) note_accent(sc, o, 240, 0, ACC_FLASH);
        }
        if (is_big) {
            o = ic_seg(ic, 28, 66, 24, 76, size, COL_TEAL, 2, sc_opa(160, dim));
            note_accent(sc, o, 160, 5, ACC_BREATH);
            o = ic_seg(ic, 72, 64, 68, 74, size, COL_TEAL, 2, sc_opa(160, dim));
            note_accent(sc, o, 160, 11, ACC_BREATH);
        }
        break;
    }

    /* 大插画的复古舞台：科技地平线 + 网格刻度，图标"站"在线上。 */
    if (is_big) {
        ic_seg(ic, 2, 97, 98, 97, size, COL_TEAL, 2, 140);
        for (int i = 0; i < 5; ++i) {
            int x = 10 + i * 20;
            ic_seg(ic, x, 97, x, 100, size, COL_TEAL, 2, 90);
        }
    }
}

/* 装饰小星（十字线），散布在留白处。独立于图标槽，用 wx_grp 级点存储。 */
static lv_point_precise_t s_star_pts[12];

static void mk_star(lv_obj_t *parent, int idx, int cx, int cy, int arm,
                    uint8_t opa)
{
    lv_point_precise_t *p = &s_star_pts[idx * 4];
    p[0].x = cx - arm; p[0].y = cy;
    p[1].x = cx + arm; p[1].y = cy;
    p[2].x = cx;       p[2].y = cy - arm;
    p[3].x = cx;       p[3].y = cy + arm;
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *ln = lv_line_create(parent);
        lv_line_set_points(ln, &p[i * 2], 2);
        lv_obj_set_style_line_color(ln, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_line_width(ln, 2, 0);
        lv_obj_set_style_line_opa(ln, opa, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        wx_mark(ln, FADE_LINE, opa);
    }
}

/* ════ 过渡淡化表 ═══════════════════════════════════════════════════ */

static void fade_walk(weather_scene_t *st, lv_obj_t *o)
{
    uintptr_t ud = (uintptr_t)lv_obj_get_user_data(o);
    if ((ud & WX_MARK_FLAG) && st->fade_n < FADE_MAX) {
        st->fade[st->fade_n++] = (fade_ent_t){
            .obj = o, .ch = (uint8_t)((ud >> 8) & 0xFF),
            .base = (uint8_t)(ud & 0xFF),
        };
    }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; ++i)
        fade_walk(st, lv_obj_get_child(o, i));
}

/* 重建指针表（init 末尾 / render_weather 末尾 — DOM 变了就要重建）。
 * base 全部来自 wx_mark 的权威值，与屏幕当前透明度无关，所以重建
 * 时机对"亮度正确性"不再敏感——只对指针有效性敏感。 */
static void fade_rebase(weather_scene_t *st)
{
    st->fade_n = 0;
    fade_walk(st, st->wx_grp);
}

static void fade_apply(weather_scene_t *st, uint8_t f)
{
    for (int i = 0; i < st->fade_n; ++i) {
        fade_ent_t *e = &st->fade[i];
        lv_opa_t v = (lv_opa_t)(((int)e->base * f) / 255);
        switch (e->ch) {
        case FADE_TEXT:   lv_obj_set_style_text_opa(e->obj, v, LV_PART_MAIN);   break;
        case FADE_LINE:   lv_obj_set_style_line_opa(e->obj, v, LV_PART_MAIN);   break;
        case FADE_ARC:    lv_obj_set_style_arc_opa(e->obj, v, LV_PART_MAIN);    break;
        case FADE_BORDER: lv_obj_set_style_border_opa(e->obj, v, LV_PART_MAIN); break;
        case FADE_BG:     lv_obj_set_style_bg_opa(e->obj, v, LV_PART_MAIN);     break;
        }
    }
}

/* ── anim exec cbs ─────────────────────────────────────────────────── */

static void anim_wx_fade_out(void *var, int32_t v)
{
    weather_scene_t *st = (weather_scene_t *)var;
    fade_apply(st, (uint8_t)v);
    if (v == 0) lv_obj_add_flag(st->wx_grp, LV_OBJ_FLAG_HIDDEN);
}

static void anim_wx_fade_in(void *var, int32_t v)
{
    fade_apply((weather_scene_t *)var, (uint8_t)v);
}

static void anim_obj_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/* face_grp 全子标签 text_opa（同 scene_clock 的 set_group_text_opa）。 */
static void face_set_opa(lv_obj_t *grp, lv_opa_t v)
{
    uint32_t n = lv_obj_get_child_count(grp);
    for (uint32_t i = 0; i < n; ++i)
        lv_obj_set_style_text_opa(lv_obj_get_child(grp, i), v, 0);
}

static void anim_face_opa(void *obj, int32_t v)
{
    face_set_opa((lv_obj_t *)obj, (lv_opa_t)v);
}

static void anim_face_opa_out(void *obj, int32_t v)
{
    face_set_opa((lv_obj_t *)obj, (lv_opa_t)v);
    if (v == 0) lv_obj_add_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
}

static void anim_topclock_opa(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* ════ 模式切换（plan-B 零缩放变形） ════════════════════════════════ */

static void kill_morph_anims(weather_scene_t *st)
{
    lv_anim_delete(st, anim_wx_fade_out);
    lv_anim_delete(st, anim_wx_fade_in);
    lv_anim_delete(st->wx_grp, anim_obj_y);
    lv_anim_delete(st->face_grp, anim_obj_y);
    lv_anim_delete(st->face_grp, anim_face_opa);
    lv_anim_delete(st->face_grp, anim_face_opa_out);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_opa);
}

/* 直接摆到目标姿态（motion_reduced / on_show / on_hide 用）。 */
static void pose_jump(weather_scene_t *st, int mode)
{
    kill_morph_anims(st);
    if (mode == MODE_CLOCK) {
        fade_apply(st, 0);
        lv_obj_add_flag(st->wx_grp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(st->wx_grp, WX_SINK_PX);
        lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(st->face_grp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(st->face_grp, 0);
        face_set_opa(st->face_grp, LV_OPA_COVER);
    } else {
        lv_obj_add_flag(st->face_grp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(st->face_grp, ENTRY_Y);
        lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_COVER, 0);
        lv_obj_clear_flag(st->wx_grp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(st->wx_grp, 0);
        if (st->fade_n) fade_apply(st, 255);
    }
    st->trans_until_ms = lv_tick_get();
}

static void start_anim(void *var, lv_anim_exec_xcb_t cb, int32_t from,
                       int32_t to, uint32_t ms, uint32_t delay, bool ease_out)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_time(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, ease_out ? apple_ease_out : apple_ease_in);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_start(&a);
}

/* 切换到目标模式。时钟放大 = 天气淡出下沉 + 小钟淡出 + 大钟滑入；
 * 反向同理。fade 表在动画前快照（rebuild 由 trans_until_ms 挡在窗口外）。 */
static void go_mode(weather_scene_t *st, int mode)
{
    st->mode = mode;
    st->mode_since_ms = lv_tick_get();
    st->colon_on = -1;
    st->cached_hhmm[0] = '\0';   /* 强制大钟首 tick 重写 */

    if (!st->motion_ok) {
        pose_jump(st, mode);
        return;
    }
    kill_morph_anims(st);
    st->trans_until_ms = lv_tick_get() + MORPH_LAG_MS + MORPH_IN_MS + 50;

    if (mode == MODE_CLOCK) {
        /* 天气退场 */
        start_anim(st, anim_wx_fade_out, 255, 0, MORPH_OUT_MS, 0, false);
        start_anim(st->wx_grp, anim_obj_y, lv_obj_get_y(st->wx_grp),
                   WX_SINK_PX, MORPH_OUT_MS, 0, false);
        /* 小钟让位 */
        start_anim(st->sb.time_lbl, anim_topclock_opa, LV_OPA_COVER,
                   LV_OPA_TRANSP, MORPH_OUT_MS, 0, false);
        /* 大钟从小钟槽位滑入中央 */
        lv_obj_clear_flag(st->face_grp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(st->face_grp, ENTRY_Y);
        face_set_opa(st->face_grp, LV_OPA_TRANSP);
        start_anim(st->face_grp, anim_obj_y, ENTRY_Y, 0,
                   MORPH_IN_MS, MORPH_LAG_MS, true);
        start_anim(st->face_grp, anim_face_opa, LV_OPA_TRANSP, LV_OPA_COVER,
                   MORPH_IN_MS, MORPH_LAG_MS, true);
    } else {
        /* 大钟退回小钟槽位 */
        start_anim(st->face_grp, anim_obj_y, lv_obj_get_y(st->face_grp),
                   ENTRY_Y, MORPH_OUT_MS, 0, false);
        start_anim(st->face_grp, anim_face_opa_out, LV_OPA_COVER,
                   LV_OPA_TRANSP, MORPH_OUT_MS, 0, false);
        start_anim(st->sb.time_lbl, anim_topclock_opa, LV_OPA_TRANSP,
                   LV_OPA_COVER, MORPH_IN_MS, MORPH_LAG_MS, true);
        /* 天气登台 */
        lv_obj_clear_flag(st->wx_grp, LV_OBJ_FLAG_HIDDEN);
        start_anim(st, anim_wx_fade_in, 0, 255, MORPH_IN_MS, MORPH_LAG_MS, true);
        start_anim(st->wx_grp, anim_obj_y, lv_obj_get_y(st->wx_grp), 0,
                   MORPH_IN_MS, MORPH_LAG_MS, true);
    }
}

/* ════ 时间推导 ═════════════════════════════════════════════════════ */

/* 主机本地时刻（epoch+tz 折算），返回 false = 未同步。锁内调用。 */
static bool local_now(const agent_state_t *s, uint32_t *out_tz_epoch)
{
    if (s->host_epoch_unix == 0) return false;
    uint32_t now = s->host_epoch_unix
                 + (lv_tick_get() - s->host_clock_received_ms) / 1000u;
    *out_tz_epoch = (uint32_t)((int32_t)now + s->host_tz_offset_seconds);
    return true;
}

static bool in_quarter_window(uint32_t tz_epoch)
{
    uint32_t sec_of_hour = tz_epoch % 3600u;
    return ((sec_of_hour / 60u) % 15u == 0u) && (sec_of_hour % 60u < QUARTER_HOLD_S);
}

/* ════ 内容刷新 ═════════════════════════════════════════════════════ */

static void render_weather(weather_scene_t *st, const weather_state_t *w)
{
    char buf[64];

    lv_obj_add_flag(st->wait_lbl, LV_OBJ_FLAG_HIDDEN);

    snprintf(buf, sizeof(buf), "%d°", (int)w->cur_temp);
    lv_label_set_text(st->temp_lbl, buf);
    lv_label_set_text(st->cond_lbl, wx_word(w->cur_code));
    lv_label_set_text(st->loc_lbl, w->loc[0] ? w->loc : "");
    /* 权威值同时落到屏幕——render 只可能在天气可见态跑（tick 门），
     * 顺手把任何历史透明残留修正掉。 */
    lv_obj_set_style_text_opa(st->temp_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(st->cond_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(st->loc_lbl, LV_OPA_COVER, 0);

    wx_icon_build(st, &st->big_icon, w->cur_code, ILLUS_SZ, 255);
    st->breath_step = -1;

    for (int i = 0; i < WEATHER_DAYS; ++i) {
        uint8_t dim = (i == 0) ? 140 : 255;      /* 昨天整列减淡 */
        const char *name = (i == 0) ? "昨天"
                         : (i == 1) ? "今天"
                         : (i == 2) ? "明天"
                         : WDAY_NAME[w->days[i].wday % 7];
        uint8_t lbl_opa = (dim == 255) ? 255 : 160;
        lv_label_set_text(st->day_name[i], name);
        lv_obj_set_style_text_color(st->day_name[i],
            lv_color_hex(i == 1 ? COL_TEAL : COL_DIM), 0);
        lv_obj_set_style_text_opa(st->day_name[i], lbl_opa, 0);
        wx_mark(st->day_name[i], FADE_TEXT, lbl_opa);

        /* 列内省略 °（大温度已表达单位）— 救回 14px 列宽。 */
        snprintf(buf, sizeof(buf), "%d/%d",
                 (int)w->days[i].t_lo, (int)w->days[i].t_hi);
        lv_label_set_text(st->day_temp[i], buf);
        lv_obj_set_style_text_opa(st->day_temp[i], lbl_opa, 0);
        wx_mark(st->day_temp[i], FADE_TEXT, lbl_opa);

        wx_icon_build(NULL, &st->day_icons[i], w->days[i].code,
                      STRIP_ICON_SZ, dim);
    }
    st->wx_drawn = true;
    /* DOM 重建完（旧表里的图标线条指针已失效）且全子树处于满亮基准态
     * — 这里是唯一合法的重建时机之二。 */
    fade_rebase(st);
}

/* 大钟下的日期行 + 迷你天气行。 */
static void render_clock_info(weather_scene_t *st, const agent_state_t *s,
                              const weather_state_t *w, uint32_t tz_epoch)
{
    char buf[64];
    time_t tt = (time_t)tz_epoch;
    struct tm tmv;
    gmtime_r(&tt, &tmv);
    snprintf(buf, sizeof(buf), "%d月%d日 %s",
             tmv.tm_mon + 1, tmv.tm_mday, WDAY_NAME[tmv.tm_wday % 7]);
    if (strcmp(buf, st->cached_date) != 0) {
        snprintf(st->cached_date, sizeof(st->cached_date), "%s", buf);
        lv_label_set_text(st->date_lbl, buf);
    }

    char mini[96] = "";
    if (w->valid) {
        if (w->loc[0])
            snprintf(mini, sizeof(mini), "%d° %s · %s",
                     (int)w->cur_temp, wx_word(w->cur_code), w->loc);
        else
            snprintf(mini, sizeof(mini), "%d° %s",
                     (int)w->cur_temp, wx_word(w->cur_code));
    }
    if (strcmp(mini, st->cached_mini) != 0) {
        snprintf(st->cached_mini, sizeof(st->cached_mini), "%s", mini);
        lv_label_set_text(st->mini_lbl, mini);
    }
    (void)s;
}

/* ════ tick ═════════════════════════════════════════════════════════ */

static void weather_tick(lv_timer_t *t)
{
    weather_scene_t *st = (weather_scene_t *)lv_timer_get_user_data(t);
    if (!st) return;

    weather_state_t wx;
    uint32_t tz_epoch = 0;
    bool synced;
    char hhmm[16];

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    status_bar_update(&st->sb, s);
    status_bar_format_time(hhmm, sizeof(hhmm), s);
    wx = s->weather;                       /* 结构体拷贝，锁外使用 */
    synced = local_now(s, &tz_epoch);
    st->motion_ok = !s->motion_reduced;
    agent_state_unlock();

    uint32_t now = lv_tick_get();
    bool in_trans = (int32_t)(now - st->trans_until_ms) < 0;

    /* 刻钟状态机：主机时间未同步时永远停在天气态。 */
    int want = (synced && in_quarter_window(tz_epoch)) ? MODE_CLOCK
                                                       : MODE_WEATHER;
    if (want != st->mode) go_mode(st, want);

    /* 天气内容刷新 — 只在天气态且过渡窗口外动 DOM（fade 表指针安全）。 */
    if (st->mode == MODE_WEATHER && !in_trans && wx.valid
        && (wx.received_ms != st->wx_stamp || !st->wx_drawn)) {
        st->wx_stamp = wx.received_ms;
        render_weather(st, &wx);
    }

    /* 大钟内容 */
    if (st->mode == MODE_CLOCK) {
        if (strcmp(hhmm, st->cached_hhmm) != 0) {
            snprintf(st->cached_hhmm, sizeof(st->cached_hhmm), "%s", hhmm);
            char b[3] = { hhmm[0], hhmm[1], '\0' };
            lv_label_set_text(st->hh, b);
            b[0] = hhmm[3]; b[1] = hhmm[4];
            lv_label_set_text(st->mm, b);
        }
        if (synced) render_clock_info(st, s, &wx, tz_epoch);

        /* 冒号 0.5Hz 闪 — 入场动画结束后接管，逐次去重。 */
        uint32_t elapsed = now - st->mode_since_ms;
        if (elapsed >= MORPH_LAG_MS + MORPH_IN_MS) {
            bool blink = st->motion_ok && (hhmm[0] != '-');
            int on = (!blink || ((elapsed / BLINK_MS) % 2 == 0)) ? 1 : 0;
            if (on != st->colon_on) {
                st->colon_on = on;
                lv_obj_set_style_text_opa(st->colon,
                    on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            }
        }
    }

    /* 插画 accent 动画：16 步 / 3s 周期，仅天气态 + 过渡窗口外。
     * BREATH = 相位偏移三角波（呼吸/流动/闪烁），FLASH = 爆闪
     * （前 2 步全亮、1 步半亮、其余低亮——打闪节奏）。 */
    if (st->mode == MODE_WEATHER && !in_trans && st->motion_ok
        && st->accent_n > 0) {
        uint32_t ph = now % BREATH_MS;
        int step = (int)(ph * 16u / BREATH_MS);
        if (step != st->breath_step) {
            st->breath_step = step;
            for (int i = 0; i < st->accent_n; ++i) {
                int base = st->accent[i].base;
                int s16 = (step + st->accent[i].phase) % 16;
                lv_opa_t v;
                if (st->accent[i].mode == ACC_FLASH) {
                    v = (lv_opa_t)((s16 < 2) ? base
                                 : (s16 == 2) ? base * 2 / 3
                                              : base * 2 / 5);
                } else {
                    int tri = (s16 < 8) ? s16 : (15 - s16);  /* 0..7..0 */
                    v = (lv_opa_t)(base - (7 - tri) * base / 18);
                }
                wx_set_opa_by_mark(st->accent[i].o, v);
            }
        }
    }
}

/* ════ init / lifecycle ═════════════════════════════════════════════ */

/* ── v5.1 转场时间锚点：同实体真变形 ────────────────────────────────
 * 本场景小钟 rest = 右上角 26px（与左上地名同字号、同行对齐）；
 * 共识姿态 = 顶部中央 48px。变形是同一 time_lbl 的位移（x/y 连续）+
 * 字号档位阶梯 26→33→40→48（transform_scale 是红线；档位步进每档
 * 一次原生栅格化）。TOP_RIGHT 对齐下共识位等效 x = -(233-70) = -163
 * （48px "HH:MM" 宽 ≈140，中心居屏）。其余内容暂未接演员表。 */
#define TIME_REST_X       (-PAGE_MARGIN)
#define TIME_REST_Y       64   /* 与地名光学同行：Rounded 数字的 ink 比
                                * Consolas 汉字行低 ~6px，上提补偿 */
#define TIME_REST_PX      26
#define TIME_CONS_X       (-163)
#define TIME_CONS_Y       56
#define TIME_CONS_PX      48

static const int16_t WTIME_RUNGS[] = { 26, 33, 40, 48 };
#define WTIME_RUNG_N 4

static void wx_anim_time_morph(void *var, int32_t v)   /* 0=rest..1000=cons */
{
    weather_scene_t *st =
        (weather_scene_t *)lv_obj_get_user_data((lv_obj_t *)var);
    if (!st) return;
    st->wtime_v = v;
    lv_obj_set_x(st->sb.time_lbl,
                 TIME_REST_X + (TIME_CONS_X - TIME_REST_X) * v / 1000);
    lv_obj_set_y(st->sb.time_lbl,
                 TIME_REST_Y + (TIME_CONS_Y - TIME_REST_Y) * v / 1000);
    int px = TIME_REST_PX + (TIME_CONS_PX - TIME_REST_PX) * v / 1000;
    int best = WTIME_RUNGS[0];
    for (int i = 1; i < WTIME_RUNG_N; ++i)
        if (LV_ABS(WTIME_RUNGS[i] - px) < LV_ABS(best - px))
            best = WTIME_RUNGS[i];
    if (best != st->wtime_px) {
        st->wtime_px = best;
        const lv_font_t *f = clock_font(best);
        if (f) lv_obj_set_style_text_font(st->sb.time_lbl, f, 0);
    }
}

static void wx_trans_to_consensus(scene_t *s, uint32_t ms)
{
    weather_scene_t *st = (weather_scene_t *)s->user_data;
    if (!st) return;
    /* 刻钟大钟态被打断时先瞬间收回小钟态，再变形归位。 */
    if (st->mode == MODE_CLOCK) pose_jump(st, MODE_WEATHER);
    lv_anim_delete(st->sb.time_lbl, wx_anim_time_morph);
    lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_COVER, 0);
    if (ms == 0) { wx_anim_time_morph(st->sb.time_lbl, 1000); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->sb.time_lbl);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, apple_ease_in);
    lv_anim_set_values(&a, st->wtime_v, 1000);
    lv_anim_set_exec_cb(&a, wx_anim_time_morph);
    lv_anim_start(&a);
}

static void wx_trans_from_consensus(scene_t *s, uint32_t ms)
{
    weather_scene_t *st = (weather_scene_t *)s->user_data;
    if (!st) return;
    lv_anim_delete(st->sb.time_lbl, wx_anim_time_morph);
    lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_COVER, 0);
    if (ms == 0) { wx_anim_time_morph(st->sb.time_lbl, 0); return; }
    wx_anim_time_morph(st->sb.time_lbl, 1000);   /* 瞬切帧＝共识姿态 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->sb.time_lbl);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, spring_disp);
    lv_anim_set_values(&a, 1000, 0);
    lv_anim_set_exec_cb(&a, wx_anim_time_morph);
    lv_anim_start(&a);
}

static trans_profile_t s_wx_profile = {
    .actors               = NULL,
    .actor_n              = 0,
    .clock_to_consensus   = wx_trans_to_consensus,
    .clock_from_consensus = wx_trans_from_consensus,
};

static void weather_init(scene_t *s, lv_obj_t *parent)
{
    weather_scene_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->colon_on = -1;
    st->breath_step = -1;

    status_bar_create(parent, &st->sb);
    /* 环境挂钟定位：footer 的 agent 计数是监控域 chrome，此场景砍掉
     * 这一层级换底部留白（status_bar_update 照写隐藏标签，无害）。 */
    lv_obj_add_flag(st->sb.active_num, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(st->sb.active_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(st->sb.token_num, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(st->sb.token_cap, LV_OBJ_FLAG_HIDDEN);
    /* 小钟 rest = 右上角 26px，与左上地名（LABEL 26px, y=70）同字号
     * 同行对齐；顶部中央只留 conn dot。status_bar 实例是场景私有的，
     * 其它场景的顶钟不受影响。user_data 携带 st 给变形 exec。 */
    lv_obj_align(st->sb.time_lbl, LV_ALIGN_TOP_RIGHT, TIME_REST_X, TIME_REST_Y);
    lv_obj_set_user_data(st->sb.time_lbl, st);
    st->wtime_px = TIME_CONS_PX;      /* status_bar 建的是 48px */
    st->wtime_v  = 0;
    wx_anim_time_morph(st->sb.time_lbl, 0);   /* → 26px rest 姿态 */

    /* ── 天气主区（整组做 y/opa 过渡） ── */
    st->wx_grp = mk_box(parent, SCREEN_W, 466);
    lv_obj_set_pos(st->wx_grp, 0, 0);

    /* 左上角地点 — 加粗与右上时间对称（Rounded Black 数字视觉很重，
     * 常规 LABEL 压不住右侧）。 */
    st->loc_lbl = mk_label(st->wx_grp, ui_type_bold(UI_T_LABEL), COL_DIM, "");
    lv_obj_align(st->loc_lbl, LV_ALIGN_TOP_LEFT, PAGE_MARGIN, LOC_Y);
    wx_mark(st->loc_lbl, FADE_TEXT, 255);

    /* 大插画容器 */
    st->big_icon.root = mk_box(st->wx_grp, ILLUS_SZ, ILLUS_SZ + 8);
    lv_obj_set_pos(st->big_icon.root, ILLUS_X, ILLUS_Y);

    /* 右列：大温度 + 天气词 */
    st->temp_lbl = mk_label(st->wx_grp, ui_type_bold(UI_T_HERO), COL_TEXT, "");
    lv_obj_align(st->temp_lbl, LV_ALIGN_TOP_MID, TEMP_DX, TEMP_Y);
    wx_mark(st->temp_lbl, FADE_TEXT, 255);
    st->cond_lbl = mk_label(st->wx_grp, ui_type(UI_T_BODY), COL_TEXT, "");
    lv_obj_align(st->cond_lbl, LV_ALIGN_TOP_MID, TEMP_DX, COND_Y);
    wx_mark(st->cond_lbl, FADE_TEXT, 255);

    /* 留白处的复古小星：一颗补在顶行正中（时间挪去右上后的空缺），
     * 另两颗散在插画与温度间的组间留白。 */
    mk_star(st->wx_grp, 0, 233, 86, 5, 100);
    mk_star(st->wx_grp, 1, 200, 148, 5, 110);
    mk_star(st->wx_grp, 2, 214, 250, 4, 80);

    /* 五天条带 */
    for (int i = 0; i < WEATHER_DAYS; ++i) {
        st->day_name[i] = mk_label(st->wx_grp,
            i == 1 ? ui_type_bold(UI_T_LABEL) : ui_type(UI_T_LABEL),
            COL_DIM, "");
        lv_obj_align(st->day_name[i], LV_ALIGN_TOP_MID, STRIP_DX[i],
                     STRIP_NAME_Y);
        wx_mark(st->day_name[i], FADE_TEXT, 255);

        st->day_icons[i].root = mk_box(st->wx_grp, STRIP_ICON_SZ,
                                       STRIP_ICON_SZ);
        lv_obj_align(st->day_icons[i].root, LV_ALIGN_TOP_MID, STRIP_DX[i],
                     STRIP_ICON_Y);

        st->day_temp[i] = mk_label(st->wx_grp, ui_type(UI_T_LABEL),
                                   COL_TEXT, "");
        lv_obj_align(st->day_temp[i], LV_ALIGN_TOP_MID, STRIP_DX[i],
                     STRIP_TEMP_Y);
        wx_mark(st->day_temp[i], FADE_TEXT, 255);
    }

    /* 占位：还没有天气数据 */
    st->wait_lbl = mk_label(st->wx_grp, ui_type(UI_T_BODY), COL_DIM,
                            "等待天气数据");
    lv_obj_align(st->wait_lbl, LV_ALIGN_TOP_MID, 0, 246);
    wx_mark(st->wait_lbl, FADE_TEXT, 255);

    /* ── 时钟主区（大钟 + 日期 + 迷你天气），初始隐藏 ── */
    st->face_grp = mk_box(parent, FACE_GRP_W, FACE_GRP_H);
    lv_obj_align(st->face_grp, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(st->face_grp, LV_OBJ_FLAG_HIDDEN);

    const lv_font_t *bf = clock_font(CLOCK_PX);
    if (!bf) bf = &lv_font_montserrat_48;

    st->hh = mk_label(st->face_grp, bf, COL_TEXT, "--");
    lv_obj_align(st->hh, LV_ALIGN_TOP_MID, -COLON_DX, FACE_Y);
    st->colon = mk_label(st->face_grp, bf, COL_TEXT, ":");
    lv_obj_align(st->colon, LV_ALIGN_TOP_MID, 0, FACE_Y);
    st->mm = mk_label(st->face_grp, bf, COL_TEXT, "--");
    lv_obj_align(st->mm, LV_ALIGN_TOP_MID, COLON_DX, FACE_Y);

    st->date_lbl = mk_label(st->face_grp, ui_type(UI_T_BODY), COL_TEXT, "");
    lv_obj_align(st->date_lbl, LV_ALIGN_TOP_MID, 0, DATE_Y);
    st->mini_lbl = mk_label(st->face_grp, ui_type(UI_T_LABEL), COL_DIM, "");
    lv_obj_align(st->mini_lbl, LV_ALIGN_TOP_MID, 0, MINI_Y);

    /* 初始 DOM（占位/星星/空标签）此刻全部处于基准不透明度。 */
    fade_rebase(st);

    st->timer = lv_timer_create(weather_tick, TICK_MS, st);
    lv_timer_pause(st->timer);

    scene_trans_bind("weather", &s_wx_profile);
}

static void weather_on_show(scene_t *s)
{
    weather_scene_t *st = (weather_scene_t *)s->user_data;
    if (!st) return;

    uint32_t tz_epoch = 0;
    bool synced;
    agent_state_lock();
    agent_state_t *a = agent_state_get();
    st->motion_ok = !a->motion_reduced;
    synced = local_now(a, &tz_epoch);
    agent_state_unlock();

    /* 进场直接摆到当下正确的姿态，不播过渡（场景框架自带 crossfade）。 */
    st->mode = (synced && in_quarter_window(tz_epoch)) ? MODE_CLOCK
                                                       : MODE_WEATHER;
    st->mode_since_ms = lv_tick_get() - (MORPH_LAG_MS + MORPH_IN_MS);
    st->colon_on = -1;
    st->cached_hhmm[0] = '\0';
    st->wx_drawn = false;         /* 强制重画（数据可能在离场期间更新过） */
    pose_jump(st, st->mode);

    if (st->timer) {
        lv_timer_resume(st->timer);
        weather_tick(st->timer);
    }
}

static void weather_on_hide(scene_t *s)
{
    weather_scene_t *st = (weather_scene_t *)s->user_data;
    if (!st) return;
    /* 杀掉在飞动画并停在天气态姿态，crossfade 快照才不会截到半程。 */
    pose_jump(st, MODE_WEATHER);
    st->mode = MODE_WEATHER;
    lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_COVER, 0);
    if (st->timer) lv_timer_pause(st->timer);
}

scene_t scene_weather = {
    .id           = "weather",
    .display_name = "Weather",
    .accent       = LV_COLOR_MAKE(0xE0, 0xA0, 0x30),
    .description  = "Retro-futurist line-art weather (5-day window) stacked "
                    "with a clock zone; the clock swells for 30 s at each "
                    "quarter hour.",
    .tags         = "weather,clock,ambient",
    .init         = weather_init,
    .on_show      = weather_on_show,
    .on_hide      = weather_on_hide,
};
