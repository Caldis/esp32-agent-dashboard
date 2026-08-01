#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * ui_deco — 机能装饰层（v7.6：编排引擎）。
 *
 * v7.5 是一张静态矩形表。v7.6 把它升级成【原型 + 编排表】：每个元素属于
 * 一种原型（archetype），原型定义它怎么入场、怎么出场、停留时怎么活着；
 * 每个场景提供一张自己的谱（ui_deco_spec_t）。加一个场景 = 加一张谱，
 * 不动引擎。
 *
 * 设计推导（视距比例、密度上限、机械 vs 有机的运动语法、为什么持续动效
 * 必须跑在 idle 刷新档上）见 ui_deco.c 的文件头。
 */

/* ── 元素原型 ────────────────────────────────────────────────────────
 * 分野只有一条，但它是整套运动语法的根：
 *   【结构元素】连续【生长】——像绘图仪落笔画线，线性匀速，不回弹；
 *   【数据元素】离散【点亮】——像指示灯逐个通电，瞬时，有节拍。
 * 内容层用弹簧（有机），装饰层全程机械，两者构成复调。 */
typedef enum {
    DECO_BRACKET = 0,  /* 取景角标：两臂自拐点生长；停留时周期性"重锁定" */
    DECO_LINE,         /* 结构线：单向擦出；停留时一道亮段沿线掠过 */
    DECO_BLOCKS,       /* 分段块组：逐块瞬亮；停留时偶发单块闪变 */
    DECO_TICKS,        /* 刻度组：逐根瞬亮；停留时一道扫描沿组推进 */
    DECO_GAUGE,        /* 分段条：逐格填充；停留时【当前格】脉动 */
    DECO_METER,        /* 水平计：填充到某个【水平】；停留时液面那根抖动 */
    DECO_ARCH_COUNT
} deco_arch_t;

/* ── 状态槽 ──────────────────────────────────────────────────────────
 * 装饰要"鲜活"就得接真数据，但引擎不该认识任何业务概念。折中是一组无
 * 名整数槽：场景往里写值，元素声明自己读第几个槽，每种原型自行解释。
 *   GAUGE   槽值 = 当前格索引        （-1 全暗）
 *   METER   槽值 = 填充到第几根       （-1 全暗，>=rn 全满）
 *   BLOCKS  槽值 = 只显示前 N 块      （-1 = 不受控，全显示）
 *   其余原型忽略
 * 元素的 slot 字段 0 表示【不接状态】，接槽的元素写 1+槽号。 */
#define UI_DECO_SLOT_N   4
#define UI_DECO_SLOT(i)  ((uint8_t)((i) + 1))

/* 生长锚点。矩形沿它的长轴生长（w>=h 沿 x，否则沿 y）：
 * FWD = 从 x1/y1 端长出，REV = 从 x2/y2 端长出。只对生长类原型有意义。 */
#define DECO_FWD   0
#define DECO_REV   1

/* ── 形状 ────────────────────────────────────────────────────────────
 * v7.7 之前整层只有轴对齐矩形，于是画面里只有横线、竖线和方块——没有
 * 角度，也没有曲线。这几种补上斜边、弧和多边形。
 *
 * 参数按 kind 复用同一组字段（省 flash，也省得为每种形状开一张表）：
 *   RECT  (x,y,w,h) = 方框；anchor 决定沿长轴从哪端生长
 *   LINE  (x,y) = 起点，(w,h) = 终点【相对位移】（可负），a = 线宽
 *   ARC   (x,y) = 圆心，w = 半径，h = 线宽，a/b = 起止角
 *         （度，0° 在 3 点钟方向，顺时针）
 *   TRI   (x,y) = 顶点1，(w,h) = 顶点2 相对，(a,b) = 顶点3 相对；实心
 *   DOT   (x,y) = 圆心，w = 半径，h = 线宽（0 = 实心圆）
 *
 * **弧的生长扫的是角度，不是半径**——这既是视觉需要（像雷达扫出来），
 * 也是性能需要：LVGL 的圆角覆盖图缓存只按半径做键（见 ui_glow.c 与
 * sdkconfig.defaults 的台账），半径每帧变就是每帧重算 mask。 */
typedef enum {
    DSHAPE_RECT = 0,
    DSHAPE_LINE,
    DSHAPE_ARC,
    DSHAPE_TRI,
    DSHAPE_DOT,
} deco_kind_t;

typedef struct {
    int16_t x, y, w, h;
    uint8_t opa;        /* 基准 opa（GAUGE/METER 忽略此值，按真值现算） */
    uint8_t anchor;     /* DECO_FWD / DECO_REV */
    uint8_t kind;       /* deco_kind_t；0 = RECT，所以旧条目不必改 */
    int16_t a, b;       /* kind 专属参数，见上表 */
} deco_shape_t;

typedef struct {
    uint8_t  arch;      /* deco_arch_t */
    uint8_t  ri, rn;    /* 在谱的矩形表里的切片 [ri, ri+rn) */
    uint16_t delay;     /* 入场延迟 ms —— 编排的全部内容 */
    uint16_t dur;       /* 生长时长 ms（生长类）/ 每格步进 ms（点亮类） */
    uint8_t  rev;       /* 1 = 点亮顺序反向（从切片末端往前） */
    uint8_t  slot;      /* 0 = 静态；否则 UI_DECO_SLOT(i) 读第 i 个状态槽 */
    /* 姿态掩码：本元素只在 (mask & 当前姿态) 非零时在场；0 = 常驻。
     * 场景的不同姿态（dashboard 的 ambient/fleet、clock 的大钟/推送卡）
     * 布局差别很大，装饰必须跟着让位——内容密的姿态就该少装饰，那是
     * 负空间的问题，不是省性能的问题。
     * 进出场【复用同一套语法】：目标 p 在 1000/0 之间切换，生长类自末端
     * 收回、点亮类自最后一格熄灭，与转场进出场同一条代码路径。 */
    uint8_t  mask;
} deco_elem_t;

typedef struct {
    const deco_shape_t *shapes;
    uint8_t             shape_n;
    const deco_elem_t  *elems;
    uint8_t             elem_n;
} ui_deco_spec_t;

typedef struct ui_deco ui_deco_t;

/* 挂到场景根下并置底。spec 必须是静态生命周期。 */
ui_deco_t *ui_deco_attach(lv_obj_t *parent, const ui_deco_spec_t *spec);

/* 转场演出。由场景 trans_profile 的 on_intro / on_outro 钩子调用。
 * ms = 转场给的期望时长；0 = 立即到位（motion_reduced）。实际编排时长由
 * 谱里的 delay+dur 决定，ms 只用来判断"要不要演"。 */
void ui_deco_intro(ui_deco_t *d, uint32_t ms);
void ui_deco_outro(ui_deco_t *d, uint32_t ms);

/* 停留期的持续动效开关。场景 on_show / on_hide 调用——不可见的场景不该
 * 烧 tick。 */
void ui_deco_live(ui_deco_t *d, bool on);

/* 写一个状态槽。语义由读它的原型决定（见上）。内部去重：值不变不失效。 */
void ui_deco_set_slot(ui_deco_t *d, int slot, int v);

/* 事件脉冲：让四角取景框立刻"重锁定"一次。停留期本来就有 6 s 一次的
 * 定时闪作为底噪，这个是给【真事件】用的——状态变化时闪一下，装饰就
 * 从"按表演出"变成"对世界有反应"。 */
void ui_deco_pulse(ui_deco_t *d);

/* 切换姿态位（见 deco_elem_t.mask）。不在新姿态里的元素就地退场、新进
 * 场的就地生长，走的是与转场同一套动效。内部去重。 */
void ui_deco_set_state(ui_deco_t *d, uint8_t state);

/* 各场景的姿态位。装饰按【内容密度】反向配置：内容铺满的姿态只留骨架。 */
#define DECO_ST_A        0x01
#define DECO_ST_B        0x02
/* mask 的高位开关：本元素【不参与转场入场】，等画面落定后再补上。
 * 转场演的是这一页的【通用骨架】；姿态专属的补充元素跟着一起涌入，既
 * 把最贵的那个窗口又加重一层，叙事上也含糊。落定后屏幕静止，它们的
 * 失效区只剩自己那一小块，AABB 预判重新生效，成本回到静置量级。 */
#define DECO_DEFER       0x80
#define DECO_ST_MASK     0x7F
/* dashboard：单 agent 的 ambient 簇（中部大片留白） / 2-4 行 fleet 卡片
 * （y134..360 x28..452 全占满，两侧只剩 28 px）。 */
#define DASH_ST_AMBIENT  DECO_ST_A
#define DASH_ST_FLEET    DECO_ST_B
/* clock：大钟居中 / 推送卡弹出（大钟退到顶部槽位，墨迹 61..109）。 */
#define CLK_ST_FACE      DECO_ST_A
#define CLK_ST_PUSH      DECO_ST_B

/* 每场景一张谱。加一个场景 = 加一张谱，不动引擎。
 * 三张谱的密度【与内容密度反向】：clock 最空所以装饰最多，dashboard 中
 * 等，weather 内容最满所以装饰只留边缘骨架。 */
const ui_deco_spec_t *ui_deco_spec_clock(void);
const ui_deco_spec_t *ui_deco_spec_dashboard(void);
const ui_deco_spec_t *ui_deco_spec_weather(void);

#define UI_DECO_CLOCK_SEG_N   6   /* clock 谱里 GAUGE 的格数（10 分钟/格） */
#define UI_DECO_WX_SEG_N      6   /* weather 谱里 GAUGE 的格数（4 小时/格） */

/* ?deco 注册。必须在 console_protocol_init() 之前（register-then-listen）。 */
void ui_deco_register_cmds(void);
