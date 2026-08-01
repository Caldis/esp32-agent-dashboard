/*
 * ui_deco — 见 ui_deco.h。（v7.5 静态矩形表 → v7.6 编排引擎。）
 *
 * ── 这一层想要什么 ──────────────────────────────────────────────────
 * 参照系是 Marathon / American Dynamism 那一路"机能装饰"：取景角标、
 * 刻度尺、分段条、伪编号。它们成立的机制不是"元素好看"，而是【用装饰
 * 密度伪造工程史】——真实工业设备满身标记，是因为每个标记背后有一次真
 * 实事件（送检、认证、维修）。远看读作整洁的体块，近看每一处都"有理由"。
 *
 * 关键是【分布】而不是【数量】：沿结构线聚集、大片留白，高方差。均匀铺
 * 满的那一秒，它就从"机能"掉进"纹理"。v7.5 实机上栽过一次——底部 12 格
 * 等宽等距横跨 354 px，读作 progress bar，砍到 6 格并偏置才对。
 *
 * ── 比例：这块面板只能吃 Marathon 1/20 的密度 ───────────────────────
 * 沿用 ui_type.h 的视距口径（1 arcmin = 2.10 px @0.6 m、3.49 px @1.0 m）：
 *
 *   最小笔画    低于 1 arcmin 直接消失 -> @1 m 需 >= 3.5 px ... 取 4 px
 *   块状元素    要读作"形状"而非"一个点"约需 8 arcmin -> @1 m 28 px
 *   Marathon    27" 2560 px @0.5 m 时 1 px ~ 1.6 arcmin，它一个 8 px 的
 *               装饰 = 12.8 arcmin；在本面板 @0.8 m 达到同样张角需 ~33 px
 *
 * 于是【缩放比 ~4x，密度比 ~1/20】。唯一的例外是【线】：长条形的检测阈值
 * 远低于点状，2 px 的 hairline 在 1 m 处依然读得到。**线可以细，块不能小。**
 *
 * ── 运动语法：机械，与内容层的弹簧构成复调 ─────────────────────────
 * 内容层是 spring（ζ=0.68 欠阻尼，有惯性、会过冲）——有机的、亲和的。
 * 装饰层【全程机械】：线性匀速、瞬时阶跃、不回弹、不缓动。两者同时在
 * 演，读起来就是"人在用一台机器"，而这正是这块面板的语义。统一成同一
 * 种运动反而会让装饰融进内容，密度感消失。
 *
 * 原型分野只有一条：
 *   【结构元素】(BRACKET/LINE) 连续【生长】——像绘图仪落笔画线；
 *   【数据元素】(BLOCKS/TICKS/GAUGE) 离散【点亮】——像指示灯逐个通电。
 * 入场编排让四角先锁定、基准线再擦出、数据最后填入，是 CAD 制图的顺序，
 * 也是"仪表框先亮、数据后到"的叙事。
 *
 * 进度量 p ∈ [0,1000] 统一表达"这个元素完成了多少"，绘制时按原型解释。
 * 妙处是【出场不需要单独实现】：p 从 1000 退回 0 时，生长类自动从末端
 * 收回，点亮类自动从最后一个往前熄灭——正是想要的后进先出。
 *
 * ── 持续动效必须跑在 idle 刷新档上 ──────────────────────────────────
 * ui_motion 的静止档是 66 ms。任何"一直在动"的效果如果要求 16 ms 档，
 * 就等于让设备永远待在高刷上烧电。所以停留期的全部效果都是【低频离散
 * 事件】——200 ms 一拍，改几个小矩形的 opa 而已，零额外刷新成本。
 * 而这恰恰与机械语法自洽：机械运动本来就不需要平滑插值。亮段沿线以
 * 200 ms 步长跳着走，读起来是"数据包在传输"，不是卡顿。
 * 只有入场/出场需要 33 ms 档，它们各自 ~0.7 s / ~0.23 s，用完即还。
 *
 * ── 成本 ────────────────────────────────────────────────────────────
 * 全部形状都是矩形，走一个 DRAW_MAIN 自绘对象（ui_glow.c 验证过的路子：
 * 加一个装饰不等于加一个对象，失效范围完全自己说了算）。
 * 两道闸门：
 *   · draw_cb 里的 AABB 预判——本层每帧都会被遍历（冒号闪、glow 刷边带），
 *     但四次整数比较就跳过所有不相交的矩形；
 *   · 失效【按元素】而不是按整层——每个元素一个 bbox，只有视觉签名
 *     (p, ph) 变了的元素才失效自己那一小块。停留期通常只有 1-2 个。
 * v7.5 实测：37 个矩形总墨迹 ~3.7k px = 转场全屏重绘的 1.6%，转场回归门
 * 里 clock 两行 -12% / -1%，成本在噪声内。
 *
 * ── 为什么装饰层是无彩色的 ─────────────────────────────────────────
 * CLAUDE.md 的设备级契约：**颜色跟随 STATE，永远不跟随页面**。参照图里
 * 那身安全橙很好看，但在这台设备上引入第三种彩色就是拿状态契约换视觉。
 * 本层只用主题 text 色，靠【明度分层】拉层次。
 *
 * ── 实机调参 ───────────────────────────────────────────────────────
 * 这类视觉只有一双眼睛能判，而改一次刷一次机太慢：
 *   ?deco g <0-255>   全局 opa 增益，扫"存在感"
 *   ?deco f <ms>      把入场包络定格在第 ms 毫秒——入场只有 0.7 s，而
 *                     一张 480 截图要 5 s，动态过程根本抓不住。同 ?glow。
 */

#include "ui_deco.h"
#include "ui_screen.h"
#include "ui_motion.h"
#include "theme.h"
#include "agent_state.h"

#include "harness/console_protocol.h"
#include "bsp/esp-bsp.h"

#include <stdlib.h>
#include <string.h>

/* ── 尺度常量（由文件头的视距推导而来，不要随手改） ───────────────── */

#define DECO_STROKE     4    /* 最小笔画：@1 m 约 1.15 arcmin，再细就没了 */
#define DECO_HAIR       2    /* hairline —— 线的检测阈值低，可以细 */

/* 四角取景框。面板圆角 76（ui_screen.h 实测），角标【不能贴真角】：
 * 拐点 (32,32) 到圆心 (76,76) 距 62.2 < 76，可见；(20,20) 则是 79.2 > 76，
 * 被切掉。32 是留了余量的安全值。
 * 这也是"面板形状把方形语法往圆形 HUD 推"的第一个落点：直角语法的角标
 * 在这块屏上只能内缩成【内接取景框】，语义反而更正确——crop mark 本来
 * 就是标记裁切线，不是标记边缘。 */
#define BRK_INSET      32
#define BRK_ARM        30    /* 臂长 >= 28 才读得出是个"角" */

/* 底部分段条（GAUGE）：6 格 x 24 宽 + 5 x 6 间距 = 174，靠左起于 63。
 * 格宽 24 px @0.8 m 约 9 arcmin，刚好在"可辨为独立块"的门槛上。
 * y=356，下沿 359 压住 UI_SAFE_BOT(360)，footer 从 392 起，不打架。 */
#define SEG_X0         63
#define SEG_Y         356
#define SEG_W          24
#define SEG_PITCH      30
#define SEG_H           4

/* 明度分层。装饰必须【退到背景】：最亮的 GAUGE 当前格也只有 190/255，
 * 而正文是 COVER。 */
#define OPA_BRK        72
#define OPA_HAIR       36
#define OPA_BLOCK      90
#define OPA_TICK       52
#define OPA_SEG_PAST  100
#define OPA_SEG_CUR   190
#define OPA_SEG_FUT    26

/* ── clock 谱 ────────────────────────────────────────────────────────
 * 矩形表【按元素分组】排列，元素表用切片引用它。分布是刻意做成高方差
 * 的：顶部左密右疏，底部左动右静、中间隔 130 px 空白。 */
static const deco_shape_t CLOCK_SHAPES[] = {
    /* 0-11 角标：**L 臂 + 一段分离的延伸刻度**（每角 3 条）。
     * 三张谱的角标形态【刻意各不相同】——它们原本是同一份 8 矩形拷贝，
     * 三个页面因此长得像同一张模板。这一版按各页语义分化：
     *   clock     L + 分度延伸  —— 精密刻度、读数
     *   dashboard 方括号 [ ]     —— 数据框、通道
     *   weather   单臂交替       —— 最轻、不对称、自然
     * 分度延伸与 L 臂之间留 8 px 缝，读作"角标之后还有一段尺"。 */
    { BRK_INSET, BRK_INSET, BRK_ARM, DECO_STROKE, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { BRK_INSET, BRK_INSET, DECO_STROKE, BRK_ARM, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 76, 76, 62, 3, OPA_BRK, DECO_FWD, DSHAPE_ARC, 200, 250 },
    { 418, 32, BRK_ARM, DECO_STROKE, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 444, 32, DECO_STROKE, BRK_ARM, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 404, 76, 62, 3, OPA_BRK, DECO_FWD, DSHAPE_ARC, 290, 340 },
    { 32, 444, BRK_ARM, DECO_STROKE, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 32, 418, DECO_STROKE, BRK_ARM, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 76, 404, 62, 3, OPA_BRK, DECO_FWD, DSHAPE_ARC, 110, 160 },
    { 418, 444, BRK_ARM, DECO_STROKE, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 444, 418, DECO_STROKE, BRK_ARM, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 404, 404, 62, 3, OPA_BRK, DECO_FWD, DSHAPE_ARC, 20, 70 },

    /* 12   LINE 顶部基准线，横贯 76..404（与面板圆角的直边段同起止）。
     * v7.5 放在 y=96：上方 92 px 全空，实机上读作【悬在半空的一横】。
     * 上移到 64 之后贴住顶缘，与大钟、底部基线形成三层，也不再和 push
     * 卡退到顶槽位的小钟（墨迹中心 ~85）打架。 */
    { 76, 65, 328, DECO_HAIR, OPA_HAIR, DECO_FWD, 0, 0, 0 },

    /* 13-18 BLOCKS 顶部分段块：宽度不等才读作编号，左密右疏给出方向性 */
    { 76,  61, 28, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 111, 61, 10, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 128, 61, 18, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 153, 61, 36, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 196, 61, 14, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 217, 61, 24, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },

    /* 19-23 TICKS 顶部右端刻度：5 根，表里从右往左排，点亮也从右往左 */
    { 401, 60, 3, 12, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 389, 60, 3, 12, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 377, 60, 3, 12, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 365, 60, 3, 12, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 353, 60, 3, 12, OPA_TICK, DECO_FWD, 0, 0, 0 },

    /* 24    LINE 左侧轴（自上而下生长）
     * 25-27 TICKS 左侧齿：3 根，长度不等
     * 两侧【刻意不对称】（左 3 齿右 2 齿、长度不同）——镜像对称读成装饰
     * 花边，错位才读成机能。实机上它俩合起来像一对方括号夹住大钟。 */
    { 24, 176, 3, 66, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 24, 176, 13, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 24, 205,  9, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 24, 239, 13, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },

    /* 28    LINE 右侧轴（自下而上生长 —— 与左侧反向，又一处不对称）
     * 29-30 TICKS 右侧齿：2 根 */
    { 453, 192, 3, 54, OPA_TICK, DECO_REV, 0, 0, 0 },
    { 444, 192, 12, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 444, 243, 12, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },

    /* 31-36 GAUGE 分钟条（opa 由真值现算，表里的值不用） */
    { SEG_X0 + 0 * SEG_PITCH, SEG_Y, SEG_W, SEG_H, 0, DECO_FWD, 0, 0, 0 },
    { SEG_X0 + 1 * SEG_PITCH, SEG_Y, SEG_W, SEG_H, 0, DECO_FWD, 0, 0, 0 },
    { SEG_X0 + 2 * SEG_PITCH, SEG_Y, SEG_W, SEG_H, 0, DECO_FWD, 0, 0, 0 },
    { SEG_X0 + 3 * SEG_PITCH, SEG_Y, SEG_W, SEG_H, 0, DECO_FWD, 0, 0, 0 },
    { SEG_X0 + 4 * SEG_PITCH, SEG_Y, SEG_W, SEG_H, 0, DECO_FWD, 0, 0, 0 },
    { SEG_X0 + 5 * SEG_PITCH, SEG_Y, SEG_W, SEG_H, 0, DECO_FWD, 0, 0, 0 },

    /* 37-39 BLOCKS 底部右端：与 GAUGE 同一条基线，但靠右、恒定、暗。
     * 同一条线上左动右静、左亮右暗、左匀右不匀——这才是要的高方差。 */
    { 369, SEG_Y,  8, SEG_H, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 383, SEG_Y, 20, SEG_H, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 409, SEG_Y,  8, SEG_H, OPA_TICK, DECO_FWD, 0, 0, 0 },

    /* 40-45 分钟条的【游标三角】：每格一个，接同一个槽，靠 GAUGE 原型
     * 的三档 opa 只让当前格那个亮起来——于是"指针"这件事不需要任何动态
     * 坐标，静态表 + 真值就够了。指向下，压在格子上方 8 px。 */
    { 69 +  0, 346, 12, 0, 0, DECO_FWD, DSHAPE_TRI, 6, 8 },
    { 69 + 30, 346, 12, 0, 0, DECO_FWD, DSHAPE_TRI, 6, 8 },
    { 69 + 60, 346, 12, 0, 0, DECO_FWD, DSHAPE_TRI, 6, 8 },
    { 69 + 90, 346, 12, 0, 0, DECO_FWD, DSHAPE_TRI, 6, 8 },
    { 69 +120, 346, 12, 0, 0, DECO_FWD, DSHAPE_TRI, 6, 8 },
    { 69 +150, 346, 12, 0, 0, DECO_FWD, DSHAPE_TRI, 6, 8 },

    /* 46/47 两条侧轴末端的【收束人字】——轴不再突然断掉，而是收在一个
     * 尖上。两臂关于竖直轴镜像，所以是 ∨ 形。 */
    { 25,  254, 7, -7, OPA_TICK, DECO_FWD, DSHAPE_CHEV, 3, 0 },
    { 454, 254, 7, -7, OPA_TICK, DECO_FWD, DSHAPE_CHEV, 3, 0 },
    /* 48 底部基线中段的【对位斜叉】。那 130 px 空白留白是对的，但完全
     * 空着又少一个锚点；一个 × 既是校准标记又不增加密度。 */
    { 300, 358, 6, 3, OPA_TICK, DECO_FWD, DSHAPE_CROSS, 0, 0 },
};

/* 入场编排。四角先依次锁定 -> 基准线擦出 -> 数据逐格填入，总 ~690 ms，
 * 与转场的 IN_MS(520) 同量级：机械层先就位并"等着"内容用弹簧落座。 */
static const deco_elem_t CLOCK_ELEMS[] = {
    { DECO_BRACKET, 0,  3,   0, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 3,  3,  40, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 6,  3,  80, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 9,  2, 120, 180, 0, 0, 0, 0, 0, 0 },
    /* 右下角那条同心弧【单独成元素】，跨角随"这一小时走到哪"生长：
     * 槽 2 = 分钟千分比，一小时扫完 50°。四角看似对称，只有这一角在动
     * ——本层第一处真正的动态几何（几何量由数据决定，而不是靠点亮第几
     * 格模拟）。必须拆出来：整组带 span_slot 会把两条 L 臂一起缩短。 */
    { DECO_BRACKET, 11, 1, 300, 400, 0, 0, 0, 0, UI_DECO_SLOT(2), 0 },
    /* 顶部三件【只在大钟姿态在场】。推送卡弹出时大钟退到顶部槽位，48px
     * 的小钟墨迹落在 61..109——与这条 60..71 的带子完全重叠。第一版没做
     * 姿态区分，实机截图里"00:29"正压在分段块和刻度中间，当时误读成了
     * 巧合的好看，其实是碰撞。让位才是对的。 */
    { DECO_LINE,   12,  1, 170, 240, 0, 0, CLK_ST_FACE, 0, 0, 0 },
    { DECO_BLOCKS, 13,  6, 290,  45, 0, 0, CLK_ST_FACE, 0, 0, 0 },
    { DECO_TICKS,  19,  5, 320,  40, 0, 0, CLK_ST_FACE, 0, 0, 0 },
    /* 侧边两轴常驻：推送卡的内容居中（92px 字形区 + 标题 + chip），
     * x=24 / x=453 两条窄轴离它很远，两个姿态都放得下。 */
    { DECO_LINE,   24,  1, 370, 160, 0, 0, 0, 0, 0, 0 },
    { DECO_TICKS,  25,  3, 420,  45, 0, 0, 0, 0, 0, 0 },
    { DECO_LINE,   28,  1, 390, 160, 0, 0, 0, 0, 0, 0 },
    { DECO_TICKS,  29,  2, 440,  45, 0, 0, 0, 0, 0, 0 },
    /* 槽 0 = 分钟（10 分钟/格）。 */
    { DECO_GAUGE,  31,  6, 480,  35, 0, UI_DECO_SLOT(0), 0, 0, 0, 0 },
    /* 槽 1 = 活跃 agent 数：亮着的【块数】就是读数。rev 让它从右往左
     * 点亮，与左边的 GAUGE 反向，同一条基线上两个方向。 */
    { DECO_BLOCKS, 37,  3, 540,  50, 1, UI_DECO_SLOT(1), 0, 0, 0, 0 },
    { DECO_GAUGE,  40,  6, 510,  35, 0, UI_DECO_SLOT(0), 0, 0, 0, 0 },
    { DECO_BRACKET, 46, 1, 450, 160, 0, 0, DECO_DEFER, 0, 0, 0 },
    { DECO_BRACKET, 47, 1, 470, 160, 0, 0, DECO_DEFER, 0, 0, 0 },
    { DECO_BRACKET, 48, 1, 560, 200, 0, 0, DECO_DEFER, 0, 0, 0 },
};

static const ui_deco_spec_t CLOCK_SPEC = {
    CLOCK_SHAPES, (uint8_t)(sizeof(CLOCK_SHAPES) / sizeof(CLOCK_SHAPES[0])),
    CLOCK_ELEMS, (uint8_t)(sizeof(CLOCK_ELEMS) / sizeof(CLOCK_ELEMS[0])),
};

const ui_deco_spec_t *ui_deco_spec_clock(void) { return &CLOCK_SPEC; }

/* ── dashboard 谱 ────────────────────────────────────────────────────
 * 这一页的中部是 fleet 卡片区（y 134..360，x 28..452），两侧只剩 28 px
 * ——放竖轴+齿会贴着卡片边缘打架。所以 dashboard 的装饰形态是【上下两
 * 条仪表带夹住内容】，完全避开中央：
 *   上带 y 106..120 —— 顶部小钟墨迹到 90，安全带从 134 起，中间这条
 *                      横带整条是空的；
 *   下带 y 368..384 —— 安全带下沿 360，footer 数字从 392 起。
 * 两条带都接真状态，这一页因此是三张谱里"仪表感"最强的。 */
static const deco_shape_t DASH_SHAPES[] = {
    /* 0-11 角标：**方括号 [ ]**（竖轴 + 上下两个短横），不是 clock 那种
     * crop mark。这一页的语义是"框住数据通道"，方括号正是那个语汇；
     * 而且它把重复率最高的那份 8 矩形拷贝彻底换掉了。
     * nav_dots 在 y=20 的 x 110/240/370，与之不重叠。 */
    { 32,  32,  4, 34, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 32,  32, 16,  4, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 32,  62, 16,  4, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 444, 32,  4, 34, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 432, 32, 16,  4, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 432, 62, 16,  4, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 32,  414, 4, 34, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 32,  414, 16, 4, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 32,  444, 16, 4, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 444, 414, 4, 34, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 432, 414, 16, 4, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 432, 444, 16, 4, OPA_BRK, DECO_REV, 0, 0, 0 },

    /* ── 上带：与顶部时间【同一水平线、左右分列】 ────────────────
     * 原先横跨 y112，deco_audit 量出它距时间墨底只有 1 px（fleet）/
     * 5 px（chip）——我目测时以为时间墨底在 90，实际是 107。那里根本
     * 放不下：107 到卡片顶 134 只有 27 px，塞 14 px 的带子两边各剩 6。
     * 改成左右两段、垂直与时间墨迹（56..107，中心 81）居中对齐，让开
     * 中央。上下各得 44/46 px，三个姿态都宽裕，dy 偏移也不再需要。
     * 构图上也更好：时间被两段数据带夹持，比横跨一条更像仪表。
     * 12 左段基准线（x40..170） */
    { 40, 81, 130, DECO_HAIR, OPA_HAIR, DECO_FWD, 0, 0, 0 },
    /* 13-16 左段 BLOCKS —— 槽 0 = 在册会话数，亮着的块数即读数 */
    { 40,  77, 24, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 70,  77, 12, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 88,  77, 18, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 112, 77, 28, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    /* 17-21 右段 METER —— 槽 1 = 活跃数。x 递增所以液面从左往右升。 */
    { 345, 75, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 357, 75, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 369, 75, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 381, 75, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 393, 75, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },

    /* 22 下带基准线 */
    { 76, 378, 328, DECO_HAIR, OPA_HAIR, DECO_FWD, 0, 0, 0 },
    /* 23-25 下带左端 BLOCKS（静态，节奏与上带不同） */
    { 76,  374, 20, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 102, 374, 10, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    { 118, 374, 28, 10, OPA_BLOCK, DECO_FWD, 0, 0, 0 },
    /* 26-29 下带右端 TICKS（扫描） */
    { 401, 372, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 389, 372, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 377, 372, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 365, 372, 3, 14, OPA_TICK, DECO_FWD, 0, 0, 0 },

    /* ── 以下只在 AMBIENT 姿态在场 ──────────────────────────────────
     * 30-35 中央夹持括号：呼吸环墨迹约 x204..276、y172..244，括号摆在
     * 它左右各 31 px 处——刚好一个 UI_GAP_LG 量级的负空间，不贴脸也不
     * 失去关联。fleet 姿态下这里是卡片，括号必须让位。 */
    { 162, 188,  3, 42, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 162, 188, 11,  3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 162, 227, 11,  3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 315, 188,  3, 42, OPA_TICK, DECO_REV, 0, 0, 0 },
    { 307, 188, 11,  3, OPA_TICK, DECO_REV, 0, 0, 0 },
    { 307, 227, 11,  3, OPA_TICK, DECO_REV, 0, 0, 0 },
    /* 36-41 侧边两轴。fleet 卡片占 x28..452，两侧只剩 28 px：几何上放得
     * 下，视觉上放不下——四行卡片已经把画面填满，再贴两条轴就没有喘息
     * 了。所以它们只属于 ambient。 */
    { 14,  214,  3, 52, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 14,  214,  9,  3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 14,  263,  9,  3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 463, 224,  3, 44, OPA_TICK, DECO_REV, 0, 0, 0 },
    { 455, 224,  9,  3, OPA_TICK, DECO_REV, 0, 0, 0 },
    { 455, 265,  9,  3, OPA_TICK, DECO_REV, 0, 0, 0 },

    /* 42-47 上带中段的【45° 斜线束】。这是工程制图的剖面线语汇（参照
     * 图里那个斜纹按钮就是它），也是本层第一处【带角度】的图形——在此
     * 之前整层只有横竖两个方向。顺带填掉上带中间那段 90 px 空白，而且
     * 因为斜纹是"这一段被划掉/保留"的意思，密度感来得比再加几个方块
     * 更便宜。 */
    { 200, 386, 14, -14, OPA_TICK, DECO_FWD, DSHAPE_LINE, 2, 0 },
    { 216, 386, 14, -14, OPA_TICK, DECO_FWD, DSHAPE_LINE, 2, 0 },
    { 232, 386, 14, -14, OPA_TICK, DECO_FWD, DSHAPE_LINE, 2, 0 },
    { 248, 386, 14, -14, OPA_TICK, DECO_FWD, DSHAPE_LINE, 2, 0 },
    { 264, 386, 14, -14, OPA_TICK, DECO_FWD, DSHAPE_LINE, 2, 0 },
    { 280, 386, 14, -14, OPA_TICK, DECO_FWD, DSHAPE_LINE, 2, 0 },

    /* 48/49 两条基准线左端的【端子圆环】——线不再凭空开始，而是从一个
     * 接点引出。r=5 恒定，圆角 mask 的键因此稳定。 */
    { 32, 81, 5, 2, OPA_BLOCK, DECO_FWD, DSHAPE_DOT, 0, 0 },
    { 66, 379, 5, 2, OPA_BLOCK, DECO_FWD, DSHAPE_DOT, 0, 0 },

    /* 50/51 斜线束两端的【对位斜叉】：给那段剖面标出起止，和 crop mark
     * 是同一族语汇。52 下带块组末尾的【节点菱形】——菱形读作枢纽，与
     * 圆环（端子）分工不同。 */
    { 192, 379, 5, 3, OPA_TICK,  DECO_FWD, DSHAPE_CROSS, 0, 0 },
    { 302, 379, 5, 3, OPA_TICK,  DECO_FWD, DSHAPE_CROSS, 0, 0 },
    { 160, 379, 5, 5, OPA_BLOCK, DECO_FWD, DSHAPE_DIAMOND, 0, 0 },
};

/* dy_a / dy_b = PLAIN / CHIP 两个子姿态的让位量（FLEET 用基准位置）。
 * 逐姿态实测验收后定的数，不是估的：
 *   FLEET  上带 16/14、下带 12/12（基准）
 *   PLAIN  上带 +18 -> 34/34；下带 -26 -> 31/38
 *   CHIP   上带 +4  -> 20/21；下带 -4  -> 19/20
 * CHIP 的量必须单列：awaiting 时整簇上移 27 px（环墨顶 172->145）且
 * chip 墨底压到 ~340，套用 PLAIN 的偏移会让上带贴到环上、下带压住 chip。
 * 两条带的 y 本来按 fleet 定（卡片顶 134 / 底 360 是硬边界），可 ambient
 * 中央空出一大片，同一个 y 就成了"贴着 chrome、离内容 52 px"。实测
 * ambient 下：时间墨底 90→上带 16 px、上带→呼吸环 52 px；词底 315→下带
 * 53 px、下带→footer 16 px。偏移后两侧各约 34 px。 */
static const deco_elem_t DASH_ELEMS[] = {
    { DECO_BRACKET, 0,  3,   0, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 3,  3,  40, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 6,  3,  80, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 9,  3, 120, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_LINE,   12,  1, 170, 240, 0, 0, 0, 0, 0, 0 },
    { DECO_BLOCKS, 13,  4, 290,  50, 0, UI_DECO_SLOT(0), 0, 0, 0, 0 },
    { DECO_METER,  17,  5, 320,  40, 0, UI_DECO_SLOT(1), 0, 0, 0, 0 },
    { DECO_LINE,   22,  1, 380, 240, 0, 0, 0, -26, -4, 0 },
    { DECO_BLOCKS, 23,  3, 470,  50, 0, 0, 0, -26, -4, 0 },
    { DECO_TICKS,  26,  4, 500,  40, 0, 0, 0, -26, -4, 0 },
    /* AMBIENT 专属。中央括号用 BRACKET 原型是有意的：它带"重锁定"闪，
     * 而它俩正夹着呼吸环——状态一变，环和括号一起应答。 */
    { DECO_BRACKET, 30, 3, 430, 200, 0, 0, DASH_ST_AMBIENT | DECO_DEFER, 0, 0, 0 },
    { DECO_BRACKET, 33, 3, 460, 200, 0, 0, DASH_ST_AMBIENT | DECO_DEFER, 0, 0, 0 },
    { DECO_LINE,    36, 1, 400, 180, 0, 0, DASH_ST_AMBIENT | DECO_DEFER, 0, 0, 0 },
    { DECO_TICKS,   37, 2, 470,  50, 0, 0, DASH_ST_AMBIENT | DECO_DEFER, 0, 0, 0 },
    { DECO_LINE,    39, 1, 420, 180, 0, 0, DASH_ST_AMBIENT | DECO_DEFER, 0, 0, 0 },
    { DECO_TICKS,   40, 2, 490,  50, 0, 0, DASH_ST_AMBIENT | DECO_DEFER, 0, 0, 0 },
    /* 斜线束当 TICKS 演：逐条瞬亮 + 停留期一道扫描掠过，正是剖面线该有
     * 的读法（一条条画上去，不是整块淡入）。 */
    { DECO_TICKS,   42, 6, 350,  40, 0, 0, 0, -26, -4, 0 },
    { DECO_BLOCKS,  48, 1, 150, 120, 0, 0, DECO_DEFER, 0, 0, 0 },
    { DECO_BLOCKS,  49, 1, 360, 120, 0, 0, DECO_DEFER, -26, -4, 0 },
    { DECO_BRACKET, 50, 2, 390, 160, 0, 0, DECO_DEFER, -26, -4, 0 },
    { DECO_BLOCKS,  52, 1, 520, 120, 0, 0, DECO_DEFER, -26, -4, 0 },
};

static const ui_deco_spec_t DASH_SPEC = {
    DASH_SHAPES, (uint8_t)(sizeof(DASH_SHAPES) / sizeof(DASH_SHAPES[0])),
    DASH_ELEMS, (uint8_t)(sizeof(DASH_ELEMS) / sizeof(DASH_ELEMS[0])),
};

const ui_deco_spec_t *ui_deco_spec_dashboard(void) { return &DASH_SPEC; }

/* ── weather 谱 ──────────────────────────────────────────────────────
 * 三张里最克制的一张：这一页的内容密度最高（插画 36..176 / 温度 HERO /
 * 装饰星 190..250 / 五日条带 306..420），中部与顶部都没有余地——顶部左
 * 有地名、右有角落小钟、中间是星组。所以只留【边缘骨架】：四角 + 左右
 * 两道短轴 + 底部一条基线。
 * 这是"装饰密度与内容密度反向"的直接应用：内容满的页面，装饰退到只剩
 * 结构；否则两层一起挤，谁都读不出来。 */
static const deco_shape_t WX_SHAPES[] = {
    /* 0-3 角标：**每角只有一条臂，方向交替**（左上横 / 右上竖 / 左下竖 /
     * 右下横）。既不是 clock 的 L+分度，也不是 dashboard 的方括号——这
     * 一页内容最满，角标必须最轻；交替方向让四角不闭合成"框"，读起来
     * 是自然的、非仪器的，正合气象页的语气。四个矩形，clock 用了十二个。 */
    { 32,  32, 34,  4, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 444, 32,  4, 34, OPA_BRK, DECO_FWD, 0, 0, 0 },
    { 32, 414,  4, 34, OPA_BRK, DECO_REV, 0, 0, 0 },
    { 414, 444, 34, 4, OPA_BRK, DECO_REV, 0, 0, 0 },

    /* 4-6 左侧【观测弧】+ 2 齿。圆心推到屏外 (-40,240)、半径 70、跨 80°，
     * 于是一段凸向画面的弧占住 x13..30。
     * 第一版是"与屏幕同心"的 r=228 弧——概念上很顺，实机上【就是一条
     * 直线】：矢高只有 r(1-cos10°)=3.5 px，付了弧的绘制成本（走圆角
     * mask，比 rect fill 贵得多）却没换到任何曲率。要读作弧，矢高/弧长
     * 得有量级（这条是 20%），而屏幕边缘没有那么多空间摆大跨度——所以
     * 把圆心推到屏外、用小半径大角度。 */
    { -40, 240, 70, 3, OPA_TICK, DECO_FWD, DSHAPE_ARC, 320, 400 },
    { 14, 193, 10, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 14, 285, 10, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    /* 7-9 右侧观测弧 + 2 齿（自下而上扫，与左弧反向） */
    { 520, 240, 70, 3, OPA_TICK, DECO_REV, DSHAPE_ARC, 140, 220 },
    { 456, 193, 10, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 456, 285, 10, 3, OPA_TICK, DECO_FWD, 0, 0, 0 },

    /* 10 中缝基准线。**不要放到底部**：第一版摆在 y=447，实机上整条
     * 消失——ui_glow 的边缘环组占着最外圈约 46 px（5 层 x step8+width7，
     * 加运动外溢），443 正好落在环带里被压掉，掉线时那圈琥珀更是把它
     * 完全盖住。这块屏的【外圈 46 px 属于状态辉光】，装饰不能进。
     * 改放插画底（278）与五日条带顶（306）之间那条 28 px 的缝：既避开
     * 辉光，又顺手成了"当下"与"预报"两区的分隔线，有结构意义。 */
    { 100, 293, 140, DECO_HAIR, OPA_HAIR, DECO_FWD, 0, 0, 0 },
    /* 11-16 中缝 GAUGE —— 槽 0 = 一天的时段（4 小时/格） */
    { 100, 289, 20, 4, 0, DECO_FWD, 0, 0, 0 },
    { 126, 289, 20, 4, 0, DECO_FWD, 0, 0, 0 },
    { 152, 289, 20, 4, 0, DECO_FWD, 0, 0, 0 },
    { 178, 289, 20, 4, 0, DECO_FWD, 0, 0, 0 },
    { 204, 289, 20, 4, 0, DECO_FWD, 0, 0, 0 },
    { 230, 289, 20, 4, 0, DECO_FWD, 0, 0, 0 },
    /* 17-19 中缝右端静态块 */
    { 320, 289,  8, 4, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 332, 289, 18, 4, OPA_TICK, DECO_FWD, 0, 0, 0 },
    { 356, 289,  8, 4, OPA_TICK, DECO_FWD, 0, 0, 0 },
    /* 20 中缝线右端外的【观测圆环】——把这条分隔线收在一个点上。 */
    { 400, 292, 6, 2, OPA_TICK, DECO_FWD, DSHAPE_DOT, 0, 0 },

    /* 21 中缝的右半段改【虚线】。这一页左边是当下（实测），右边是五日
     * 预报（推断）——虚线在制图里正是"推断/参考"的意思，语义与版面刚好
     * 对上，是这一层少见的形状与内容同构的地方。
     * 22 实线转虚线的交界点放一个【菱形】：那个点就是"现在"。 */
    { 240, 294, 140, 0, OPA_HAIR, DECO_FWD, DSHAPE_LINE, 2, 6 },
    { 240, 293, 5, 5, OPA_BLOCK, DECO_FWD, DSHAPE_DIAMOND, 0, 0 },
};

static const deco_elem_t WX_ELEMS[] = {
    { DECO_BRACKET, 0,  1,   0, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 1,  1,  40, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 2,  1,  80, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_BRACKET, 3,  1, 120, 180, 0, 0, 0, 0, 0, 0 },
    { DECO_LINE,    4,  1, 200, 200, 0, 0, 0, 0, 0, 0 },
    { DECO_TICKS,   5,  2, 300,  60, 0, 0, 0, 0, 0, 0 },
    { DECO_LINE,    7,  1, 230, 200, 0, 0, 0, 0, 0, 0 },
    { DECO_TICKS,   8,  2, 330,  60, 0, 0, 0, 0, 0, 0 },
    { DECO_LINE,   10,  1, 380, 220, 0, 0, 0, 0, 0, 0 },
    { DECO_GAUGE,  11,  6, 470,  35, 0, UI_DECO_SLOT(0), 0, 0, 0, 0 },
    { DECO_BLOCKS, 17,  3, 530,  50, 1, 0, 0, 0, 0, 0 },
    { DECO_BLOCKS, 20,  1, 560, 120, 0, 0, DECO_DEFER, 0, 0, 0 },
    { DECO_LINE,   21,  1, 420, 240, 0, 0, DECO_DEFER, 0, 0, 0 },
    { DECO_BLOCKS, 22,  1, 500, 120, 0, 0, DECO_DEFER, 0, 0, 0 },
};

static const ui_deco_spec_t WX_SPEC = {
    WX_SHAPES, (uint8_t)(sizeof(WX_SHAPES) / sizeof(WX_SHAPES[0])),
    WX_ELEMS, (uint8_t)(sizeof(WX_ELEMS) / sizeof(WX_ELEMS[0])),
};

const ui_deco_spec_t *ui_deco_spec_weather(void) { return &WX_SPEC; }

/* ── 引擎 ────────────────────────────────────────────────────────── */

#define DECO_ELEM_MAX   24
#define DECO_INST_MAX    4

#define TICK_LIVE_MS   200   /* 停留期：必须够慢，好待在 idle 刷新档里 */
/* 入场/出场期的拍频。33 -> 50 -> 66，两步都是实测驱动的：
 *  · 装饰的运动语法本来就是【机械阶跃】，画的又全是直边矩形，没有需要
 *    平滑插值的曲线，15 fps 与 30 fps 在观感上几乎不可辨；
 *  · 66 = ui_motion 的【静止档周期】，于是本层不再需要持有高刷档：每一
 *    拍的变化恰好对应一个刷新周期。之前 hold 高刷（16 ms）却只有 50 ms
 *    一拍，三分之二的刷新周期找不到脏区域纯空转——这正是 CLAUDE.md v7.0
 *    那条教训的镜像（当年是动画慢于刷新而丢帧，这里是刷新快于动画而空
 *    转）。顺带省电：入场不再把设备钉在高刷上。 */
#define TICK_ANIM_MS    66
#define OUT_SCALE       33   /* 出场 = 入场时长的 33%，须 <= OUT_MS(240) */
/* 锁定确认：一道窄强调按入场顺序扫过全层。比入场快，读作"确认"而不是
 * "重新出场"。ph 的第 8 位借来标记"本元素正被扫到"，于是它自动进入
 * 失效签名——不需要另一套脏管理。 */
#define ALERT_MS       520
#define ALERT_W        180   /* 强调窗口宽度（ms） */
#define ALERT_BIT      0x100

typedef enum { PH_HIDDEN = 0, PH_IN, PH_LIVE, PH_OUT } deco_phase_t;

typedef struct {
    int16_t   p;         /* 完成度 0..1000 */
    int16_t   ph;        /* 停留期相位 */
    int16_t   p_seen;    /* 上次失效时的签名，用来判断"要不要重画" */
    int16_t   ph_seen;
    uint16_t  in_at;     /* 入场起点 ms（= delay） */
    uint16_t  in_span;   /* 入场总时长 ms */
    uint16_t  out_at;    /* 出场起点 ms（LIFO 反转后） */
    uint16_t  out_span;
    lv_area_t bbox;
} deco_rt_t;

struct ui_deco {
    lv_obj_t             *obj;
    const ui_deco_spec_t *spec;
    deco_phase_t          phase;
    uint32_t              t0;        /* 当前相位起点 tick */
    uint32_t              span;      /* 当前相位总时长 ms */
    uint32_t              lead;      /* 入场前的让路时间（见 ui_deco_intro） */
    uint32_t              tick;      /* 停留期拍数 */
    bool                  holding;   /* 是否持有高刷档（必须成对） */
    int                   slot[UI_DECO_SLOT_N];   /* 场景写入的真值 */
    uint8_t               pulse_left;             /* 事件脉冲剩余拍数 */
    uint8_t               state;                  /* 当前姿态位 */
    uint8_t               pace;                   /* AGENT_ATTN_*：节奏档 */
    uint16_t              alert_ms;               /* 锁定确认：剩余毫秒 */
    uint16_t              max_delay;              /* 谱里最大的入场 delay */
    bool                  morphing;               /* 有元素正在进/退场 */
    deco_rt_t             e[DECO_ELEM_MAX];
};

/* 本元素在当前姿态里是否在场。姿态位为 0 = 常驻（DECO_DEFER 不算姿态）。 */
static bool elem_on(const struct ui_deco *d, const deco_elem_t *el)
{
    uint8_t m = el->mask & DECO_ST_MASK;
    return m == 0 || (m & d->state) != 0;
}

/* 几何调制：把 span_slot 的千分比乘进进度。槽值 <0（无数据）按满算，
 * 否则元素会在数据到来前完全消失——装饰应当先在场，再被数据修剪。 */
static int32_t elem_span_p(const struct ui_deco *d, const deco_elem_t *el,
                           int32_t p)
{
    if (!el->span_slot || el->span_slot > UI_DECO_SLOT_N) return p;
    int v = d->slot[el->span_slot - 1];
    if (v < 0) return p;
    if (v > 1000) v = 1000;
    return p * v / 1000;
}

/* 参见 DECO_DEFER：转场入场时按兵不动，落定后由 morphing 补上。 */
static bool elem_deferred(const deco_elem_t *el)
{
    return (el->mask & DECO_DEFER) != 0;
}

/* 姿态 A 下的整组 y 偏移（见 deco_elem_t.dy_a）。 */
static int32_t elem_dy(const struct ui_deco *d, const deco_elem_t *el)
{
    if (d->state & DECO_ST_A) return el->dy_a;
    if (d->state & DECO_ST_B) return el->dy_b;
    return 0;
}

/* 元素读到的槽值；未接槽（slot==0）返回 -1 = "不受控"。 */
static int slot_of(const struct ui_deco *d, const deco_elem_t *el)
{
    if (!el->slot || el->slot > UI_DECO_SLOT_N) return -1;
    return d->slot[el->slot - 1];
}

static struct ui_deco s_inst[DECO_INST_MAX];
static int         s_inst_n;
static lv_timer_t *s_timer;

static bool     s_on    = true;
static uint8_t  s_gain  = 255;
static int32_t  s_freeze = -1;      /* >=0: 入场包络定格在这一毫秒 */

/* ── 节奏档（idle / busy / alert）────────────────────────────────────
 * 同一个设备状态，内容层用颜色说（金/teal/暗），装饰层用【时间】说。
 * 空闲时压到七成亮度、重锁定 9 s 一次，几乎不出声；该你了时提到 1.25 倍
 * 并把周期压到 2.4 s，整层明显"急"起来——不用借一丝颜色。 */
static const uint8_t  PACE_BRK[3]   = { 45, 30, 12 };  /* 重锁定周期（拍） */
static const uint8_t  PACE_PAUSE[3] = { 20, 12,  4 };  /* 扫描/掠过的静默 */
/* 亮度乘数（/255）。alert 档要【提亮】所以超过 255，用 uint16 避免截断。 */
static const uint16_t PACE_MUL[3]   = { 180, 255, 320 };

static lv_opa_t gained(const struct ui_deco *d, int32_t base)
{
    if (base < 0) base = 0;
    base = base * PACE_MUL[d->pace < 3 ? d->pace : 1] / 255;
    if (base > 255) base = 255;
    return (lv_opa_t)(base * s_gain / 255);
}

/* 确定性 hash（BLOCKS 的偶发闪变要"看起来随机"但必须可复现）。 */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* 生长类只有一个矩形在动，点亮类每格一个阈值——两者都归一到 p。 */
static bool arch_is_grow(uint8_t a)
{
    return a == DECO_BRACKET || a == DECO_LINE;
}

/* ── 几何：把一个矩形按 p 解算成"当前该画多大" ───────────────────── */
static bool on_clip(const lv_area_t *a, const lv_area_t *c)
{
    return !(a->x1 > c->x2 || a->x2 < c->x1 || a->y1 > c->y2 || a->y2 < c->y1);
}

/* 形状的【满尺寸】包围盒——失效用（生长途中只会更小）。
 * 弧【必须】走 lv_draw_arc_get_area：按整圆估会得到 2r 见方的盒子，
 * weather 那条 r=228 的同心弧一失效就是大半个屏。 */
static void shape_bbox(const deco_shape_t *sh, lv_area_t *o)
{
    switch (sh->kind) {
    case DSHAPE_LINE: {
        int32_t x2 = sh->x + sh->w, y2 = sh->y + sh->h;
        int32_t pad = (sh->a ? sh->a : 2) + 1;
        o->x1 = LV_MIN(sh->x, x2) - pad; o->y1 = LV_MIN(sh->y, y2) - pad;
        o->x2 = LV_MAX(sh->x, x2) + pad; o->y2 = LV_MAX(sh->y, y2) + pad;
        break;
    }
    case DSHAPE_ARC:
        lv_draw_arc_get_area(sh->x, sh->y, (uint16_t)sh->w,
                             sh->a, sh->b, sh->h, false, o);
        break;
    case DSHAPE_TRI: {
        int32_t x2 = sh->x + sh->w, y2 = sh->y + sh->h;
        int32_t x3 = sh->x + sh->a, y3 = sh->y + sh->b;
        o->x1 = LV_MIN(sh->x, LV_MIN(x2, x3)); o->y1 = LV_MIN(sh->y, LV_MIN(y2, y3));
        o->x2 = LV_MAX(sh->x, LV_MAX(x2, x3)); o->y2 = LV_MAX(sh->y, LV_MAX(y2, y3));
        break;
    }
    case DSHAPE_DOT:
        o->x1 = sh->x - sh->w; o->y1 = sh->y - sh->w;
        o->x2 = sh->x + sh->w; o->y2 = sh->y + sh->w;
        break;
    case DSHAPE_CHEV: {
        int32_t pad = (sh->a ? sh->a : 2) + 1;
        o->x1 = sh->x - LV_ABS(sh->w) - pad;
        o->y1 = LV_MIN(sh->y, sh->y + sh->h) - pad;
        o->x2 = sh->x + LV_ABS(sh->w) + pad;
        o->y2 = LV_MAX(sh->y, sh->y + sh->h) + pad;
        break;
    }
    case DSHAPE_CROSS: {
        int32_t pad = (sh->h ? sh->h : 2) + 1;
        o->x1 = sh->x - sh->w - pad; o->y1 = sh->y - sh->w - pad;
        o->x2 = sh->x + sh->w + pad; o->y2 = sh->y + sh->w + pad;
        break;
    }
    case DSHAPE_DIAMOND: {
        int32_t hh = sh->h ? sh->h : sh->w;
        o->x1 = sh->x - sh->w; o->y1 = sh->y - hh;
        o->x2 = sh->x + sh->w; o->y2 = sh->y + hh;
        break;
    }
    default:
        o->x1 = sh->x; o->y1 = sh->y;
        o->x2 = sh->x + sh->w - 1; o->y2 = sh->y + sh->h - 1;
    }
}

/* 画一个形状。p 是所属元素的完成度；生长类按 kind 各自解释：
 *   RECT 沿长轴裁剪 / LINE 终点插值 / ARC 扫角 / TRI、DOT 到点即现。
 * rdsc 由调用方初始化一次复用（lv_draw_rect_dsc_t 很大，逐个 memset
 * 是白花钱）；line/arc/tri 的 dsc 在分支内声明，栈峰值只有一个。 */
/* 画一条线段。LINE / CHEV / CROSS 共用；dash>0 走 LVGL 原生虚线。 */
static void draw_seg(lv_layer_t *layer, lv_color_t col, lv_opa_t opa,
                     int32_t width, int32_t dash,
                     int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.opa   = opa;
    ld.width = width ? width : 2;
    if (dash > 0) { ld.dash_width = dash; ld.dash_gap = dash; }
    ld.p1.x = x1; ld.p1.y = y1;
    ld.p2.x = x2; ld.p2.y = y2;
    lv_draw_line(layer, &ld);
}

static void draw_shape(lv_layer_t *layer, const lv_area_t *clip,
                       lv_draw_rect_dsc_t *rdsc, lv_color_t col,
                       const deco_shape_t *sh0, bool grow, int32_t p,
                       lv_opa_t opa, int32_t dy)
{
    deco_shape_t shb;
    const deco_shape_t *sh = sh0;
    if (dy) { shb = *sh0; shb.y = (int16_t)(shb.y + dy); sh = &shb; }
    switch (sh->kind) {
    case DSHAPE_LINE: {
        int32_t ex = sh->x + (grow ? sh->w * p / 1000 : sh->w);
        int32_t ey = sh->y + (grow ? sh->h * p / 1000 : sh->h);
        if (ex == sh->x && ey == sh->y) return;
        int32_t pad = (sh->a ? sh->a : 2) + 1;
        lv_area_t bb = { LV_MIN(sh->x, ex) - pad, LV_MIN(sh->y, ey) - pad,
                         LV_MAX(sh->x, ex) + pad, LV_MAX(sh->y, ey) + pad };
        if (!on_clip(&bb, clip)) return;
        draw_seg(layer, col, opa, sh->a, sh->b, sh->x, sh->y, ex, ey);
        return;
    }
    case DSHAPE_ARC: {
        int32_t a1 = grow ? (sh->a + (sh->b - sh->a) * p / 1000) : sh->b;
        if (a1 == sh->a) return;
        lv_area_t bb;
        lv_draw_arc_get_area(sh->x, sh->y, (uint16_t)sh->w, sh->a, a1,
                             sh->h, false, &bb);
        if (!on_clip(&bb, clip)) return;
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.color = col;  ad.opa = opa;
        ad.width = sh->h;
        /* 半径【恒定】：生长扫的是角度。半径是圆角 mask 缓存的键。 */
        ad.radius = (uint16_t)sh->w;
        ad.center.x = sh->x; ad.center.y = sh->y;
        ad.start_angle = sh->a; ad.end_angle = a1;
        lv_draw_arc(layer, &ad);
        return;
    }
    case DSHAPE_TRI: {
        if (p <= 0) return;                    /* 多边形不生长，到点即现 */
        lv_area_t bb; shape_bbox(sh, &bb);
        if (!on_clip(&bb, clip)) return;
        lv_draw_triangle_dsc_t td;
        lv_draw_triangle_dsc_init(&td);
        td.color = col;  td.opa = opa;
        td.p[0].x = sh->x;          td.p[0].y = sh->y;
        td.p[1].x = sh->x + sh->w;  td.p[1].y = sh->y + sh->h;
        td.p[2].x = sh->x + sh->a;  td.p[2].y = sh->y + sh->b;
        lv_draw_triangle(layer, &td);
        return;
    }
    case DSHAPE_DOT: {
        if (p <= 0) return;
        lv_area_t bb; shape_bbox(sh, &bb);
        if (!on_clip(&bb, clip)) return;
        rdsc->radius       = LV_RADIUS_CIRCLE;
        rdsc->bg_opa       = sh->h ? LV_OPA_TRANSP : opa;
        rdsc->border_opa   = sh->h ? opa : LV_OPA_TRANSP;
        rdsc->border_width = sh->h;
        rdsc->border_color = col;
        lv_draw_rect(layer, rdsc, &bb);
        rdsc->radius     = 0;          /* 还原共享的 dsc */
        rdsc->border_opa = LV_OPA_TRANSP;
        return;
    }
    case DSHAPE_CHEV: {
        /* 两臂自尖端生长——与 BRACKET 同一个手势，只是带角度。 */
        int32_t ax = grow ? sh->w * p / 1000 : sh->w;
        int32_t ay = grow ? sh->h * p / 1000 : sh->h;
        if (ax == 0 && ay == 0) return;
        int32_t pad = (sh->a ? sh->a : 2) + 1;
        lv_area_t bb = { sh->x - LV_ABS(sh->w) - pad,
                         LV_MIN(sh->y, sh->y + sh->h) - pad,
                         sh->x + LV_ABS(sh->w) + pad,
                         LV_MAX(sh->y, sh->y + sh->h) + pad };
        if (!on_clip(&bb, clip)) return;
        draw_seg(layer, col, opa, sh->a, 0, sh->x, sh->y, sh->x + ax, sh->y + ay);
        draw_seg(layer, col, opa, sh->a, 0, sh->x, sh->y, sh->x - ax, sh->y + ay);
        return;
    }
    case DSHAPE_CROSS: {
        int32_t r = grow ? sh->w * p / 1000 : sh->w;   /* 四臂自中心张开 */
        if (r <= 0) return;
        lv_area_t bb; shape_bbox(sh, &bb);
        if (!on_clip(&bb, clip)) return;
        if (sh->a) {                                    /* 斜叉：真画线 */
            draw_seg(layer, col, opa, sh->h, 0, sh->x - r, sh->y - r,
                     sh->x + r, sh->y + r);
            draw_seg(layer, col, opa, sh->h, 0, sh->x - r, sh->y + r,
                     sh->x + r, sh->y - r);
        } else {
            /* 正十字【不需要抗锯齿线】——它就是两个轴对齐矩形，走最便宜
             * 的 fill 路径。这块板上 AA 线是最贵的绘制之一（weather 那
             * 30 条线的教训），能用 rect 表达的形状就不要交给 draw_line。
             * 作为对位标记 + 与 × 语义等价，所以谱里默认用正十字。 */
            int32_t hw = (sh->h ? sh->h : 2) / 2;
            if (hw < 1) hw = 1;
            lv_area_t ah = { sh->x - r, sh->y - hw, sh->x + r, sh->y + hw };
            lv_area_t av = { sh->x - hw, sh->y - r, sh->x + hw, sh->y + r };
            rdsc->bg_opa = opa;
            lv_draw_rect(layer, rdsc, &ah);
            lv_draw_rect(layer, rdsc, &av);
        }
        return;
    }
    case DSHAPE_DIAMOND: {
        if (p <= 0) return;                    /* 实心块不生长，到点即现 */
        lv_area_t bb; shape_bbox(sh, &bb);
        if (!on_clip(&bb, clip)) return;
        int32_t hh = sh->h ? sh->h : sh->w;
        lv_draw_triangle_dsc_t td;
        lv_draw_triangle_dsc_init(&td);
        td.color = col;  td.opa = opa;
        td.p[0].x = sh->x;         td.p[0].y = sh->y - hh;
        td.p[1].x = sh->x - sh->w; td.p[1].y = sh->y;
        td.p[2].x = sh->x + sh->w; td.p[2].y = sh->y;
        lv_draw_triangle(layer, &td);
        td.p[0].y = sh->y + hh;
        lv_draw_triangle(layer, &td);
        return;
    }
    default: break;
    }

    /* RECT */
    int32_t x = sh->x, y = sh->y, w = sh->w, h = sh->h;
    if (grow) {
        bool horiz = (w >= h);
        int32_t len = horiz ? w : h;
        int32_t g   = len * p / 1000;
        if (g <= 0) return;
        if (horiz) { if (sh->anchor == DECO_REV) x += len - g; w = g; }
        else       { if (sh->anchor == DECO_REV) y += len - g; h = g; }
    }
    lv_area_t a = { x, y, x + w - 1, y + h - 1 };
    if (!on_clip(&a, clip)) return;
    rdsc->bg_opa = opa;
    lv_draw_rect(layer, rdsc, &a);
}

/* GAUGE 第 k 格的基准 opa。三档静态——"当前格最亮"本身就够读作运转中，
 * 而这块屏上每一次 per-frame 动画都要从渲染预算里出。 */
static int32_t gauge_opa(int k, int cur)
{
    if (cur < 0)  return OPA_SEG_FUT;
    if (k <  cur) return OPA_SEG_PAST;
    if (k == cur) return OPA_SEG_CUR;
    return OPA_SEG_FUT;
}

/* METER 与 GAUGE 的区别是【语义】不是形态：GAUGE 回答"现在在第几格"
 * （所以只有一格最亮），METER 回答"到了什么水平"（所以是一段连续的
 * 液面，液面那根最亮）。两者共用矩形表，靠原型分流。 */
static int32_t meter_opa(int k, int lvl)
{
    if (lvl < 0)      return OPA_SEG_FUT;
    if (k <  lvl - 1) return OPA_SEG_PAST;
    if (k == lvl - 1) return OPA_SEG_CUR;   /* 液面 */
    return OPA_SEG_FUT;
}

/* ── 停留期效果：全部是 200 ms 一拍的离散事件 ─────────────────────── */

/* 相位推进。返回本元素的新 ph；引擎据其变化决定要不要失效。 */
static int16_t live_phase(const struct ui_deco *d, const deco_elem_t *el,
                          uint32_t tick, int ei)
{
    switch (el->arch) {
    case DECO_BRACKET:
        /* 每 30 拍（6 s）一次"重锁定"：四角同时闪 2 拍。像自动对焦确认。
         * 四个角标是四个元素，靠同一个 tick 天然同步。
         * pulse_left 让【真事件】也能触发同一下闪——定时闪是底噪，事件
         * 闪才让这层从"按表演出"变成"对世界有反应"。 */
        return (int16_t)((tick % PACE_BRK[d->pace < 3 ? d->pace : 1] < 2)
                         || d->pulse_left > 0);
    case DECO_LINE: {
        /* 一道亮段沿线掠过，走完停一段再来。步长以【拍】计，所以它是
         * 跳着走的——机械语法要的就是阶跃，读作数据包在传输。
         * 暂停期把相位【钳住】而不是继续递增：ph 不变 = 签名不变 =
         * flush_dirty 不失效。否则这条线会在什么都没变的 14 拍里每拍
         * 失效一次 656 px。 */
        int32_t ph = (int32_t)(tick % (uint32_t)(16 + PACE_PAUSE[d->pace < 3 ? d->pace : 1]));
        return (int16_t)(ph < 16 ? ph : 16);
    }
    case DECO_BLOCKS: {
        /* 偶发单块闪变：约 1/6 的拍里挑一块压暗 1 拍。ph 编码 "哪一块+1"，
         * 0 = 本拍无闪变。 */
        uint32_t hv = hash32(tick * 2654435761U + (uint32_t)ei);
        if ((hv & 7) >= 2) return 0;
        return (int16_t)(1 + (hv >> 8) % (el->rn ? el->rn : 1));
    }
    case DECO_TICKS: {
        /* 一道扫描沿刻度组推进，走完停 12 拍（暂停期同样钳住相位）。 */
        int32_t ph = (int32_t)(tick % (uint32_t)(el->rn + PACE_PAUSE[d->pace < 3 ? d->pace : 1]));
        return (int16_t)(ph < el->rn ? ph : el->rn);
    }
    case DECO_GAUGE:
    case DECO_METER:
        /* 当前格 / 液面脉动：两档，每 3 拍（600 ms）切换。 */
        return (int16_t)((tick / 3) % 2);
    default:
        return 0;
    }
}

/* ── 绘制 ────────────────────────────────────────────────────────── */
static void draw_cb(lv_event_t *e)
{
    if (!s_on || s_gain == 0) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) return;
    lv_obj_t *self = lv_event_get_target(e);

    struct ui_deco *d = NULL;
    for (int i = 0; i < s_inst_n; ++i)
        if (s_inst[i].obj == self) { d = &s_inst[i]; break; }
    if (!d || d->phase == PH_HIDDEN) return;

    const lv_area_t *clip = &layer->_clip_area;
    const theme_palette_t *pal = theme_current();

    lv_draw_rect_dsc_t dsc;
    lv_color_t col = lv_color_hex(pal ? pal->text : 0xF3EEE2);
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color   = col;
    dsc.border_opa = LV_OPA_TRANSP;
    dsc.radius     = 0;

    const ui_deco_spec_t *sp = d->spec;
    for (int ei = 0; ei < sp->elem_n; ++ei) {
        const deco_elem_t *el = &sp->elems[ei];
        const deco_rt_t   *rt = &d->e[ei];
        if (rt->p <= 0) continue;
        /* 姿态偏移（负空间适配）与几何调制（动态几何）都在这里折进来：
         * 前者平移整组，后者按数据比例缩短进度。 */
        int32_t dy    = elem_dy(d, el);
        int32_t eff_p = elem_span_p(d, el, rt->p);
        if (eff_p <= 0) continue;

        for (int k = 0; k < el->rn; ++k) {
            const deco_shape_t *r = &sp->shapes[el->ri + k];
            int32_t opa = r->opa;

            if (!arch_is_grow(el->arch)) {
                /* 点亮类：第 idx 格在 p 越过其阈值的那一帧【整块】出现。
                 * p 退回时高 idx 先灭，出场的后进先出因此不用另写。 */
                int idx = el->rev ? (el->rn - 1 - k) : k;
                if (rt->p <= (int32_t)idx * 1000 / el->rn) continue;
                /* 接了状态槽的元素在这里把真值折进来。BLOCKS 的解释是
                 * "只显示前 N 块"——块【数量】本身成为读数（dashboard
                 * 用它显示 fleet 行数），这比再加一个计数器更机能。 */
                int sv = slot_of(d, el);
                if (el->arch == DECO_GAUGE)      opa = gauge_opa(k, sv);
                else if (el->arch == DECO_METER) opa = meter_opa(k, sv);
                else if (el->slot && sv >= 0 && k >= sv) continue;
            }

            /* 停留期效果只在完全就位后生效——入场途中再叠一层闪变，读
             * 起来就是两套动画在打架。 */
            if (d->phase == PH_LIVE) {
                int16_t ph = rt->ph & 0xFF;
                if (rt->ph & ALERT_BIT) opa = opa * 2;   /* 锁定确认扫过 */
                switch (el->arch) {
                case DECO_BRACKET:
                    if (ph) opa = opa * 9 / 5;
                    break;
                case DECO_BLOCKS:
                    if (ph == k + 1) opa /= 3;
                    break;
                case DECO_TICKS:
                    if (ph == k) opa = opa * 5 / 2;
                    break;
                case DECO_GAUGE:
                    /* 当前格【向上】脉动。压暗是错的：190*3/5=114 已经贴到
                     * "已过"档的 100，半个周期里读者分不清哪一格是当前。
                     * 往上顶到 253 才让"现在"始终是全层最亮的一点。 */
                    if (ph && k == slot_of(d, el)) opa = opa * 4 / 3;
                    break;
                case DECO_METER:
                    if (ph && k == slot_of(d, el) - 1) opa = opa * 4 / 3;
                    break;
                default: break;
                }
            }

            lv_opa_t o = gained(d, opa);
            if (o == 0) continue;
            draw_shape(layer, clip, &dsc, col, r,
                       arch_is_grow(el->arch), eff_p, o, dy);
        }

        /* LINE 的掠过亮段：在基准线上【叠画】一小段，alpha 累积即变亮。
         * 单独处理是因为它不是表里的矩形——它是同一个矩形的一个子区间。 */
        if (el->arch == DECO_LINE && d->phase == PH_LIVE &&
            (rt->ph & 0xFF) < 16 && sp->shapes[el->ri].kind == DSHAPE_RECT) {
            const deco_shape_t *r = &sp->shapes[el->ri];
            bool horiz = (r->w >= r->h);
            int32_t len = horiz ? r->w : r->h;
            int32_t seg = len / 8;
            int32_t at  = len * (rt->ph & 0xFF) / 16;
            if (seg < 8) seg = 8;
            if (at + seg > len) seg = len - at;
            if (seg > 0) {
                int32_t ly = r->y + elem_dy(d, el);
                lv_area_t a = horiz
                    ? (lv_area_t){ r->x + at, ly, r->x + at + seg - 1, ly + r->h - 1 }
                    : (lv_area_t){ r->x, ly + at, r->x + r->w - 1, ly + at + seg - 1 };
                if (!(a.x1 > clip->x2 || a.x2 < clip->x1 ||
                      a.y1 > clip->y2 || a.y2 < clip->y1)) {
                    dsc.bg_opa = gained(d, (int32_t)r->opa * 5 / 2);
                    if (dsc.bg_opa) lv_draw_rect(layer, &dsc, &a);
                }
            }
        }
    }
}

/* ── 相位推进 ────────────────────────────────────────────────────── */

static void drop_tier(struct ui_deco *d)
{
    if (d->holding) { ui_motion_release(); d->holding = false; }
}

/* 元素当前的【视觉签名】——变了才需要重画。
 * 生长类的 p 是连续的（笔尖真的在移动），签名就是 p 本身；点亮类的视觉
 * 只在【跨过格阈值】那一刻变，中间那些 p 值画出来一模一样。用 p 当签名
 * 会让一个 6 格元素在 270 ms 的入场里失效 8 次而不是 6 次——三张谱都上
 * 齐之后，转场两端各演一套，这些白跑的失效把 weather->clock 顶出了 15%
 * 的回归门。原型分野在这里第二次发红利：它既定义动效语法，也定义各自
 * 该用什么粒度失效。 */
static int16_t visual_sig(const deco_elem_t *el, int16_t p)
{
    if (arch_is_grow(el->arch)) return p;
    return (int16_t)((int32_t)p * el->rn / 1000);
}

/* 失效一个元素：bbox 是基准位置，按当前姿态的偏移平移后再交给 LVGL。 */
static void elem_inval(struct ui_deco *d, int i)
{
    lv_area_t a = d->e[i].bbox;
    int32_t dy = elem_dy(d, &d->spec->elems[i]);
    if (dy) { a.y1 += dy; a.y2 += dy; }
    lv_obj_invalidate_area(d->obj, &a);
}

/* 只失效【签名变了】的元素。停留期通常只有 1-2 个元素在动，于是整层的
 * 持续动效成本约等于几百个像素。 */
static void flush_dirty(struct ui_deco *d)
{
    for (int i = 0; i < d->spec->elem_n; ++i) {
        deco_rt_t *rt = &d->e[i];
        int16_t sig = visual_sig(&d->spec->elems[i], rt->p);
        if (sig == rt->p_seen && rt->ph == rt->ph_seen) continue;
        rt->p_seen = sig; rt->ph_seen = rt->ph;
        elem_inval(d, i);
    }
}

static void step_inst(struct ui_deco *d)
{
    const ui_deco_spec_t *sp = d->spec;

    if (d->phase == PH_IN || d->phase == PH_OUT) {
        uint32_t t;
        if (s_freeze >= 0 && d->phase == PH_IN) {
            t = (uint32_t)s_freeze;
        } else {
            uint32_t el = lv_tick_elaps(d->t0);
            /* 入场先让路（lead）：演员落座之前一个矩形都不画。 */
            t = (d->phase == PH_IN && el < d->lead) ? 0 : el - (d->phase == PH_IN ? d->lead : 0);
        }
        bool done = (t >= d->span);
        if (done) t = d->span;

        for (int i = 0; i < sp->elem_n; ++i) {
            deco_rt_t *rt = &d->e[i];
            uint32_t at   = (d->phase == PH_IN) ? rt->in_at   : rt->out_at;
            uint32_t span = (d->phase == PH_IN) ? rt->in_span : rt->out_span;
            int32_t  v;
            if (span == 0)      v = (t >= at) ? 1000 : 0;
            else if (t <= at)   v = 0;
            else if (t >= at + span) v = 1000;
            else                v = (int32_t)(t - at) * 1000 / (int32_t)span;
            /* 出场是同一条曲线【倒着放】——p 退回 0 时生长类自末端收回、
             * 点亮类自最后一格熄灭，后进先出自动成立。 */
            int32_t np = (d->phase == PH_IN) ? v : 1000 - v;
            if (!elem_on(d, &sp->elems[i])) np = 0;   /* 不属于当前姿态 */
            if (d->phase == PH_IN && elem_deferred(&sp->elems[i])) np = 0;
            rt->p = (int16_t)np;
            rt->ph = 0;
        }
        flush_dirty(d);

        if (done && s_freeze < 0) {
            if (d->phase == PH_IN) {
                d->phase = PH_LIVE;
                d->tick  = 0;
                /* 不在这里切回慢拍：DECO_DEFER 的元素正等着补场，
                 * timer_cb 会依 morphing 决定拍频。 */
            } else {
                d->phase = PH_HIDDEN;
            }
            drop_tier(d);
        }
        return;
    }

    if (d->phase != PH_LIVE) return;
    d->tick++;
    if (d->pulse_left) d->pulse_left--;

    /* 姿态切换的进/退场也在这里推进——【复用转场那套 p 语法】，所以
     * 换姿态时元素是长出来/收回去的，不是硬切。 */
    bool moving = false;
    for (int i = 0; i < sp->elem_n; ++i) {
        const deco_elem_t *el = &sp->elems[i];
        deco_rt_t *rt = &d->e[i];
        int16_t want = elem_on(d, el) ? 1000 : 0;
        if (rt->p != want) {
            uint16_t span = want ? rt->in_span : rt->out_span;
            int32_t step = 1000 * (int32_t)TICK_ANIM_MS / (span ? span : 1);
            if (step < 1) step = 1;
            if (rt->p < want) { rt->p = (int16_t)(rt->p + step);
                                if (rt->p > want) rt->p = want; }
            else              { rt->p = (int16_t)(rt->p - step);
                                if (rt->p < want) rt->p = want; }
            moving = true;
        }
        rt->ph = live_phase(d, el, d->tick, i);
        if (d->alert_ms) {
            int32_t pos = (int32_t)(ALERT_MS - d->alert_ms)
                        * (d->max_delay + ALERT_W) / ALERT_MS;
            if (el->delay <= pos && el->delay > pos - ALERT_W)
                rt->ph |= ALERT_BIT;
        }
    }
    if (d->alert_ms)
        d->alert_ms = (d->alert_ms > TICK_ANIM_MS)
                    ? (uint16_t)(d->alert_ms - TICK_ANIM_MS) : 0;
    d->morphing = moving;
    flush_dirty(d);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    bool any_anim = false;
    for (int i = 0; i < s_inst_n; ++i) {
        step_inst(&s_inst[i]);
        if (s_inst[i].phase == PH_IN || s_inst[i].phase == PH_OUT) any_anim = true;
    }
    /* 没有实例在演出时回落到慢拍；全部隐藏时干脆停表。姿态切换途中
     * （morphing）同样要快拍，否则元素会以 200 ms 一步的粗颗粒跳出来。 */
    for (int i = 0; i < s_inst_n && !any_anim; ++i)
        if (s_inst[i].morphing || s_inst[i].alert_ms) any_anim = true;
    if (!any_anim) {
        bool any_live = false;
        for (int i = 0; i < s_inst_n; ++i)
            if (s_inst[i].phase == PH_LIVE) { any_live = true; break; }
        if (any_live) lv_timer_set_period(s_timer, TICK_LIVE_MS);
        else          lv_timer_pause(s_timer);
    }
}

/* ── 编排表的预解算 ──────────────────────────────────────────────── */
static void build_rt(struct ui_deco *d)
{
    const ui_deco_spec_t *sp = d->spec;
    uint32_t in_total = 0;

    for (int i = 0; i < sp->elem_n; ++i) {
        const deco_elem_t *el = &sp->elems[i];
        deco_rt_t *rt = &d->e[i];
        /* 生长类 dur 是整体时长；点亮类 dur 是每格步进。 */
        rt->in_at   = el->delay;
        rt->in_span = arch_is_grow(el->arch) ? el->dur
                                             : (uint16_t)(el->dur * el->rn);
        if ((uint32_t)rt->in_at + rt->in_span > in_total)
            in_total = (uint32_t)rt->in_at + rt->in_span;

        /* bbox = 本元素所有矩形的并集（用【满尺寸】算，生长途中只会更小）。 */
        lv_area_t b = { INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN };
        for (int k = 0; k < el->rn; ++k) {
            lv_area_t sb;
            shape_bbox(&sp->shapes[el->ri + k], &sb);
            if (sb.x1 < b.x1) b.x1 = sb.x1;
            if (sb.y1 < b.y1) b.y1 = sb.y1;
            if (sb.x2 > b.x2) b.x2 = sb.x2;
            if (sb.y2 > b.y2) b.y2 = sb.y2;
        }
        /* bbox 存【基准】位置，不含姿态偏移——失效时按当前姿态平移。
         * 早先按两姿态并集存，上下带的盒子因此凭空高出 18-26 px，常态
         * 每次失效都在为一个用不到的位置买单（实测 dashboard->clock
         * render +30%）。姿态切换时改为失效新旧两处，那是稀疏事件。 */
        rt->bbox = b;
    }

    /* 出场 = 入场的镜像（后进先出）并整体压到 OUT_SCALE%：机械层断电
     * 该干脆，而且必须在 OUT_MS(240) 的黑幕之前演完，否则后半截会被
     * 场景容器一起隐藏掉，等于没演。 */
    for (int i = 0; i < sp->elem_n; ++i) {
        deco_rt_t *rt = &d->e[i];
        uint32_t end = (uint32_t)rt->in_at + rt->in_span;
        rt->out_at   = (uint16_t)((in_total - end) * OUT_SCALE / 100);
        rt->out_span = (uint16_t)(rt->in_span * OUT_SCALE / 100);
    }
    d->span = in_total;
    d->max_delay = 0;
    for (int i = 0; i < sp->elem_n; ++i)
        if (sp->elems[i].delay > d->max_delay) d->max_delay = sp->elems[i].delay;
}

/* ── 公开 API ────────────────────────────────────────────────────── */

ui_deco_t *ui_deco_attach(lv_obj_t *parent, const ui_deco_spec_t *spec)
{
    if (!parent || !spec || spec->elem_n > DECO_ELEM_MAX) return NULL;
    for (int i = 0; i < s_inst_n; ++i)
        if (lv_obj_get_parent(s_inst[i].obj) == parent) return &s_inst[i];
    if (s_inst_n >= DECO_INST_MAX) return NULL;

    struct ui_deco *d = &s_inst[s_inst_n];
    memset(d, 0, sizeof(*d));
    d->spec  = spec;
    d->phase = PH_HIDDEN;
    d->pace  = AGENT_ATTN_BUSY;          /* 场景 tick 第一拍就会校正 */
    for (int i = 0; i < UI_DECO_SLOT_N; ++i) d->slot[i] = -1;

    /* 置底。注意不能挂 lv_layer_bottom()：scene_framework 给每个场景根画
     * 了【不透明黑】背景（黑帧瞬切用），会把底层完全盖住。
     * 也【不】注册成 trans_actor：装饰是"面板的一部分"，一个对象里装着
     * 几十个形状，位移它等于位移全屏——它靠 on_intro/on_outro 钩子自己
     * 演（见 scene_trans.h 的 v7.6 说明）。 */
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, UI_LV_W, UI_LV_W);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_move_background(o);
    d->obj = o;

    build_rt(d);
    s_inst_n++;

    if (!s_timer) {
        s_timer = lv_timer_create(timer_cb, TICK_LIVE_MS, NULL);
        lv_timer_pause(s_timer);
    }
    return d;
}

void ui_deco_intro(ui_deco_t *d, uint32_t ms)
{
    if (!d || !s_timer) return;
    if (ms == 0) {                       /* motion_reduced：直接到位 */
        for (int i = 0; i < d->spec->elem_n; ++i) {
            d->e[i].p = elem_on(d, &d->spec->elems[i]) ? 1000 : 0;
            d->e[i].ph = 0;
        }
        d->phase = PH_LIVE; d->tick = 0;
        drop_tier(d);
        lv_obj_invalidate(d->obj);
        lv_timer_set_period(s_timer, TICK_LIVE_MS);
        lv_timer_resume(s_timer);
        return;
    }
    /* ── 让路（v7.6.1，实测驱动的改版） ─────────────────────────────
     * 装饰的入场原本与演员 intro 并行（"机械层先就位、内容后落座"）。
     * 三张谱上齐后一次【同固件背靠背 A/B】把这个设计判了死刑：
     * render +10%~+58%，四行破 15% 回归门，且 drawn 反而下降——每帧变贵
     * 而不是帧数变多。
     * 根因不在装饰自己那 3.7k px 墨迹，而在【它被演员的大失效区连累】：
     * 演员从屏外飞入时失效区可达 400x200，装饰在最底层，落在里面的十几
     * 个矩形每帧都得重画，每个都是一次 draw-task 创建与调度。静置时
     * AABB 预判能挡掉绝大多数，转场时挡不住。
     * （顺带推翻了本文件头原先"稀疏几何 fill 一定比 blit 便宜"的论断
     * ——那只在失效区小的时候成立。）
     * 修法是把入场整体推后到演员落座之后，两个昂贵窗口不再重叠。叙事
     * 也依然成立，只是反了过来：不是仪表框先亮等内容，而是内容落定后
     * 仪表框再扫描确认——反而更像真机器的上电自检。 */
    d->phase = PH_IN;
    d->t0    = lv_tick_get();
    d->lead  = ms;
    /* 不再 take_tier：拍频已经等于静止档周期（见 TICK_ANIM_MS）。
     * drop_tier 仍保留在 live(false)/settle 路径上作为防御——holding
     * 恒为 false 时它是 no-op。 */
    lv_timer_set_period(s_timer, TICK_ANIM_MS);
    lv_timer_resume(s_timer);
    step_inst(d);
}

void ui_deco_outro(ui_deco_t *d, uint32_t ms)
{
    if (!d || !s_timer) return;
    if (d->phase == PH_HIDDEN) return;
    if (ms == 0) {
        for (int i = 0; i < d->spec->elem_n; ++i) d->e[i].p = 0;
        d->phase = PH_HIDDEN;
        drop_tier(d);
        lv_obj_invalidate(d->obj);
        return;
    }
    /* 出场【不】让路：装饰要先撤，才轮到演员飞出——顺序与入场镜像。
     * 出场只有入场的 33%（~220 ms），与演员出场窗口的重叠远小于入场。 */
    d->phase = PH_OUT;
    d->t0    = lv_tick_get();
    d->lead  = 0;
    /* 不再 take_tier：拍频已经等于静止档周期（见 TICK_ANIM_MS）。
     * drop_tier 仍保留在 live(false)/settle 路径上作为防御——holding
     * 恒为 false 时它是 no-op。 */
    lv_timer_set_period(s_timer, TICK_ANIM_MS);
    lv_timer_resume(s_timer);
    step_inst(d);
}

void ui_deco_live(ui_deco_t *d, bool on)
{
    if (!d || !s_timer) return;
    if (on) {
        /* 非转场路径（首次 show、push 卡收回等）也要有完整画面。 */
        if (d->phase == PH_HIDDEN) {
            for (int i = 0; i < d->spec->elem_n; ++i)
                d->e[i].p = elem_on(d, &d->spec->elems[i]) ? 1000 : 0;
            d->phase = PH_LIVE;
            d->tick  = 0;
            lv_obj_invalidate(d->obj);
        }
        lv_timer_resume(s_timer);
    } else {
        drop_tier(d);
        if (d->phase == PH_LIVE) d->phase = PH_HIDDEN;
        for (int i = 0; i < d->spec->elem_n; ++i) { d->e[i].p = 0; d->e[i].ph = 0; }
    }
}

void ui_deco_set_slot(ui_deco_t *d, int slot, int v)
{
    if (!d || slot < 0 || slot >= UI_DECO_SLOT_N) return;
    if (d->slot[slot] == v) return;          /* 去重：值不变不失效 */
    d->slot[slot] = v;
    /* 只失效读这个槽的元素。 */
    uint8_t tag = UI_DECO_SLOT(slot);
    for (int i = 0; i < d->spec->elem_n; ++i)
        if (d->spec->elems[i].slot == tag || d->spec->elems[i].span_slot == tag)
            elem_inval(d, i);
}

void ui_deco_set_state(ui_deco_t *d, uint8_t state)
{
    if (!d || d->state == state) return;
    /* 带姿态偏移的元素要把【旧位置】也擦掉，否则平移后留残影。 */
    if (d->phase != PH_HIDDEN) {
        for (int i = 0; i < d->spec->elem_n; ++i)
            if (d->spec->elems[i].dy_a || d->spec->elems[i].dy_b)
                elem_inval(d, i);
    }
    d->state = state;
    if (d->phase != PH_HIDDEN) {
        for (int i = 0; i < d->spec->elem_n; ++i)
            if (d->spec->elems[i].dy_a || d->spec->elems[i].dy_b)
                elem_inval(d, i);
    }
    if (d->phase == PH_LIVE && s_timer) {
        d->morphing = true;                       /* 下一拍开始长/收 */
        lv_timer_set_period(s_timer, TICK_ANIM_MS);
        lv_timer_resume(s_timer);
    }
}

void ui_deco_set_pace(ui_deco_t *d, uint8_t attn)
{
    if (!d || attn > 2 || d->pace == attn) return;
    d->pace = attn;
    /* 亮度乘数变了 = 整层视觉都变了，只能整块失效一次。节奏切换很稀疏
     * （空闲↔思考↔该你了），不在热路径上。 */
    if (d->phase != PH_HIDDEN) lv_obj_invalidate(d->obj);
}

void ui_deco_alert(ui_deco_t *d)
{
    if (!d || !s_timer || d->phase != PH_LIVE) return;
    d->alert_ms = ALERT_MS;
    lv_timer_set_period(s_timer, TICK_ANIM_MS);
    lv_timer_resume(s_timer);
}

void ui_deco_pulse(ui_deco_t *d)
{
    if (!d || d->phase != PH_LIVE) return;
    d->pulse_left = 2;
    for (int i = 0; i < d->spec->elem_n; ++i) {
        if (d->spec->elems[i].arch != DECO_BRACKET) continue;
        d->e[i].ph = 1;
        d->e[i].ph_seen = 1;
        elem_inval(d, i);
    }
}

/* ── ?deco ───────────────────────────────────────────────────────────
 *   ?deco            报告状态
 *   ?deco 0 | 1      整层开关（A/B，与 ?bake / ?wxcomp 同惯例）
 *   ?deco g <0-255>  全局 opa 增益 —— 实机扫"存在感"
 *   ?deco f <ms>     入场包络定格在第 ms 毫秒；`?deco f off` 解除
 *   ?deco in | out   手动播一次入场/出场
 * 定格的理由同 ?glow：入场只有 0.7 s，而一张 480 截图要 5 s，动态过程
 * 根本抓不住，也就没法验证"某一相位到底长什么样"。 */
static void invalidate_all(void)
{
    for (int i = 0; i < s_inst_n; ++i) lv_obj_invalidate(s_inst[i].obj);
}

static int cmd_deco(const console_args_t *args)
{
    const char *a = (args->argc >= 2) ? args->argv[1] : NULL;
    const char *b = (args->argc >= 3) ? args->argv[2] : NULL;

    bsp_display_lock(-1);
    if (a && a[0] == 'g') {
        int v = b ? atoi(b) : 255;
        s_gain = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        invalidate_all();
    } else if (a && a[0] == 'f') {
        if (b && b[0] == 'o') {                 /* off：解除定格 */
            s_freeze = -1;
            /* 立刻推一拍，好让查询到的 phase 就是解除后的真相（否则要等
             * 下一个 timer tick 才从 IN 自愈到 LIVE，读起来像卡住了）。 */
            for (int i = 0; i < s_inst_n; ++i) step_inst(&s_inst[i]);
        } else {
            s_freeze = b ? atoi(b) : 0;
            for (int i = 0; i < s_inst_n; ++i) {
                s_inst[i].phase = PH_IN;
                s_inst[i].t0    = lv_tick_get();
                step_inst(&s_inst[i]);
            }
            invalidate_all();
        }
    } else if (a && strcmp(a, "in") == 0) {
        s_freeze = -1;
        for (int i = 0; i < s_inst_n; ++i) ui_deco_intro(&s_inst[i], 1);
    } else if (a && strcmp(a, "out") == 0) {
        s_freeze = -1;
        for (int i = 0; i < s_inst_n; ++i) ui_deco_outro(&s_inst[i], 1);
    } else if (a) {
        bool want = (atoi(a) != 0);
        if (want != s_on) { s_on = want; invalidate_all(); }
    }
    int ph = s_inst_n ? (int)s_inst[0].phase : -1;
    int sp = s_inst_n ? (int)s_inst[0].span  : 0;
    bsp_display_unlock();

    console_reply_ok("{\"deco\":%d,\"gain\":%d,\"phase\":%d,\"in_ms\":%d,"
                     "\"freeze\":%d,\"elems\":%d,\"insts\":%d}",
                     s_on ? 1 : 0, s_gain, ph, sp, (int)s_freeze,
                     s_inst_n ? s_inst[0].spec->elem_n : 0, s_inst_n);
    return 0;
}

static const console_cmd_t s_cmd_deco = { "?deco", cmd_deco,
    "deco layer: ?deco | 0|1 | g <0-255> | f <ms>|off | in | out" };

void ui_deco_register_cmds(void)
{
    console_protocol_register(&s_cmd_deco);
}
