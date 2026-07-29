# 转场渲染基线 — v7.1（2026-07-30）

场景转场（scene_trans：出场 → 黑幕瞬切 → 入场，约 1.1 s）的逐帧渲染
成本基线。这是继 [PERFORMANCE.md](PERFORMANCE.md)（v0.6 全栈基线，
桥/线缆/堆）之后的第二份可复现基线，聚焦一件事：**转场窗口内每一帧
花了多少毫秒、画了多少帧**。

本文件的每个数字都由 `tools/perf/trans_bench.py` 产出，结果 JSON 提交
在 `tools/perf/results/`。复测与回归判据见 §5。

## 1. v7.1 落了什么：weather 图标预合成（`?wxcomp`）

v7.0 基线的成本解剖指认 weather 系转场是纯渲染受限，钱花在**每帧重新
光栅化运动中的全部矢量内容**上。v7.1 落地了当时排名第一的杠杆：

**大插画（~35 个 lv_line/lv_arc/border-ring）与五日条带小图标（5×~7
个）不再以矢量对象上屏。** 矢量常驻一个钉在父容器裁剪区外的离屏工作台
（`scene_weather.c` 的 `stage_icon`），静置期用
`lv_snapshot_take_to_draw_buf` 烤成 ARGB8888 位图挂到屏上的 lv_image
（`wx_compose`）。转场帧里这些演员各只剩一次图像 blit；矢量重光栅化的
账（对象数 × 渲染分块 × 每帧）只在天气码变化或呼吸步进时付，而且付在
静置期。与 `?bake` 的替身不同，没有转场窗口内的快照成本。

关键工程点（都踩过对应的坑，见台账）：

- **格式 ARGB8888**：snapshot 与软渲染器双白名单的交集里唯一带 alpha
  的格式。LVGL 9.4 的 snapshot 白名单新收了 ARGB8565（3 B/px），但
  blend-to-RGB565 侧没有实现——只进一张白名单就是 v6.3「烘焙空转一个
  版本」的坑，勘察结论记档：**ARGB8565 目前不可用**。
- **合成身份缓存**（`big_code`/`day_code[]`）：`weather_on_show` 每次
  进场强制重画 + 数据重推常带来「码没变」的重渲染。合成是同步离屏
  渲染，6 张全烤 ≈ 30-50 ms 停顿，码没变就跳过重建+合成（标签照常改
  写）。工作台被小图标借用过则大插画必须重建（`stage_dirty`）——
  accent 登记指向台上对象，clean 后不重建就是悬垂指针。
- **呼吸照常**：accent 步进改台上矢量的 opa（批量写入照旧），然后重
  烤 + 一块矩形失效。屏上始终只有一个图像对象，转场无需知道呼吸存在。
- **大声降级**：合成失败自动翻回矢量直渲（`?wxcomp 0` 同路径）并
  WARN。`?wxcomp 0|1` 是常驻 A/B 开关；console task 只翻 flag，DOM
  形态统一在 weather_tick（LVGL task）上应用。

## 2. 数字

### 配对 A/B（同臂内容冻结，2 块 × 臂序对调，render_avg ms）

| 转场 | compose ON | OFF（矢量直渲） | Δ |
|---|---:|---:|---:|
| clock→weather     | 33.9 | 39.2 | **−5.3（−13.5%）** |
| dashboard→weather | 27.3 | 31.2 | **−3.9（−12.4%）** |
| weather→clock     | 22.9 | 25.5 | **−2.6（−10.2%）** |
| weather→dashboard | 19.3 | 20.3 | −1.0（−4.9%） |
| clock→dashboard（对照） | 19.5 | 18.9 | −0.6 ≈ 噪声 ✓ |

对照行（不含 weather 内容）两臂几乎相同，证明测量干净。drawn 同步
+1~2（运动采样更密）。**weather 静置呼吸重绘 render_avg 8.6 → 5.5 ms
（−36%）**——合成后的呼吸是一次离屏重烤 + 位图 blit，不再整片重画矢量
（`?wxbreath` 同法测得）。

### 提交的基线：`trans-2026-07-30-003209-baseline-v7.1.json`

（compose ON 默认路径；当时实况：阴天大图标 + 雨/雷为主的五日条带）

| 转场 | frame_ms | render_avg | drawn |
|---|---:|---:|---:|
| dashboard→clock   | 21.1 | 19.0 | 31 |
| clock→weather     | 38.1 | 33.7 | 25 |
| weather→dashboard | 22.8 | 20.3 | 37 |
| dashboard→weather | 30.2 | 27.1 | 30 |
| weather→clock     | 26.2 | 22.6 | 31 |
| clock→dashboard   | 22.7 | 20.9 | 33 |

**不要拿这张表跟 v7.0 基线表逐行比**：跨天的天气内容不同（±20% 敏感
性，v7.0 表当天是更轻的内容），结论以同内容配对 A/B 为准。

## 3. 方法学与注意事项（v7.1 增补）

- **窗口法**（不变）：`?perf`（清）→ `dash btn` → 1.6 s → `?perf`。
  settle 1.6 s 是方法学常数。
- **内容敏感性**（不变，本轮再次撞见）：weather 行随实况插画复杂度
  浮动 ±20%，单行离群（如一轮 c→w 41.3 ms / 918k inval）复测即回落。
  代码 A/B 必须同内容背靠背；桥要全程停着（数据冻结）。
- **辉光稳定**（新）：桥停 >12 s 设备浮现离线辉光。辉光淡入若压在
  bench 前几行上会造成臂序偏差——**杀桥后先睡 15 s 再起跑**，两臂辉光
  状态才对齐。
- **入场预热**（新）：bench 首个 weather 行含 on_show 强制重画。切换
  `?wxcomp` 后必须先访问一次 weather（≥3 s）让形态应用+首烤发生在
  窗口外，否则首行吃进 6 张快照的停顿。身份缓存落地后同码重入已免
  烤，此步是双保险。
- **臂序对调**（新）：一块 ON→OFF、一块 OFF→ON，两块聚合。臂序相关
  的系统性漂移（辉光/热/内容）由此抵消；`clock→dashboard` 当对照行。
- **首行污染**（新认识）：normalize 归位按压的 scene_flash 与 row 1
  （dashboard→clock）窗口部分重叠，该行历轮在 15.4-23.2 ms 之间摆动，
  与被测代码无关——读表时给 row 1 打折扣。
- **会话隔离**（新）：连续 python 会话背靠背开合串口会撞上回复错位
  （记忆中的 console 粘连坑）；命令与 bench 之间垫 1 s。曾观测到
  hook 竞态下**两个桥同时活着**抢 COM9、以及 reset_reason 落在
  "unknown"（未映射的 ESP_RST_USB 一类）的莫名重启——先怀疑环境
  （双桥/USB 复位），再怀疑固件；单会话连驱 24 次转场是干净的固件
  无罪证明法（scratchpad 的 repro 脚本模式）。
- **按键映射**（设备实测）：BOOT=dashboard、PWR=clock、USER=weather。
- **不变量随跑随查**（exit 2，不变）：uptime 单调、`?ghost`
  mismatched=0、每行 drawn>0、held 计数（dashboard↔clock 4+4，
  weather 配对 0+0）。

## 4. 成本解剖——下一轮从这里接手

转场仍是纯渲染受限（flush wait ~0.5 ms）。铁律依旧：**渲染成本跟着
「要重新生成的内容」走，不跟脏区面积走**（本轮新证据：合成后 weather
静置 inval_px 反而翻倍——位图整矩形失效——render 却 −36%）。

预合成之后，weather 转场帧的剩余成本主要在：15 个条带标签 + HERO 88
大温度 + BODY/LABEL 文本（tiny_ttf 字形 blit，故意不烤——稀疏内容烤
位图是已量化的负收益）、6 张位图的 PSRAM blit、以及 56 行分块下的
对象遍历开销。

候选杠杆（按预期收益排序，全部先量再动）：

1. **ESP32-S3 PIE SIMD blend**（`LV_DRAW_SW_ASM_CUSTOM`）：本仓 LVGL
   9.4 只带 NEON/Helium 汇编，S3 上 `CONFIG_LV_DRAW_SW_ASM_NONE=y`
   ——fill/mask-blend/image-blend 全是标量 C。Espressif 的
   esp_lvgl_port 有 S3 PIE 汇编（本机不存在，需引源码 + 验证 9.4
   宏契约 + 量 mask-blend 覆盖率）。文本/AA 线/位图 blit 全吃这条。
2. **演员位移绕过 style 路径**：省 refresh_style 双重失效 + layout
   走查（refr_us−render_us 差值 ~2-4 ms/帧）。风险中（rest 契约、
   same_pose 读 style x/y）。
3. **dashboard 环形呼吸的分块成本**：clock→dashboard 20.9 ms 里
   96px border-ring 每帧全 mask 光栅化（CIRCLE_CACHE 只 4 项且大半径
   不缓存）。环是密集小面积内容，或许适合同款预合成（注意 v6.3 台账
   ：整个 ambient 簇烤过一次是 −23%，环单独没量过）。
4. ~~RGB565A8 / ARGB8565 快照~~：双白名单勘察完毕，blend 侧无实现，
   除非上游补（记档防止重查）。

## 5. 复测

```powershell
# 采一轮（表格 + JSON 落盘 tools/perf/results/）
& ./tools/with_port.ps1 { python tools/perf/trans_bench.py --label myrun }

# 与最新 baseline 比对：>15% 回归 exit 1，不变量破坏 exit 2
& ./tools/with_port.ps1 { python tools/perf/trans_bench.py --compare baseline }

# A/B 一对（同块、辉光稳定、切臂后先预热 weather 再测）
& ./tools/with_port.ps1 {
  Start-Sleep -Seconds 15
  python tools/perf/trans_bench.py --label ab-on
  python -m esp_harness console --cmd "?wxcomp 0"; Start-Sleep -Seconds 1
  python -m esp_harness console --cmd "dash btn user"; Start-Sleep -Seconds 3
  python -m esp_harness console --cmd "dash btn boot"; Start-Sleep -Seconds 2
  python tools/perf/trans_bench.py --label ab-off
  python -m esp_harness console --cmd "?wxcomp 1"
}
```
