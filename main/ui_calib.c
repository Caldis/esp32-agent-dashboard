/*
 * ui_calib — 面板可见几何的标定尺 (v7.3)。
 *
 * 存在理由：屏幕的三件基本事实——可见区起点、可见区大小、圆角半径——
 * 在这个仓库里长期是【规格书上抄来的数 + 手调的猜测】，从没在 LVGL 坐标
 * 里验证过。代价是两个用户可见的缺陷（右/下缺口、拐角贴不上）。
 *
 * 而它们【只有一双眼睛能读】：`?dump` 渲染的是整个 480×480 帧缓冲【缩放】
 * 到请求尺寸，不是可见区裁剪——物理上哪一列点亮根本不在帧缓冲里。所以
 * 这把尺子的设计目标是：把"看不清的 1px 判断"换成"数得清的有/无"。
 *
 *   ?vis 1  原点   —— 顶/左刻度条，最外可见的那条即原点
 *   ?vis 2  延伸   —— 右/下刻度条，最外可见的那条即最后一列/行
 *   ?vis 3  圆角   —— 四角各一个候选半径并行比对（见下）
 *   ?vis 4  远端   —— 3px 粗探针打到坐标空间尽头，用来质疑"可见区多大"
 *   ?vis 5  复核   —— 四边同一套刻度（0/3/7/12px），对称二次确认
 *   ?vis 0  收起
 *
 * 圆角那一档的判据值得记住，因为它是反直觉的：候选半径【小于】真值时，
 * 弧落在面板圆角切掉的区域里 -> 整条看不见；【大于】真值则整条可见但与
 * 边缘留缝。所以"最小的那条完整弧"就是真值，四轮二分即可定死。
 *
 * v7.3 的读数（结论已写进 ui_screen.h）：可见区 = 整个 [0,479]²，原点
 * 0，圆角 76。换面板批次要复量——这就是留着它的意义。
 */

#include "ui_calib.h"
#include "ui_screen.h"

#include "harness/console_protocol.h"
#include "bsp/esp-bsp.h"

#include "lvgl.h"

/* 候选原点：1/2/4/5。
 *
 * 第一轮打的是 0/3/6/9，用户报「都不完整」——那本身就是一次有效测量：
 * 完整一圈当且仅当 k == a，四个都不完整就把这四个值全排除了，加上历史
 * 夹逼 a ≤ 6，剩下的候选正好是 {1,2,4,5}。第二轮就打这四个数，一轮定死。
 *
 * 代价是相邻候选只差 1px，四条 1px 线并排在 305ppi 上会糊成一团——所以
 * 这轮【一次只画一个】，用 `?vis <k>` 逐个看。参数即候选值，回答只需要
 * 说哪个 k 四条边同时贴住边缘。 */
static const struct { int16_t k; uint32_t rgb; const char *name; } CAND[] = {
    { 1, 0xFF3020, "red"    },
    { 2, 0x30FF40, "green"  },
    { 4, 0x40A0FF, "blue"   },
    { 5, 0xFFFFFF, "white"  },
};
#define CAND_N (sizeof(CAND) / sizeof(CAND[0]))

/* 刻度条：每个候选一条【1px】的短线，贴在它对应的那一行/那一列上。
 * 横向四条测 a_y（贴顶边），纵向四条测 a_x（贴左边）。
 *
 * 为什么不再画整框：候选之间只差 1px，四个 1px 的同心框在 305ppi 上会
 * 糊成一条彩带，"哪个完整"根本分不出来。而刻度条把问题换成了肉眼最擅长
 * 的判断——**最外面那条能看见的是什么颜色**：k < a 的条整条落在屏外看
 * 不见，k ≥ a 的条都看得见，所以第一条可见的就是 a。四条沿边错开摆，
 * 位置本身也帮着辨认顺序。 */
#define BAR_LEN   90
static const int16_t BAR_AT[CAND_N] = { 40, 150, 260, 350 };  /* 沿边错开 */

/* ── mode 2：可见区【延伸】的候选（右缘 / 下缘的最后一列/行） ───────
 * 量出原点还不够：右缘与下缘的位置依赖"可见区正好 466 宽"这个假设，而
 * 那个假设从来没被验证过（用户点出的正是这一点）。原点 a=1 时，若宽度
 * 确实是 466，最后一列就在 466。四条 1px 竖线打在 464/465/466/467：
 * 落在屏外的看不见，所以【最靠外的那条可见线】就是真正的最后一列。 */
static const int16_t EXT_CAND[CAND_N] = { 464, 465, 466, 467 };

/* ── mode 3：圆角半径的候选 ─────────────────────────────────────────
 * UI_VIS_RADIUS=60 的来历是"实测调定"，也就是手调出来的估计，同样没被
 * 验证过。四个候选一角一个（面板四角物理对称，所以可以并行测）：
 * 弧线与物理边缘完全贴合的那个角就是真半径——半径偏小会在弧外留一条黑
 * 边，偏大则弧的两端被面板裁掉、线看起来断开。 */
static const int16_t RAD_CAND[CAND_N] = { 73, 74, 75, 76 };

/* ── mode 4：远端探针 —— 质疑 UI_VIS_W=466 本身 ─────────────────────
 * mode 1/2 的读数合起来说"可见区至少覆盖 [1,467]"，比 466 宽；用户又
 * 报"按 466 方框锚定的四个圆角弧全都缩在里面、不贴边"。两条证据指向同
 * 一个可能：**可见区接近整个 480 空间**，也就是 UI_VIS_W=466 这个前提
 * （来自产品规格，从未在 LVGL 坐标里验证过）根本不成立。
 *
 * 这一档把探针一路打到坐标空间的尽头。线宽 3px——到这一步要定的是"边
 * 在哪一带"，不是 1px 精度，别再考验眼力。 */
static const int16_t FAR_CAND[CAND_N] = { 468, 472, 476, 479 };
#define FAR_W 3

/* ── mode 5：四边对称的二次确认 ─────────────────────────────────────
 * mode 4 证明右/下一直可见到 479、mode 1 证明左/上可见到 1，于是结论是
 * "整个 480 空间都可见"。但那结论压在【没探过第 0 列/行】上——上下左右
 * 四条边都该在同一套刻度下复核一遍，而不是各测各的。
 *
 * 每边四条：距该边 0 / 3 / 7 / 12 px（2px 粗，彼此留缝）。若可见区真是
 * [0,479]，四边都应看到全部四条；某边少看到 n 条，就是那一侧被裁掉的量。 */
static const int16_t EDGE_OFF[CAND_N] = { 0, 3, 7, 12 };
#define EDGE_W 2

static lv_obj_t *s_bar[CAND_N * 2];
static lv_obj_t *s_ext[CAND_N * 2];
static lv_obj_t *s_arc[CAND_N];
static lv_obj_t *s_far[CAND_N * 2];
static lv_obj_t *s_edge[CAND_N * 4];      /* 上 / 下 / 左 / 右 */

static lv_obj_t *mk_bar(int x, int y, int w, int h, uint32_t rgb)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

/* 一角一个候选半径的 1px 四分弧。LVGL 角度：0°=3 点钟，顺时针。 */
static lv_obj_t *mk_corner_arc(int cx, int cy, int r, int a0, int a1,
                               uint32_t rgb)
{
    lv_obj_t *a = lv_arc_create(lv_layer_top());
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, 2 * r, 2 * r);
    lv_obj_set_pos(a, cx - r, cy - r);
    lv_arc_set_bg_angles(a, (lv_value_precise_t)a0, (lv_value_precise_t)a1);
    lv_obj_set_style_arc_color(a, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
    return a;
}

static void calib_build(void)
{
    if (s_bar[0]) return;
    for (unsigned i = 0; i < CAND_N; ++i) {
        /* mode 1 — 原点。横条：y = 候选值，测 a_y；竖条：x = 候选值，测 a_x。 */
        s_bar[i] = mk_bar(BAR_AT[i], CAND[i].k, BAR_LEN, 1, CAND[i].rgb);
        s_bar[CAND_N + i] = mk_bar(CAND[i].k, BAR_AT[i], 1, BAR_LEN,
                                   CAND[i].rgb);
        /* mode 2 — 延伸。竖条贴右缘（测最后一列），横条贴下缘。 */
        s_ext[i] = mk_bar(EXT_CAND[i], BAR_AT[i], 1, BAR_LEN, CAND[i].rgb);
        s_ext[CAND_N + i] = mk_bar(BAR_AT[i], EXT_CAND[i], BAR_LEN, 1,
                                   CAND[i].rgb);
        /* mode 4 — 远端探针（3px 粗），打到坐标空间尽头。 */
        s_far[i] = mk_bar(FAR_CAND[i] - FAR_W + 1, BAR_AT[i], FAR_W, BAR_LEN,
                          CAND[i].rgb);
        s_far[CAND_N + i] = mk_bar(BAR_AT[i], FAR_CAND[i] - FAR_W + 1,
                                   BAR_LEN, FAR_W, CAND[i].rgb);

        /* mode 5 — 四边同一套刻度。LAST = 坐标空间最后一列/行。 */
        const int LAST = UI_LV_W - 1;
        int off = EDGE_OFF[i], at = BAR_AT[i];
        s_edge[i]              = mk_bar(at, off, BAR_LEN, EDGE_W,
                                        CAND[i].rgb);                 /* 上 */
        s_edge[CAND_N + i]     = mk_bar(at, LAST - off - EDGE_W + 1,
                                        BAR_LEN, EDGE_W, CAND[i].rgb);/* 下 */
        s_edge[CAND_N * 2 + i] = mk_bar(off, at, EDGE_W, BAR_LEN,
                                        CAND[i].rgb);                 /* 左 */
        s_edge[CAND_N * 3 + i] = mk_bar(LAST - off - EDGE_W + 1, at,
                                        EDGE_W, BAR_LEN, CAND[i].rgb);/* 右 */
    }
    /* mode 3 — 圆角。一角一个候选半径，锚在【已量准的】可见区方框上。 */
    const int L = UI_VIS_ORG, T = UI_VIS_ORG;
    const int R = UI_VIS_ORG + UI_VIS_W - 1, B = UI_VIS_ORG + UI_VIS_W - 1;
    s_arc[0] = mk_corner_arc(L + RAD_CAND[0], T + RAD_CAND[0], RAD_CAND[0],
                             180, 270, CAND[0].rgb);   /* 左上 */
    s_arc[1] = mk_corner_arc(R - RAD_CAND[1], T + RAD_CAND[1], RAD_CAND[1],
                             270, 360, CAND[1].rgb);   /* 右上 */
    s_arc[2] = mk_corner_arc(L + RAD_CAND[2], B - RAD_CAND[2], RAD_CAND[2],
                              90, 180, CAND[2].rgb);   /* 左下 */
    s_arc[3] = mk_corner_arc(R - RAD_CAND[3], B - RAD_CAND[3], RAD_CAND[3],
                               0,  90, CAND[3].rgb);   /* 右下 */
}

static void set_hidden(lv_obj_t **objs, int n, bool show)
{
    for (int i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        if (show) lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_calib_show(int mode)
{
    calib_build();
    set_hidden(s_bar, CAND_N * 2, mode == 1);
    set_hidden(s_ext, CAND_N * 2, mode == 2);
    set_hidden(s_arc, CAND_N,     mode == 3);
    set_hidden(s_far, CAND_N * 2, mode == 4);
    set_hidden(s_edge, CAND_N * 4, mode == 5);
}

/* console task 不在 LVGL task 上——建对象/改 flag 必须持显示锁。 */
static int cmd_vis(const console_args_t *args)
{
    int mode = (args->argc >= 2) ? atoi(args->argv[1]) : 0;
    bsp_display_lock(-1);
    ui_calib_show(mode);
    bsp_display_unlock();
    const char *legend =
        mode == 1 ? "origin bars @1/2/4/5 (top=a_y,left=a_x); "
                    "outermost VISIBLE colour == origin"
      : mode == 2 ? "extent bars @464/465/466/467 (right,bottom); "
                    "outermost VISIBLE colour == last visible col/row"
      : mode == 3 ? "corner arcs r=73(TL,red)/74(TR,green)/75(BL,blue)/"
                    "76(BR,white); SMALLEST arc that is not cut == radius"
      : mode == 4 ? "FAR probe 3px @468/472/476/479 (right,bottom) — tests "
                    "whether the visible area is really only 466 wide"
      : mode == 5 ? "EDGE confirm: all four edges, 2px bars 0/3/7/12 px "
                    "inward (red/green/blue/white); missing bars == crop"
                  : "off";
    console_reply_ok("{\"calib\":%d,\"colors\":\"red,green,blue,white\","
                     "\"read\":\"%s\"}", mode, legend);
    return 0;
}

static const console_cmd_t s_cmd_vis = { "?vis", cmd_vis,
    "panel geometry ruler: ?vis 0=off 1=origin 2=extent(right/bottom) "
    "3=corner-radius. Only a human eye can read these — the framebuffer "
    "does not know which pixels the panel lights." };

void ui_calib_init(void)
{
    console_protocol_register(&s_cmd_vis);
}
