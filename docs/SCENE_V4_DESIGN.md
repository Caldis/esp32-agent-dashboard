# 场景体系 v4 设计 — 全手动切换 + 聚合页恢复 + 时钟页

> 状态: 设计定稿,待实施。本文档是下一个会话的实施依据。
> 日期: 2026-07-05 · 前置调研已完成(见 §1 考古结论,均已核实)

## 0. 目标(用户原话归纳)

1. **恢复聚合页**:idle 视图在最初设计中是"多个 agent 状态的聚合页面",不是单纯的睡眠屏。需要恢复。
2. **两个环境页面完全手动切换**:dashboard(multi-agent 平铺)与聚合页之间不再有任何自动切换,彻底解耦。
3. **新增时钟页**:顶部时间移到屏幕中央,iPhone 横屏充电(StandBy)风格的大号粗体时间;界面只有居中时间 + 底部 active/tokens;**入场时时钟数字从顶部位置平滑过渡到中央**,运动曲线要有 iOS 的流畅感,不能闪现。

## 1. 考古结论(已核实,不需要重查)

- v0(`15638ca`)有 7 个场景,其中 **`scene_sessions.c` 就是那个聚合页**:双栏 per-agent 视图(左 slot[0]=claude-code、右 slot[1]=codex;每栏 kind 标签 + 状态 pill + 最近 2 条 transcript + tokens;单 agent 时全宽)。
- 但它**从注册那天起就不可达**:没有任何 `switch_scene` 调用、不在按键导航里、不在自动切换逻辑里。属于"注册了但永远显示不出来"。
- 2026-06-21 提交 `1a3036c` "refactor: remove dead scenes (sessions/tokens/status)" 把它连同 tokens/status 场景一起删除。**恢复参考源码:`git show 1a3036c^:main/scenes/scene_sessions.c`**(widget 预分配 + tick 更新的模式值得沿用,布局不必照抄,见 §4)。
- `scene_idle.c` 从诞生(`d0b128f`)起就是纯 zZz 屏保,**没有被改坏**;丢的是 sessions 这个聚合页,而且严格说它从未真正工作过。
- 残留物:`main/harness/agent_commands.c` 的 `cmd_snapshot` 里还有 `strcmp(cur_id, "sessions")` 死判断(约 115 行);`main/tool_icons.h` 注释还提 "sessions dual-pane"。
- 现有 bug(顺带修复):有 agent 工作时手动切到 idle,副标题显示 "no agents"(`recently_stopped()` 在 `slot_count > 0` 时直接 return false)。重做聚合页后自然消失。

## 2. v4 场景体系总览

| 场景 | id(wire) | 定位 | 进入方式 |
|---|---|---|---|
| Dashboard | `dashboard` | multi-agent **平铺**(fleet 行卡片;单 agent 时 ambient 呼吸) | BOOT 循环 / default_scene |
| Overview 聚合页 | `idle`(保留,兼容 wire) | 跨 agent **聚合 rollup**;0 agent 时退化为 zZz 空状态 | BOOT 循环 |
| Clock 时钟页 | `clock`(新) | StandBy 风大字时钟 | BOOT 循环 |
| Prompt | `prompt` | 权限审批 takeover | 状态驱动(不变) |
| Awaiting | `awaiting` | 单 agent 等待输入 takeover | 状态驱动(不变) |

- BOOT 循环顺序 = 注册顺序:dashboard → overview → clock → dashboard。`cycle_view()`(button_router.c)已自动跳过 prompt/awaiting、awaiting 期间 pin 视图——**无需改动**,新场景注册后自动进入循环。
- 三个环境场景之间**只有 BOOT 手动切换**(以及 host 侧 `dash idle` 这类显式遥控命令,它算"手动")。
- prompt/awaiting 两个 takeover 保持状态驱动,但退出时必须**恢复用户先前所在的环境场景**,不能硬编码回 dashboard(见 §3)。

## 3. 变更 A — 环境场景全手动(先做,独立可验证)

**文件:`main/harness/agent_commands.c`**

`cmd_snapshot` 的 auto-pick 块(约 106–118 行)现状:

```c
if (result.prompt_set)                                    → switch_scene("prompt")   // 保留
else if (result.prompt_clear && cur == "prompt")          → switch_scene("dashboard") // 改:恢复先前场景
else if (result.total_now > 0 && cur == "idle")           → switch_scene("dashboard") // 删除
else if (result.total_now == 0 && cur == "sessions|dashboard") → switch_scene("idle") // 删除(含 sessions 死判断)
```

1. 删除后两条 ambient 自动切换分支(连带清掉 "sessions" 残留)。
2. prompt 的进入/退出改成 awaiting 同款"记住并恢复":加 `static int s_pre_prompt_scene_idx = -1;`
   - `cmd_prompt()` 和 `cmd_snapshot` 的 `prompt_set` 分支:切走前若当前不是 prompt/awaiting,记录 `scene_fw_current_index()`;
   - `prompt_clear` 分支:恢复记录的场景(<0 时回 index 0),清空记录。
   - 参考实现:`esp32_agent_dashboard_main.c` 的 `s_pre_awaiting_scene_idx`(93–138 行)——awaiting 已经做对了,**不要动它**,回归确认即可。
3. `dash idle` 命令保留(host 显式遥控,属于手动;stress.py / profile_scene.py 依赖它)。**可选**:加 `dash scene <id>` 泛化命令,方便测试三场景。

**连带影响(必须处理):**

- **dashboard 空状态**:0 agent 时不再被自动切走,dashboard 必须能体面地停留。检查 `scene_dashboard.c` 对 `slot_count == 0` 的渲染(grep 未见 empty 处理,大概率要补一个居中 dim 的 "no agents" 提示)。
- **`tools/mock_device_v1.py`**:mock 模拟了设备自动切场景(约 121、253 行,快照 0 agent → `current_scene = "idle"`),需同步改成手动语义,否则 bridge 测试与真机行为背离。
- **bridge 无需改**:`claude_buddy_bridge.py` 只推数据,从不发 `dash idle`(已 grep 核实)。
- 检查 `tools/test_bridge_selfheal.py`、`tools/test_firmware_architecture.py` 有无断言"0 agent 自动回 idle / 有 agent 自动回 dashboard"的用例,更新之。

## 4. 变更 B — Overview 聚合页(重做 scene_idle)

**文件:`main/scenes/scene_idle.c` → 建议重命名 `scene_overview.c`**(CMakeLists 同步;`scene_t.id` **保持 `"idle"`** 兼容 `dash idle` 与 NVS `default_scene` 旧值;`display_name` 改 `"Overview"`,description/tags 更新)。

**内容定位:聚合 rollup,不是 per-agent 平铺**(平铺是 dashboard 的职责;老 sessions 的双栏其实更接近平铺,不照抄,只借它的 widget 预分配模式)。

- **有 agent 时(`slot_count > 0`)**:
  - 中央大数字:live agent 总数;下一行 "N running · M waiting"(状态计数);
  - tokens 汇总:所有 slot 的 `tokens_today` 合计 + `tokens_cumulative` 合计(沿用 k/M 格式化,参考老 sessions 的 `format_tokens`);
  - 按 kind 分组一行:如 "cc ×2 · codex ×1";
  - (可选,二期)跨 agent 最新一条活动线。
- **无 agent 时(`slot_count == 0`)**:保留现有 zZz 呼吸点 + "no agents"/"agent just stopped" 逻辑作为空状态(代码已有,搬进条件分支即可)。
- 结构:init 预建两组 widget(rollup 组 + zzz 组),tick 里按 `slot_count` 切 hidden flag 并更新文本;短暂持 `agent_state_lock` 拷贝数据后释放再改 widget(全仓惯例)。
- status_bar 照常(顶部时间 + 底部 active/tokens)。
- 中文文本一律走 `cjk_font_get(px)`;纯 ASCII 用 montserrat(toast 只支持 ASCII,全仓惯例)。

## 5. 变更 C — Clock 时钟页(新 `main/scenes/scene_clock.c`)

**布局**:屏上只有两样东西(466×466 圆形 AMOLED):

- 居中大号 `HH:MM`;
- 底部 active/tokens:`status_bar_create()` 之后 `lv_obj_add_flag(sb.time_lbl, LV_OBJ_FLAG_HIDDEN)` 隐藏顶部小时钟即可(conn 状态提示照常保留,`status_bar_update` 对隐藏 label 继续 set_text 无害)。

**字体**:`cjk_font_get(~150)`——tiny_ttf 运行时按任意 px 渲染 zh.ttf,已有按 size 缓存的管线(`main/cjk_font.c`),数字在 GB2312 子集内。SimHei 数字偏细,要达到 StandBy 级粗体,后续可参考 `tools/make_cjk_font.py` 流程再嵌一个只含 `0-9:` 的 ExtraBold 子集 ttf(11 个字形,几 KB)+ 对应 accessor。**第一版先用 SimHei 150px 验证布局与动画,粗体作为 M4 后的增强,不阻塞。**

**时间源**:与 status_bar 完全一致(`host_epoch_unix` + `lv_tick_get() - host_clock_received_ms` 推算 + `host_tz_offset_seconds`;无 host 时钟显示 `--:--`)。把 status_bar.c 里的时间格式化抽成共享 helper(如 `status_bar_format_time(char*, size_t, const agent_state_t*)`),两处调用,勿复制粘贴。

**tick**:1s timer;分钟变化才 `lv_label_set_text`;`on_hide` 暂停 timer(照 idle 的模式)。

**入场过渡(核心需求)**:

- `on_show`:大时钟 label 先摆到**顶部小时钟的视觉位置**,`transform_scale` 起步值 ≈ `256 * 顶部字号 / 150`(顶部 48pt → 约 82/256),`transform_pivot` 设 label 中心;
- 两条并行 `lv_anim`:`y`(顶部 → 屏幕垂直中心)+ `transform_scale`(起步值 → 256),duration ≈ 550ms,**path = `apple_ease_out`**(`main/anim/apple_ease.h`,cubic-bezier(0.4,0,0.2,1),即 iOS 标准缓动,LUT 定点实现,现成);想要一点"落定"感可试 `apple_ease_spring`(0.5,1.5,0.5,1 轻微过冲)。
- `motion_reduced == true` 时跳过动画直接落位(`agent_state` 已有该 flag,scene_dashboard.c 的 `ambient_slide_to` 是现成示范)。
- **风险与降级**:LVGL 9.4 对 label 做 transform_scale 走中间 layer 渲染,150px tiny_ttf 大字逐帧重栅格化 + layer 内存(466 宽 ARGB)可能掉帧或压 heap(heap_watchdog 在跑)。若 A 方案不流畅,降级 B:**只动 y 不动 scale**,大钟固定终字号,从顶部滑入,同时 alpha 0→255 淡入——视觉上仍是"从顶部过渡到中间",省掉 layer 变换。先实现 A,`esp-harness screenshot` + 目测帧率决定。

**注册**:`esp32_agent_dashboard_main.c` 中 `scene_fw_register(&scene_clock)` 放在 idle(overview) 之后、prompt 之前;`main/CMakeLists.txt` 加源文件;`scenes.h` 加 extern。

## 6. 实施里程碑(每个独立提交 + 验证)

- **M1** 变更 A:手动切换解耦 + prompt 恢复先前场景 + dashboard 空状态 + mock/测试同步。
- **M2** 变更 B:overview 聚合页重做。
- **M3** 变更 C 静态版:clock 场景布局 + 1s tick + 大字(无动画)。
- **M4** clock 入场动画 + motion_reduced + 性能验证(必要时降级 B 方案)。
- 每个 M 之后:`esp-harness cycle`(build+flash+verify;注意 CLAUDE.md 的 COM9/autostart 契约)→ `dash btn boot` 模拟循环切换 → `esp-harness screenshot` 逐场景截图确认。

## 7. 验证清单

- [ ] BOOT 循环:dashboard → overview → clock → dashboard,toast 依次显示 view 名。
- [ ] 0 agent:三个环境场景都能停留;dashboard 显示空状态;**不再自动跳 idle**。
- [ ] agent 出现/工作中:停在 overview/clock 不被抢走;status_bar 与聚合数字照常刷新。
- [ ] prompt 到达:从 clock 进 prompt;决策/清除后**回 clock**(不是 dashboard)。
- [ ] awaiting takeover(单 agent):从 overview 进 awaiting,清除后回 overview(既有机制回归)。
- [ ] clock:无 host 时钟 `--:--`;跨分钟跳变;入场动画流畅无闪现;motion_reduced 直接落位。
- [ ] `tools/mock_device_v1.py` 行为与真机一致;stress.py / profile_scene.py 的 `dash idle` 依赖不破。
- [ ] 文档:PROTOCOL.md(若提及自动切换)、docs/CURRENT_ARCHITECTURE.md、README 场景表更新。

## 8. 开放问题(均给了默认建议,可直接做)

1. overview 聚合内容密度 → 建议先做 rollup 大数字版(M2),活动线二期再说。
2. 粗体数字字体 → 建议 Inter/Montserrat ExtraBold 数字子集;SimHei 150px 先顶。
3. `dash scene <id>` 泛化命令 → 建议顺手加(M1),测试三场景省事。
