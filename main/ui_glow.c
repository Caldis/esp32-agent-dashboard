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
 * ── 贴边几何 ─────────────────────────────────────────────────────
 * "右侧和底部有间隙"的根因不在辉光，在 ui_screen.h 的一个错误前提：
 * 那里断言可见区只有 466 宽、起点未知，于是贴边元素按 466 方框锚定，
 * 左/上缩 3px 而右/下缩 17px——不对称的缺口正是这么来的。
 * v7.3 用 `?vis` 四边实测：**可见区就是整个 480 坐标空间**（BSP 的
 * BSP_LCD_H_RES 本来也写着 480）。环组因此直接摆在 [0, UI_LV_W-1] 上，
 * 不需要原点补偿、不需要外溢容差。测量链见 ui_screen.h。
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
 *   + 几何改为满空间 + 实测圆角 76 .......... 28.8 ms/帧（半径变大，
 *     ink_band 的圆角折算随之加深——这 2ms 是买"四角真正贴合"的钱）
 * v7.4（用户要"扩散半径更大 + 消散更长"）：
 *   5 层 / step8 / width7（跨度 15→39px）+ PING 950→1700 . 35 ms/帧
 *   + 修正层权重（原二次衰减把最内层压到 opa 8，在地板以下根本没画，
 *     等于付了带宽没换到可见扩散）..................... 38-43 ms/帧
 * 同期排除的死胡同：把圆角 mask 缓存从 4 开到 16 项——推理很顺（缓存
 * 只按半径做键，五层描边要十个键），实测两处皆无收益，台账在
 * sdkconfig.defaults。注意由此可推出一件事：**工作集本来就装不下**
 * （10 个键 vs 4 项），所以"半径常量化"那 6ms 不是靠缓存命中赚来的。
 *
 * 成本的本质是【面积】：四条边带 ~90k px/帧，而带宽的大头不是墨迹深度
 * 而是【圆角折算】（BOX_RADIUS - R_in/√2，见 ink_band）——半径 76 的角
 * 光这一项就占 ~22px 的下限，再加上 39px 的扩散跨度。想再降只有两条路，
 * 都会动到已被用户认可的观感，所以按兵不动：
 *   · 层数减到 4、层宽加到 9（跨度不变、少一次圆角 mask，估 -20%）；
 *   · 层间距【随消散张开】（开场紧凑=便宜且快，深带只出现在慢动作段）。
 * 43ms ≈ 23fps 的代价换来的是这套效果本身——用户看过实机后拍板留下。
 * 真要提速先量，别照抄估算。 */
#define RING_N        5      /* 色散分光的层数 */
#define RING_STEP     8      /* 每层内缩 px（静止姿态） */
#define RING_WIDTH    7      /* 层宽；step-width=1 的窄缝让色相仍可分辨 */
/* 不再需要外溢。v7.3 四边实测：可见区就是整个坐标空间 [0,479]，没有
 * 原点偏移、没有屏外余量（ui_screen.h 有完整测量链）。所以环组就摆在
 * 坐标空间上，边缘即边缘。
 * 历史：这个值曾是 4，用来吃掉"可见区只有 466、起点 a 未知"那套错误
 * 模型里的不确定度——那才是左上/底部被啃掉的原因。 */
#define GLOW_BLEED    0

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
/* 谱的长度与层数【无关】——色相会沿层序流动（spectrum_at），4 层照样
 * 在一次 ping 里扫完整条 5 色谱。 */
#define SPECTRUM_N 5
static const uint32_t SPECTRUM[SPECTRUM_N] = {
    0xFF2A5A,   /* 品红 —— 长波端 */
    0xFF9020,   /* 琥珀 */
    0x2BB3B1,   /* teal —— 家族色 */
    0x4070FF,   /* 蓝 */
    0xA040FF,   /* 紫 —— 短波端 */
};

/* 单色分光的色相偏移（±，作用在 R/B 通道上，近似"这一层偏红/偏蓝"）。 */
static const int8_t DISP_TINT[RING_N] = { +40, +20, 0, -20, -40 };

/* color==0 是"跟随主题强调色"的哨兵（见 ui_glow_ping）。
 * KEY 满扩散 + 全谱；两个状态提示窄扩散 + 单色轻分光。 */
const ui_glow_style_t UI_GLOW_KEY     = { 0x000000, 200, 255 };
const ui_glow_style_t UI_GLOW_WAITING = { 0xE0A030, 150,  96 };
const ui_glow_style_t UI_GLOW_LOST    = { 0xE0503C, 150,  96 };

/* 谱上的连续取色：pos 是定点位置（256 = 一格），在相邻两色间线性插值。
 * v7.4 用它取代"到点整组镜像"的硬切——那一下所有层同帧换色，读起来就是
 * 一次跳变（用户："琥珀色变成蓝紫色那下很突然"）。色序照样会走完一整圈
 * （翻转的意图保留），但走成了过程：色相沿层序流动，像棱镜在慢慢转。 */
static uint32_t spectrum_at(int32_t pos)
{
    int32_t n = (int32_t)SPECTRUM_N;
    int32_t g = ((pos / 256) % n + n) % n;
    int32_t f = ((pos % 256) + 256) % 256;
    uint32_t a = SPECTRUM[g], b = SPECTRUM[(g + 1) % n];
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int32_t ca = (int32_t)((a >> sh) & 0xFF), cb = (int32_t)((b >> sh) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * f / 256) << sh;
    }
    return out;
}

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
 *   000-250  分光张开：位移从第一帧就开始，走【缓出】——前 1/3 就张到
 *            七成。这是 v7.4 的修正：上一版让位移从 t=80‰ 才起步，
 *            于是开场那一瞬五层紧挨着、外侧品红+琥珀糊成一片暖色，
 *            之后才"忽然"散成彩虹——用户报的"先是琥珀色再色散、过渡
 *            生硬"就是这个。棱镜的戏就是【分光】，它不该迟到。
 *   000-100  收束：整组从屏外压进来，亮度缓入到峰值
 *   全程     色相流动：谱沿层序推移一整圈——色序确实前后对调了，
 *            但走成过程而非一帧硬切（v7.4 修正，见 spectrum_at）
 *   380-750  依次失焦：由外向内逐层变宽变淡，像焦点一层层松掉
 *   350-1000 消散：亮度长距离退场 + 整组向外绽开（占全程 65%）
 *   全程     波动：一道正弦沿层序推进，让五层此起彼伏而不是齐明齐灭
 *
 * 排期的铁律（第一版就栽在这）：**效果必须落在还亮着的时段**。最初版
 * 用 1-(1-t)^3 收尾，t=0.71 时亮度已只剩 2%，而失焦/翻转排在 0.45 之
 * 后——等于把最精彩的三个效果演给黑屏看。 */
#define PING_MS      1700     /* v7.4: 950 -> 1700，消散占 65% ≈ 1.1s */
#define STEP_MS        16     /* 与高刷档同拍——这套效果值得满帧 */
#define WAVE_CYCLES     2     /* 波沿层序推进的圈数 */
#define SPREAD_MS     250     /* ‰，位移张到满所需的时间 */

/* 0..1000 定点。sin 用 LVGL 的整数三角（返回 -32767..32767）。 */
static int32_t wave(int32_t phase_deg)
{
    return lv_trigo_sin((int16_t)(phase_deg % 3600 / 10)) / 328;  /* ±100 */
}

/* 把 (样式, 归一化时间) 解算成五层的绘制参数。纯函数——所有效果都在
 * 这里，绘制回调只负责把结果画出来。 */
static void solve_rings(const ui_glow_style_t *st, int32_t t1000, bool spectrum)
{
    /* 色相沿层序推移的相位（定点，256 = 一格）。整段 ping 走满一圈，
     * 于是"色序前后对调"这件事发生了，但是以连续过程的方式发生的。 */
    int32_t travel = t1000 * SPECTRUM_N * 256 / 1000;

    /* 整体亮度：缓入到峰值，短暂保持，然后【长距离】退场。退场用线性
     * 而不是缓出——缓出会把 80% 的亮度在前 30% 的时间里丢掉（见上面的
     * 排期铁律）。v7.4 把退场从 400‰ 拉到 650‰。 */
    int32_t g;
    if (t1000 < 100)      g = wave(900 * t1000 / 100) * 10;   /* 正弦缓入 */
    else if (t1000 < 350) g = 1000;
    else                  g = (1000 - t1000) * 1000 / 650;
    if (g < 0) g = 0;
    if (g > 1000) g = 1000;

    /* 放大：起手从屏外压进来（收束），尾段一路向外绽散——绽散与消散同步
     * 开始，读作"光散掉了"而不是"灯关了"。 */
    int32_t expand = 0;
    if (t1000 < 100)       expand = MAX_EXPAND * (100 - t1000) / 100;
    else if (t1000 > 350)  expand = MAX_EXPAND * (t1000 - 350) / 650;

    /* 分光：位移幅度从【第一帧】起就张开，走正弦缓出（前 1/3 到七成）。
     * 让棱镜的主戏与亮起同时发生，而不是等亮完了再补一个变化。 */
    int32_t spread_amp = t1000 < SPREAD_MS
                       ? MAX_DISP * wave(900 * t1000 / SPREAD_MS) / 100
                       : MAX_DISP;

    for (int i = 0; i < RING_N; ++i) {
        ring_draw_t *r = &s_ring[i];
        int32_t phase = i * 3600 / RING_N;                    /* 层相位 */

        /* 波动：一道正弦沿层序推进（±100）。 */
        int32_t w = wave(t1000 * 36 * WAVE_CYCLES / 100 + phase);

        /* 依次失焦：外层先散、内层后散（层间错峰），380 起 370 内走完。 */
        int32_t defocus = t1000 - 380 - i * 370 / RING_N;
        defocus = defocus < 0 ? 0 : defocus * 1000 / 370;
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
         * 更淡，但只打到 2/3——打太狠效果就演给黑屏看了）。
         * v7.4 层权重改成二次衰减：扩散半径拉大之后，等亮度的宽带读起来
         * 是"一圈粗边框"，只有向内明显变暗才读作"辉光渗进来"。 */
        /* 斜率要克制：k 取到 255-i*255/N 时最内层权重只剩 10/255，乘上
         * peak 后 opa≈8，直接掉到 GLOW_OPA_FLOOR 以下——那一层根本没画，
         * 等于付了带宽却没换到可见的扩散。160 的跨度让最内层保住 ~25%。 */
        int32_t k = 255 - i * 160 / RING_N;          /* 255..127 */
        int32_t weight = k * k / 255;                /* 二次 falloff */
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

        /* 色相沿层序连续流动（见 spectrum_at）。单色的状态提示不参与
         * 流动——它的颜色是状态契约，只做固定的轻微分光。 */
        r->rgb = spectrum ? spectrum_at(i * 256 + travel)
                          : tint_rgb(st->color, DISP_TINT[i]);
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
