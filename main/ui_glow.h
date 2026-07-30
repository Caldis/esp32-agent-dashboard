#pragma once
#include <stdint.h>

/*
 * ui_glow — 屏幕边缘内发光，公共组件 (v6.9 → v7.3 色散重制)。
 *
 * LVGL 9 没有 inset shadow，所以用【同心圆角描边叠加】近似：N 层由可见
 * 边缘向内层层内缩，亮度逐层递减，合起来读作一圈由边缘向内渗的辉光。
 * 层窄而少，靠亮度梯度而不是模糊来造柔和感。
 *
 * v7.3：按键回应升级为【色散】——四层各占一个色相（品红/琥珀/teal/紫），
 * 叠加位移、波动、依次失焦、色序翻转、收束绽散，读作棱镜把一道白光拆开。
 * 状态提示（waiting/lost）不进谱：颜色在这台设备上是状态契约，只做同色
 * 相的轻微分光。实现从"N 个 lv_obj"改成"一个自绘对象"，因为几何动画
 * 走不了样式批量写入那条路（细节见 ui_glow.c 顶部）。
 *
 * 两种用法，可以共存：
 *
 *   ping     一次性。淡入 -> 保持 -> 扩散范围收缩的同时淡出。
 *            用于"按键收到了"这类瞬时回应。
 *   sustain  常驻。淡入后静止保持，直到 clear。
 *            用于"设备处于某个状态"这类持续提示（如掉线）。
 *
 * 二者叠加时 ping 临时接管，结束后自动回落到 sustain 的姿态——按键回应
 * 不该把状态提示抹掉。
 *
 * 功耗：sustain 淡入结束后画面是静止的，所以它【不持有高刷档】。只有
 * 淡入淡出的那几百毫秒才需要（见 ui_motion 的引用计数）。持续持有高刷
 * 去显示一个不动的边框，是白烧电。
 *
 * 渲染成本：环的包围盒是整块屏，失效范围必须自己控制在边缘四条窄带上
 * （v6.6 实测：整屏级重绘 53.4ms/帧 -> 窄带 18.5ms/帧）。v7.3 的色散
 * 因为环更宽、有位移、带更深，实测 26.6ms/帧（≈37fps，950ms 一次性）。
 * 定档过程与三个已排除的成本项见 ui_glow.c 的成本旋钮注释。
 */

typedef struct {
    uint32_t color;    /* RGB888 */
    uint8_t  peak;     /* 峰值亮度 0..255 */
    uint8_t  spread;   /* 扩散范围 0..255：决定向内亮几层 */
} ui_glow_style_t;

/* 按键回应：主题强调色，满扩散。 */
extern const ui_glow_style_t UI_GLOW_KEY;
/* 掉线提示：琥珀 / 红，窄扩散——它是常驻的，铺开会太吵。 */
extern const ui_glow_style_t UI_GLOW_WAITING;
extern const ui_glow_style_t UI_GLOW_LOST;

void ui_glow_init(void);

/* 注册 `?glow`。必须在 console_protocol_init() 之前调用（register-then-
 * listen），而 ui_glow_init 要持显示锁、在那之后——所以分成两个入口。 */
void ui_glow_register_cmds(void);

void ui_glow_ping(const ui_glow_style_t *s);
void ui_glow_sustain(const ui_glow_style_t *s);
void ui_glow_sustain_clear(void);
