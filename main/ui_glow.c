/*
 * ui_glow — 见 ui_glow.h。（v6.6 scene_flash → v6.9 组件化 → v7.3 色散重制。）
 *
 * ── v7.3 为什么换架构 ─────────────────────────────────────────────
 * 旧实现是 N 个 lv_obj 各画一圈描边，靠 ui_motion 的批量样式写入 +
 * 手动失效四条边带来压住"每层包围盒都是整屏"的重绘代价。那一招只对
 * 【不带 LAYOUT 标志的属性】成立——opa 可以，位置和尺寸不行：
 * lv_obj_enable_style_refresh(false) 会让 refresh_style 整个提前返回，
 * 连布局标脏一起跳过，几何改动根本不会生效。
 *
 * 而色散要的正是几何：位移（色差的本质就是各色分量错开）、放大、失焦
 * （用变宽+变淡近似模糊）。所以改成【一个自绘对象】：一个覆盖全屏的
 * lv_obj，在 LV_EVENT_DRAW_MAIN 里自己画 N 圈。好处是三重的——
 *   · 几何变化只是绘制参数，不碰样式系统，没有布局、没有对象级失效；
 *   · 失效范围完全由我们决定（照旧只失效四条边带，代价与层数无关）；
 *   · 加一层不再等于加一个对象。
 *
 * ── 可见区原点 ───────────────────────────────────────────────────
 * 面板可见区在 LVGL 空间里的起点 a 量不准（`?vis` 夹到 {1,2,4,5}，见
 * ui_screen.h）。旧代码按 a=0 摆，于是右/下缺口恒为 a+3、左/上只有
 * 3-a ——用户报的"右侧和底部有点间隙"就是这个。
 * 新做法不去贴那条量不准的边：整个环组【向外溢出 GLOW_BLEED】，由物理
 * 边缘裁掉多余部分。a 取 0..7 任意值都不会露缺口，最多是最外圈被裁掉
 * 几像素——而最外圈本来就是渐变最亮的一端，少几像素看不出来。
 */

#include "ui_glow.h"
#include "ui_motion.h"
#include "ui_screen.h"
#include "theme.h"

#include "harness/console_protocol.h"
#include "bsp/esp-bsp.h"

#include <stdlib.h>

#include "lvgl.h"

/* ── 几何 ────────────────────────────────────────────────────────── */

/* 层数/步长/线宽三个数直接乘进每帧的重绘面积，是这套效果的成本旋钮。
 * v7.3 定档过程（实测，`?glow` + `?perf`，每档三次取中位）：
 *   5 层 / step4 / blur7 / 半径每帧变 ....... 37.7 ms/帧
 *   + 半径常量化（见 draw_cb） .............. 31.6 ms/帧
 *   + 去掉 HIDDEN 的整屏失效 + 收到 4 层 .... 26.6 ms/帧（≈37fps）
 *   + 带宽按单轴算 + opa 地板 ............... 26.6（噪声内，无效）
 * 同期排除的死胡同：把圆角 mask 缓存从 4 开到 16 项——推理很顺（缓存
 * 只按半径做键，四层描边要八个键），实测两处皆无收益，台账在
 * sdkconfig.defaults。键稳定之后 4 项就够用。
 * 四层已经够读出棱镜分光（品红/琥珀/teal/紫），第五层的边际观感远不
 * 抵它的成本。
 *
 * 剩下的 26.6ms 是【面积】：四条边带 ~87k px/帧，而带宽的大头不是墨迹
 * 深度而是【圆角折算】（BOX_RADIUS - R_in/√2，见 ink_band）——半径 64
 * 的角光是这一项就占 ~19px 的下限。想再降只能动几何本身（更小的
 * BOX_RADIUS 会与面板圆角失配，不可取），或者把边带拆成"细直边 + 四个
 * 角方块"（估算只省 ~30%，却把 4 次分块渲染变成 8 次，未必划算——要量）。
 * 与旧版单色渐变的 18.5ms 之差，就是这套效果的明码标价。 */
#define RING_N        4      /* 色散分光的层数 */
#define RING_STEP     3      /* 每层内缩 px（静止姿态） */
#define RING_WIDTH    3
#define GLOW_BLEED    4      /* 向外溢出量：吃掉 a 的不确定度 */

/* 环组外框：以可见区估计原点为基准再外扩 BLEED。半径同步外扩，四角才
 * 与面板圆角同心。 */
#define BOX_X         (UI_VIS_ORG - GLOW_BLEED)
#define BOX_W         (UI_VIS_W + 2 * GLOW_BLEED)
#define BOX_RADIUS    (UI_VIS_RADIUS + GLOW_BLEED)

/* 运动的最大外溢。放大只把环往【外】推（推到屏外，被物理边缘吃掉），
 * 所以它不加深墨迹，不进带宽公式；位移和失焦变宽会。 */
#define MAX_DISP      5
#define MAX_EXPAND    8
#define MAX_BLUR_W    4

/* 低于这个 opa 的层不画也不算带宽（见 draw_cb / ink_band）。 */
#define GLOW_OPA_FLOOR 6

/* ── 色散谱 ──────────────────────────────────────────────────────
 * 按键回应走【全谱分光】：五层各占一个色相，叠加位移之后读作棱镜把
 * 一道白光拆开。teal 留在谱中间——它是本机的家族色，让这道彩虹仍然
 * 姓这个项目的姓。
 *
 * 状态提示（waiting/lost）不进谱：颜色在这台设备上是【状态契约】
 * （金=该你了、teal=思考中、暗=空闲），把掉线提示做成彩虹就是拿契约
 * 换炫技。它们只在自身色相左右做很轻的分光（DISP_TINT），保留"色差"
 * 的质感而不改变语义。 */
static const uint32_t SPECTRUM[RING_N] = {
    0xFF2A5A,   /* 品红 —— 长波端 */
    0xFF9020,   /* 琥珀 */
    0x2BB3B1,   /* teal —— 家族色 */
    0xA040FF,   /* 紫 —— 短波端 */
};

/* 单色分光的色相偏移（±，作用在 R/B 通道上，近似"这一层偏红/偏蓝"）。 */
static const int8_t DISP_TINT[RING_N] = { +40, +14, -14, -40 };

/* color==0 是"跟随主题强调色"的哨兵（见 ui_glow_ping）。
 * KEY 满扩散 + 全谱；两个状态提示窄扩散 + 单色轻分光。 */
const ui_glow_style_t UI_GLOW_KEY     = { 0x000000, 200, 255 };
const ui_glow_style_t UI_GLOW_WAITING = { 0xE0A030, 150,  96 };
const ui_glow_style_t UI_GLOW_LOST    = { 0xE0503C, 150,  96 };

static uint32_t tint_rgb(uint32_t rgb, int8_t t)
{
    int r = (int)((rgb >> 16) & 0xFF) + t;
    int b = (int)(rgb & 0xFF) - t;
    r = (r < 0) ? 0 : (r > 255) ? 255 : r;
    b = (b < 0) ? 0 : (b > 255) ? 255 : b;
    return ((uint32_t)r << 16) | (rgb & 0x00FF00) | (uint32_t)b;
}

/* ── 每层的绘制参数（自绘回调唯一的输入） ───────────────────────── */

typedef struct {
    int16_t  inset;      /* 相对环组外框的内缩，可为负（溢出） */
    int16_t  width;      /* 描边宽度；变宽 + 变淡 = 失焦 */
    int16_t  dx, dy;     /* 位移 —— 色差的本体 */
    lv_opa_t opa;
    uint32_t rgb;
} ring_draw_t;

static ring_draw_t s_ring[RING_N];
static lv_obj_t   *s_canvas;          /* 唯一的自绘对象 */
static lv_timer_t *s_timer;
static bool  s_holding;               /* 是否持有高刷档（必须成对） */

static bool  s_sustain_on;
static ui_glow_style_t s_sustain;
static ui_glow_style_t s_active;      /* 当前在播的样式 */
static bool  s_active_spectrum;       /* 本次播放是否用全谱 */
static uint32_t s_t0;                 /* 本次播放的起点 tick */

/* ── 包络与效果 ──────────────────────────────────────────────────
 * 一次 ping 的时间线（‰ of PING_MS）：
 *
 *   000-080  收束：整组从屏外压进来（放大收敛），亮度冲到峰值
 *   080-350  分光：位移由 0 张到最大，五层各朝不同方向错开——这一段
 *            是"色散"本身，白光被拆成五道
 *   350      翻转：色序前后对调（棱镜倒置），一次视觉重音
 *   400-850  依次失焦：由外向内逐层变宽变淡，像焦点一层层松掉
 *   600-1000 绽散：整组一边向外撑开一边淡出
 *   全程     波动：一道正弦沿层序推进，让五层此起彼伏而不是齐明齐灭
 *
 * 排期的铁律（第一版就栽在这）：**效果必须落在还亮着的时段**。上一版
 * 用 1-(1-t)^3 收尾，t=0.71 时亮度已经只剩 2%，而失焦/翻转排在 0.45
 * 之后——等于把最精彩的三个效果演给黑屏看。现在亮度保持到 0.6 才开始
 * 线性退场，所有效果都在幕布拉上之前演完。 */
#define PING_MS       950
#define STEP_MS        16     /* 与高刷档同拍——这套效果值得满帧 */
#define WAVE_CYCLES     2     /* 波沿层序推进的圈数 */
#define REVERSE_AT    350     /* ‰，色序翻转的时刻 */

/* 0..1000 定点。sin 用 LVGL 的整数三角（返回 -32767..32767）。 */
static int32_t wave(int32_t phase_deg)
{
    return lv_trigo_sin((int16_t)(phase_deg % 3600 / 10)) / 328;  /* ±100 */
}

/* 把 (样式, 归一化时间) 解算成五层的绘制参数。纯函数——所有效果都在
 * 这里，绘制回调只负责把结果画出来。 */
static void solve_rings(const ui_glow_style_t *st, int32_t t1000, bool spectrum)
{
    bool reversed = t1000 >= REVERSE_AT;

    /* 整体亮度：冲起来快，保持住，最后线性退场。退场用线性而不是缓出
     * ——缓出会把 80% 的亮度在前 30% 的时间里丢掉（见上面的排期铁律）。 */
    int32_t g;
    if (t1000 < 80)       g = t1000 * 1000 / 80;
    else if (t1000 < 600) g = 1000;
    else                  g = (1000 - t1000) * 1000 / 400;
    if (g < 0) g = 0;
    if (g > 1000) g = 1000;

    /* 放大：起手从屏外压进来（收束），尾段再向外绽散。中间保持静止位，
     * 让分光和失焦独占舞台。 */
    int32_t expand = 0;
    if (t1000 < 80)        expand = MAX_EXPAND * (80 - t1000) / 80;
    else if (t1000 > 600)  expand = MAX_EXPAND * (t1000 - 600) / 400;

    /* 分光：位移幅度 0 → 满，在 80..350 之间张开，之后保持。 */
    int32_t spread_amp = t1000 < 80 ? 0
                       : t1000 < 350 ? MAX_DISP * (t1000 - 80) / 270
                                     : MAX_DISP;

    for (int i = 0; i < RING_N; ++i) {
        ring_draw_t *r = &s_ring[i];
        int32_t phase = i * 3600 / RING_N;                    /* 层相位 */

        /* 波动：一道正弦沿层序推进（±100）。 */
        int32_t w = wave(t1000 * 36 * WAVE_CYCLES / 100 + phase);

        /* 依次失焦：外层先散、内层后散（层间错峰），400 起 450 内走完。 */
        int32_t defocus = t1000 - 400 - i * 450 / RING_N;
        defocus = defocus < 0 ? 0 : defocus * 1000 / 450;
        if (defocus > 1000) defocus = 1000;

        /* 位移：方向沿层序错开并随时间旋转——五道光在边缘绕圈拉开，
         * 这就是"色散"读起来的样子。 */
        int32_t ang = t1000 * 36 / 100 + phase * 2;
        r->dx = (int16_t)(spread_amp * wave(ang) / 100);
        r->dy = (int16_t)(spread_amp * wave(ang + 900) / 100);

        r->inset = (int16_t)(i * RING_STEP - expand);
        /* 线宽量化成 2 档：内缘半径 = 外缘 - 线宽，也是一个缓存键，
         * 连续变化同样会冲刷缓存。两档已经够读出"焦点松掉"。 */
        r->width = (int16_t)(RING_WIDTH + (defocus > 500 ? MAX_BLUR_W : 0));

        /* 亮度：峰值 × 包络 × 层权重 × 波动，失焦再打个折（散开就该
         * 更淡，但只打到 2/3——打太狠效果就演给黑屏看了）。 */
        int32_t weight = 255 - i * 90 / RING_N;
        int32_t v = st->peak * g / 1000 * weight / 255;
        v = v * (100 + w * 30 / 100) / 100;                   /* ±30% 波动 */
        v = v * 1000 / (1000 + defocus / 2);
        /* spread 决定向内亮到第几层（边界层线性过渡，否则熄灭是台阶）。 */
        int32_t reach = (int32_t)st->spread * RING_N;
        int32_t lim = reach - i * 255;
        if (lim < 0) lim = 0;
        if (lim > 255) lim = 255;
        v = v * lim / 255;
        r->opa = (lv_opa_t)(v < 0 ? 0 : v > 255 ? 255 : v);

        /* 色序翻转：到点整组前后对调，一次视觉重音。 */
        int idx = reversed ? (RING_N - 1 - i) : i;
        r->rgb = spectrum ? SPECTRUM[idx] : tint_rgb(st->color, DISP_TINT[idx]);
    }
}

/* 静止姿态（sustain）：不动、不失焦、只保留很轻的分光。 */
static void solve_rings_static(const ui_glow_style_t *st)
{
    for (int i = 0; i < RING_N; ++i) {
        ring_draw_t *r = &s_ring[i];
        int32_t weight = 255 - i * 200 / RING_N;
        int32_t reach = (int32_t)st->spread * RING_N;
        int32_t lim = reach - i * 255;
        if (lim < 0) lim = 0;
        if (lim > 255) lim = 255;
        int32_t v = st->peak * weight / 255 * lim / 255;
        r->inset = (int16_t)(i * RING_STEP);
        r->width = RING_WIDTH;
        r->dx = r->dy = 0;
        r->opa = (lv_opa_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        r->rgb = tint_rgb(st->color, DISP_TINT[i]);
    }
}

/* ── 自绘 ────────────────────────────────────────────────────────
 * 五层各一次圆角描边。每层的包围盒都是整屏，但绘制被裁到当前失效的
 * 那条边带上，所以真正光栅化的只有边带与环相交的部分。 */
static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) return;

    lv_draw_rect_dsc_t dsc;
    for (int i = 0; i < RING_N; ++i) {
        const ring_draw_t *r = &s_ring[i];
        /* 低于地板的层直接不画：一层描边不管多淡都要付一次完整的圆角
         * mask + 填充，而 AMOLED 上 6/255 与全黑肉眼无差。淡出尾段大半
         * 时间都在这个区间，省下的是真钱。 */
        if (r->opa < GLOW_OPA_FLOOR || r->width <= 0) continue;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_opa       = LV_OPA_TRANSP;
        dsc.border_opa   = r->opa;
        dsc.border_color = lv_color_hex(r->rgb);
        dsc.border_width = r->width;
        /* 半径是【每层的常量】，不跟着 inset 走。LVGL 的圆角覆盖图缓存
         * 只按半径做键（见 sdkconfig.defaults 的 CIRCLE_CACHE 注释），
         * 半径每帧变 = 每帧重算每个角的抗锯齿图，实测 37.7 ms/帧。
         * 位移只是平移、放大只改方框尺寸，都不动半径键，所以那两个效果
         * 依旧免费；代价是放大时四角略微变"方"——肉眼不可辨。 */
        dsc.radius       = BOX_RADIUS - i * RING_STEP;
        lv_area_t a = {
            .x1 = BOX_X + r->inset + r->dx,
            .y1 = BOX_X + r->inset + r->dy,
            .x2 = BOX_X + BOX_W - 1 - r->inset + r->dx,
            .y2 = BOX_X + BOX_W - 1 - r->inset + r->dy,
        };
        lv_draw_rect(layer, &dsc, &a);
    }
}

/* 当前这一帧的墨迹深度（px，从环组外框往内算）。
 *
 * 带宽必须按【圆角】折算，不能按直边算：直边处最内层的墨只深入
 * inset+width，但四个角所有层的弧心都落在距角 (BOX_RADIUS, BOX_RADIUS)
 * 处，最内层的弧半径只有 R_in，于是它在 45° 方向距边
 * BOX_RADIUS - R_in/√2，比直边深得多。按直边算会让四角永远刷不到，留下
 * 上一帧残影——表现为发光在拐角处断开（v6.7 实测过的缺陷）。
 *
 * v7.3 改成【每帧现算】而不是取全程最大值：色散的位移/失焦只在中后段
 * 才把墨推深，一开始环还贴着边。按最坏情况开一条固定宽带，等于全程为
 * 那几帧买单——而带宽直接乘进每帧的重绘面积。 */
static int32_t ink_band(void)
{
    int32_t deepest = 0;                 /* 最内层墨迹的内缩量 */
    for (int i = 0; i < RING_N; ++i) {
        const ring_draw_t *r = &s_ring[i];
        if (r->opa < GLOW_OPA_FLOOR) continue;
        int32_t d = r->inset + r->width;
        /* 每条带只有【一个轴】的位移会加深它（上/下带看 dy，左/右带看
         * dx）。取两轴之和是把对角线当成了单轴深度，白白多算近一倍带宽
         * ——而带宽直接乘进每帧重绘面积。 */
        int32_t ax = r->dx > 0 ? r->dx : -r->dx;
        int32_t ay = r->dy > 0 ? r->dy : -r->dy;
        int32_t disp = ax > ay ? ax : ay;
        if (d + disp > deepest) deepest = d + disp;
    }
    if (deepest <= 0) return 0;
    int32_t r_in = BOX_RADIUS - deepest;
    if (r_in < 0) r_in = 0;
    return BOX_RADIUS - (r_in * 707) / 1000 + 6 + UI_VIS_ORG;
}

/* 只失效边缘四条窄带。整对象失效 = 整屏级重绘（v6.6 实测 53.4ms/帧），
 * 这一条是本组件存在的全部性能前提，不要改成 lv_obj_invalidate。
 * 收尾时要传上一帧的带宽（墨已经被算成 0 了，但屏上还留着）——所以
 * 调用方在改 s_ring 之前先记下旧带宽，取两者的大者。 */
static int32_t s_last_band;

static void invalidate_bands(void)
{
    if (!s_canvas) return;
    int32_t b = ink_band();
    if (s_last_band > b) b = s_last_band;      /* 擦掉上一帧留下的墨 */
    s_last_band = ink_band();
    if (b <= 0) return;
    if (b > UI_VIS_W / 2) b = UI_VIS_W / 2;
    const lv_area_t bands[4] = {
        { 0,          0,             UI_LV_W - 1, b - 1        },
        { 0,          UI_VIS_W - b,  UI_LV_W - 1, UI_VIS_W - 1 },
        { 0,          0,             b - 1,       UI_VIS_W - 1 },
        { UI_VIS_W-b, 0,             UI_LV_W - 1, UI_VIS_W - 1 },
    };
    for (int i = 0; i < 4; ++i) lv_obj_invalidate_area(s_canvas, &bands[i]);
}

/* 收起 = 把所有层的 opa 归零，【不是】切 HIDDEN。
 * lv_obj_clear_flag(HIDDEN) 会对整个对象失效——而这个对象是 480×480 的
 * 全屏画布，于是每次 ping 的第一帧都要整屏重绘一次（实测
 * inval_max_px=230400、render_max≈48 ms，一帧就把窗口均值拉高一大截）。
 * 画布常驻可见、靠 opa 收敛：空闲时 draw_cb 扫一遍四层全是 0 直接返回，
 * 成本可忽略，而失效范围永远只有那四条边带。 */
static void canvas_clear(void)
{
    for (int i = 0; i < RING_N; ++i) s_ring[i].opa = 0;
}

static void drop_tier(void)
{
    if (s_holding) { ui_motion_release(); s_holding = false; }
}

/* 播完之后回落到 sustain 姿态（若有），否则收干净。 */
static void settle(void)
{
    lv_timer_pause(s_timer);
    drop_tier();
    if (s_sustain_on) {
        s_active = s_sustain;
        solve_rings_static(&s_active);
    } else {
        canvas_clear();
    }
    invalidate_bands();
}

static void step_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_canvas) return;
    uint32_t el = lv_tick_elaps(s_t0);
    if (el >= PING_MS) { settle(); return; }
    solve_rings(&s_active, (int32_t)(el * 1000 / PING_MS), s_active_spectrum);
    invalidate_bands();
}

void ui_glow_init(void)
{
    if (s_canvas) return;
    /* 一个覆盖【整个 LVGL 空间】的透明对象。盖满是为了任何一条边带失效
     * 时它都被遍历到，从而触发自绘。 */
    s_canvas = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, UI_LV_W, UI_LV_W);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_TRANSP, 0);
    /* 不可点击、不滚动：它只是一层视觉回应，绝不能吃掉触摸事件。 */
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    /* 常驻可见（不用 HIDDEN，见 canvas_clear 的注释）。空闲时四层
     * opa 全 0，draw_cb 空转返回。 */
    lv_obj_add_event_cb(s_canvas, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    canvas_clear();

    s_timer = lv_timer_create(step_cb, STEP_MS, NULL);
    lv_timer_pause(s_timer);
}

void ui_glow_ping(const ui_glow_style_t *s)
{
    if (!s_canvas || !s) return;
    s_active = *s;
    /* UI_GLOW_KEY 走全谱分光；它是纯 UI 回应，没有状态语义可破坏。
     * 状态提示保留自身色相，只做轻微分光。 */
    s_active_spectrum = (s == &UI_GLOW_KEY);
    if (s_active.color == 0) {          /* UI_GLOW_KEY: 跟随主题强调色 */
        const theme_palette_t *pal = theme_current();
        s_active.color = pal ? pal->accent_claude : 0x2BB3B1;
    }
    /* 连按重启包络，但只持有一份档位——多 hold 一次就再也放不回低刷。 */
    if (!s_holding) { ui_motion_hold(); s_holding = true; }
    s_t0 = lv_tick_get();
    solve_rings(&s_active, 0, s_active_spectrum);
    invalidate_bands();
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}

void ui_glow_sustain(const ui_glow_style_t *s)
{
    if (!s_canvas || !s) return;
    s_sustain    = *s;
    s_sustain_on = true;
    /* ping 正在播时不要打断它：它结束后 settle() 会自动落到 sustain。 */
    if (s_holding) return;
    s_active = s_sustain;
    solve_rings_static(&s_active);
    invalidate_bands();
}

void ui_glow_sustain_clear(void)
{
    if (!s_canvas || !s_sustain_on) return;
    s_sustain_on = false;
    if (s_holding) return;              /* ping 在播，结束时自然收干净 */
    canvas_clear();
    invalidate_bands();
}

/* ── ?glow：定格观察 ─────────────────────────────────────────────
 * 一次 ping 只有 1 s，而整屏截图光传输就要 ~4.7 s——动态过程根本抓不住，
 * 也就没法验证"某一相位到底长什么样"。把包络钉在指定毫秒上，效果就成了
 * 可截图、可比对的静止画面。这也是调设计的手：想看分光最开的那一帧，
 * `?glow 250` 就是了。
 *   ?glow          触发一次正常 ping
 *   ?glow <ms>     定格在包络的第 ms 毫秒（0..PING_MS）
 *   ?glow off      解除定格 */
static void glow_freeze(int32_t ms)
{
    if (!s_canvas) return;
    lv_timer_pause(s_timer);
    drop_tier();
    if (ms < 0) { settle(); return; }
    if (ms > PING_MS) ms = PING_MS;
    s_active = UI_GLOW_KEY;
    s_active_spectrum = true;
    const theme_palette_t *pal = theme_current();
    if (s_active.color == 0) s_active.color = pal ? pal->accent_claude : 0x2BB3B1;
    solve_rings(&s_active, ms * 1000 / PING_MS, true);
    invalidate_bands();
}

static int cmd_glow(const console_args_t *args)
{
    const char *a = (args->argc >= 2) ? args->argv[1] : NULL;
    bsp_display_lock(-1);
    if (!a)                       ui_glow_ping(&UI_GLOW_KEY);
    else if (a[0] == 'o')         glow_freeze(-1);
    else                          glow_freeze((int32_t)atoi(a));
    bsp_display_unlock();
    console_reply_ok("{\"glow\":\"%s\",\"ping_ms\":%d,\"rings\":%d}",
                     a ? a : "ping", PING_MS, RING_N);
    return 0;
}

static const console_cmd_t s_cmd_glow = { "?glow", cmd_glow,
    "dispersion glow: ?glow (ping) | ?glow <ms> (freeze envelope) | ?glow off" };

void ui_glow_register_cmds(void)
{
    console_protocol_register(&s_cmd_glow);
}
