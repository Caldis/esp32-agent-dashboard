# 转场渲染基线 — v7.0（2026-07-29）

场景转场（scene_trans：出场 → 黑幕瞬切 → 入场，约 1.1 s）的逐帧渲染
成本基线。这是继 [PERFORMANCE.md](PERFORMANCE.md)（v0.6 全栈基线，
桥/线缆/堆）之后的第二份可复现基线，聚焦一件事：**转场窗口内每一帧
花了多少毫秒、画了多少帧**。

本文件的每个数字都由 `tools/perf/trans_bench.py` 产出，结果 JSON 提交
在 `tools/perf/results/`。复测与回归判据见 §5。

## 1. 基线数字

提交的基线：`tools/perf/results/trans-2026-07-29-233229-baseline-v7.0.json`
（git b2ea745-dirty = v7.0 优化组落地的工作树，`?bake 0` 默认路径）。

| 转场 | frame_ms 前→后 | render_avg 前→后 | drawn 前→后 |
|---|---:|---:|---:|
| dashboard→clock   | 17.6 → **16.1** | 15.1 → 13.9 | 28 → **36** |
| clock→weather     | 39.3 → **36.0** | 34.0 → 30.9 | 24 → **27** |
| weather→dashboard | 21.0 → **18.2** | 18.6 → 15.9 | 34 → **43** |
| dashboard→weather | 34.2 → **28.9** | 30.2 → 25.2 | 24 → **32** |
| weather→clock     | 31.1 → **32.6** | 27.2 → 29.3 | 26 → **28** |
| clock→dashboard   | 22.0 → **21.9** | 20.3 → 20.2 | 34 → **38** |

「前」= 同日 v6.9 固件（33 ms 动画节拍时代）的同方法测量。两列要一起
读：**frame_ms 是每帧成本，drawn 是运动采样密度**。v7.0 前动画被
33 ms 节拍硬锁在 30 fps，drawn 的普涨（+15~60%）是本轮最稳定的收益
——转场肉眼可见地变顺，即便 weather 系的每帧成本仍受渲染约束。

同日复测的散布（见 §3 内容敏感性）：clock→weather 在 28.2~36.0 之间
波动，dashboard↔clock 稳定在 ±1 ms 内。

## 2. v7.0 落了什么（对应提交）

1. **动画计时器跟随刷新档**（`main/ui_motion.c`）。LVGL 的 lv_anim
   timer 周期是编译期 LV_DEF_REFR_PERIOD=33 ms，不随 display refr
   timer 走；只抬显示档=运动仍采样在 30 Hz。apply_period 现在两个一起
   设。副作用是纯赚：16 ms 步进下每帧位移减半→脏区并集缩小，weather
   系 render_avg 单此一项降 ~15-25%。
2. **按键切换 async 到 LVGL 任务**（`main/button_router.c`）。根治
   v6.5 bake panic：`lv_snapshot_take`（完整软渲染）曾跑在按键任务的
   3072~3584 B 小栈上——慢按第 1 轮爆栈，快按测不出（转场未完成时每次
   按键都变 retarget，出/入场在 LVGL 大栈的 step_cb 里跑）。修后慢按/
   快按各 25 轮、49 张替身、0 失配、0 重启。
3. **status_bar 文本先比后写**（`main/status_bar.c`）。lv_label_set_text
   不比较内容，同文重设也 realloc+失效；三个标签每 tick（500 ms）白挨
   三块脏矩形。属待机卫生，非转场杠杆。

烘焙（`?bake`）维持默认关：16 ms 采样下 A/B 差 ±2 ms（v6.3 那笔 17%
是 33 ms 时代的账），台账见 `main/scene_trans.c` 的 s_bake_on 注释。

## 3. 方法学与注意事项

- **窗口法**：`?perf`（清）→ `dash btn` → 1.6 s → `?perf`（读）。窗口
  含转场全程 + 一小段空闲尾巴；frame_ms 只算真正渲染了的帧，尾巴稀释
  很小且各次一致。settle 1.6 s 是方法学常数，改了就不可比。
- **内容敏感性**：weather 系行的成本随【当前天气】的插画复杂度浮动
  （线段数因天气码而异），同日可差 ±20%。代码 A/B 必须两臂背靠背跑
  （同一天气内容）；提交的基线是历史参考点，跨天比对 weather 行超容差
  先怀疑天气变了，再怀疑回归。
- **按键映射**（设备实测，勿信旧文档）：BOOT=dashboard、PWR=clock、
  USER=weather；物理左→右 = BOOT, PWR, USER。
- **必须独占串口**：套 `with_port.ps1`，否则 hook 自启 bridge 中途抢
  COM9（v6.3 毁过三次采样）。
- **不变量随跑随查**（exit 2）：uptime 单调（重启=当年 bake panic 的
  信号形态：某行 drawn=0 + 后续时间戳归零）、`?ghost` mismatched=0、
  每行 drawn>0、held 计数符合预期表（dashboard↔clock 4+4，weather 配
  对 0+0）——held 日志是 v6.2 共享元素层唯一的漂移探测器。

## 4. 成本解剖——下一轮从这里接手

现状：转场是**纯渲染受限**（flush wait ~1 ms）。铁律依旧：**渲染成本
跟着"要重新生成的内容"走，不跟脏区面积走**（反例台账在
scene_trans.c / sdkconfig.defaults 原地）。

- weather 系的 25~31 ms/帧花在每帧重画运动中的全部内容：大插画 30 条
  抗锯齿线、5 日条带 15 个标签、HERO 88 大温度。
- **烘焙为何不再赚**：ARGB8888 替身 4 B/px 存 PSRAM，整屏级 blit 的
  PSRAM 带宽 ≈ 重新光栅化的成本，还要预付 ~6 帧等价的快照成本。
- **inval_n 每帧 60+ 不是杠杆**：lv_obj_set_x/y 走 style 路径，每次
  refresh_style 双重 invalidate + 屏级 layout 走查；矩形计数虚高 ~2×
  但同区域会在 inv 缓冲里合并，实际重画量没有翻倍。

候选杠杆（按预期收益排序，全部先量再动）：

1. **静置期预合成 weather 插画**：天气码变更时把插画烤成一张位图
   （每次数据变化付一次，转场按构造零成本）。要解的账：accent 呼吸
   （16 步/3 s）会要求位图重建，呼吸的收益 vs 预合成的收益要对量。
2. **上游给 lv_snapshot 加 RGB565A8**（3 B/px）：blit 带宽 −25%，
   可能把烘焙从 ±0 拉回正收益。改 lv_snapshot.c 白名单 + 重测。
3. **演员位移绕过 style 路径**（直接 move_to）：省双重失效 + layout
   走查。风险：rest 姿态契约（same_pose 读 style x/y）与布局回弹，
   收尾时须把 style 值补写回去。预期收益小（layout 走查在 refr_us 与
   render_us 的差值里，~2-4 ms/帧），风险中。

## 5. 复测

```powershell
# 采一轮（表格 + JSON 落盘 tools/perf/results/）
& ./tools/with_port.ps1 { python tools/perf/trans_bench.py --label myrun }

# 与最新 baseline 比对：>15% 回归 exit 1，不变量破坏 exit 2
& ./tools/with_port.ps1 { python tools/perf/trans_bench.py --compare baseline }

# 换代码后重立基线（新文件名带 baseline 前缀即可被 --compare baseline 找到）
& ./tools/with_port.ps1 { python tools/perf/trans_bench.py --label baseline-v7.1 }
```
