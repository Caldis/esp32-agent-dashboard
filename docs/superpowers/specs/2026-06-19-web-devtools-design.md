# Web Dev Tools for ESP32 Agent Dashboard — 设计

- 状态:草案,待用户审阅
- 日期:2026-06-19
- 相关:`docs/WEB_MIRROR.md`(旧的 WASM-only mirror 愿景,本设计取代之)、`PROTOCOL.md`、`docs/CURRENT_ARCHITECTURE.md`

## 1. 目标与非目标

### 一句话
让浏览器成为 ESP32 dashboard 的 **dev tools**:同一份数据处理代码、同一条交互链路,
ESP 有的 bug/数据/行为 web 都有,反之亦然;**只有渲染外观允许不同**。

### 目标
1. **数据链路同源**:浏览器跑固件**真实的 C 数据处理代码**(协议 tokenise、命令解析、
   `agent_state` 状态管理、snapshot 合并),编译成 WASM。不重新实现。
2. **bug/数据/行为一致**:因为是同一份 C 源,截断长度、slot prune、`tiny_json` 解析怪癖、
   G-7 tokeniser 引号处理、场景切换决策等边界行为逐字节一致。
3. **可交互端到端沙盒**:浏览器能点物理按键(BOOT/USER)、quick-reply、注入/回放 hook 事件,
   不依赖真机和真实 CC。
4. **真机 + web 并行**:`serve --web` 把**完全相同的 dash 字节流** fan-out 给真机和浏览器,
   两端随接随用、不重启。
5. **快速调试**:零前端构建(原生 JS),数据源(真实 CC / 回放 / 注入)随时切。

### 非目标
- **不追求像素级一致**:渲染层(LVGL scene 绘制)与 JS/Canvas 各写各的。UI 可以不一样,
  且 web 可加 ESP 上没有的调试可视化(检查 `agent_state` 内部、信号时间线等)。
- 不把 LVGL/BSP/board/scene 渲染代码编译进 WASM(那是旧 `WEB_MIRROR.md` 的重路线,放弃)。
- 不改 `Bridge`/`SnapshotPublisher` 的对外行为(只在 `DevicePusher` 内部做 fan-out)。

## 2. 核心架构

```
真实 CC ─hook→ hook_dispatch ─TCP:7321→ 真实 bridge (SessionRegistry / Publisher / 节流)
                                            │ DevicePusher.push("snapshot", …)  —— fan-out
                          ┌─────────────────┴─────────────────┐
                          ▼ 真机 sink (SessionHandle)          ▼ web sink (WebDeviceSink, 哑转发)
                  console_protocol.c                     原始 dash 行 ─SSE→ 浏览器
                  → agent_commands.c                                          │
                  → agent_state.c  ── 同 ─┐                                   ▼
                  → scene 渲染 (LVGL)      │ 一                      WASM (同一份 C 源)
                                          │ 份                      console tokeniser
                                          │ C                       → agent_commands.c
                                          │ 源                       → tiny_json.c
                  真机屏                    └─ 码 ─────────────────────→ agent_state.c (状态机)
                                                                          │ state_json() / signals()
                                                                          ▼
                                                                  JS / Canvas 渲染 (UI 可不同)
```

**关键保证**:两条链路都从**同一串 dash 字节流**开始,经过**同一份 C 数据处理代码**,
因此得到的 `agent_state` 完全相同。真机和 web 的差异**仅在最后的渲染**。

### 共享边界(契约面)
| 层 | 真机 | web | 是否共享 |
|---|---|---|---|
| 协议 tokenise | `console_protocol.c` | WASM 复用同款 G-7 tokeniser | ✅ 同源 |
| 命令解析 | `agent_commands.c` + `tiny_json.c` | 同一份编进 WASM | ✅ 同源 |
| 状态管理 | `agent_state.c` + `agent_snapshot_apply.c` | 同一份编进 WASM | ✅ 同源 |
| 场景切换**决策** | `scene_fw_show("prompt")` | shim 记录"当前场景"暴露给 JS | ✅ 决策同源,机制不同 |
| 像素**渲染** | `scenes/*.c`(LVGL) | `app.js`(Canvas) | ❌ 各写各的(允许) |
| 物理输入采集 | `buttons.c`(GPIO) | 浏览器点击 | ❌ 各写各的(允许) |
| 按键→决策语义 | BOOT=once / USER=deny(见 §6) | 同款映射,共享纯函数 | ✅ 同源 |

## 3. 组件清单

| 文件 | 动作 | 职责 |
|---|---|---|
| `tools/claude_buddy_bridge.py` | 改 | `DevicePusher` 单 sink → 多 sink fan-out;`cmd_serve` 加 `--web`/`--web-listen`;内嵌启动 web server,把 `WebDeviceSink` 注册为额外 sink、把 `bridge` 引用交给 server 供注入 |
| `tools/web/device_sink.py` | 新增 | `WebDeviceSink`:实现 `SessionHandle` 鸭子接口(`write_line/on_event/on_err/is_open/close`)。**哑转发**——`write_line` 广播原始行给 SSE 客户端;收到浏览器 POST 的决策时构造 `EVT:` 回调 `DevicePusher._on_evt`。不解析、不维护状态。 |
| `tools/web/serve.py` | 重写 | HTTP + SSE(下行) + POST(上行) server。可被 `cmd_serve` 内嵌(`--web`);也可独立运行连一个已跑的 bridge 的 TCP 设备口。 |
| `tools/web/wasm/` | 新增 | WASM 数据层:`wasm_shim.c`(stub)+ `wasm_api.c`(导出接口)+ `build_wasm.sh`(emcc)+ `build_native.sh`(cc,供测试)。源文件经 symlink/相对路径引用 `main/` 的真 C 文件,**不复制**。 |
| `tools/web/static/index.html` + `app.js` | 新增 | 加载 WASM;SSE 收 dash 行 → `dash_feed_line()`;读 `state_json()`/`signals()` → Canvas 渲染 7 场景 + 左侧 dev 面板;按键/注入 → POST。 |
| `tools/web/test_device_sink.py` | 新增 | `WebDeviceSink` 转发 + EVT 注入单测 |
| `tools/test_fanout.py` | 新增 | `DevicePusher` 多 sink 广播 + EVT 汇聚单测 |
| `tools/web/test_wasm_datalayer.py` | 新增 | 原生编译数据层(`build_native.sh`),喂样本 dash 命令断言 `state_json` —— **"相同 bug/数据"的回归锁** |
| `docs/WEB_MIRROR.md` | 改 | 更新为反映本设计(数据层 WASM + JS 渲染),旧 WASM-only 愿景标注为已取代 |

## 4. WASM 数据层

### 编译单元(全部纯 C,无 LVGL/BSP)
- `main/agent_state.c`
- `main/agent_commands.c`
- `main/harness/agent_snapshot_apply.c`
- `main/tiny_json.c`
- 复用 esp-harness 的 G-7 tokeniser(若无法直接取用,则在 `wasm_shim.c` 内放一份**与
  `mock_device_v1.py._tokenise` 和固件 `console_protocol.c` 等价**的实现,并由测试锁定一致)

### `wasm_shim.c`(把固件依赖桩成 web 可用形态)
| 固件符号 | shim 行为 |
|---|---|
| `xSemaphoreCreateMutex/Take/Give` | no-op(WASM 单线程) |
| `lv_tick_get()` | 单调毫秒计数(由 JS 注入 `now` 或内部累加) |
| `esp_get_free_heap_size` / `esp_get_minimum_free_heap_size` / `esp_timer_get_time` | 返回可配置假值(供 `dash health`) |
| `nvs_open/set_str/get_str/set_u8/get_u8/commit/close` | 内存 `key→value` map(config 持久化语义) |
| `bsp_display_lock/unlock` | no-op |
| `ESP_LOGx` | 转 `console.log`(可关) |
| `console_args_t` / `console_reply_ok/err` / `console_send_evt` / `console_begin_payload/write_raw/end_payload` / `console_protocol_register` / `console_cmd_t` | 捕获 reply/evt/payload 到一个**信号队列**,经 `signals()` 暴露给 JS dev 面板 |
| `scene_fw_find_by_id` / `scene_fw_show` / `scene_fw_current` | 维护一个 `current_scene` 变量;`scene_fw_show` 记录切换并入信号队列 |
| `push_banner_show(...)` | 入信号队列(banner 是渲染,数据层只发"被触发"信号) |
| `theme_*` | no-op(主题应用是渲染层) |

### `wasm_api.c`(JS ↔ WASM 契约,emscripten `EXPORTED_FUNCTIONS`)
```
void        dash_init(void);                 // agent_state_init + console_protocol_register(经 shim)
int         dash_feed_line(const char* line);// 喂一条 "dash …" 行,走真实 tokenise+分发,返回 reply 码
const char* state_json(void);                // 序列化整个 agent_state_t → JSON(渲染数据源)
const char* drain_signals(void);             // 取走自上次以来的 reply/evt/scene/banner 信号(JSON 数组)
const char* current_scene(void);             // 当前场景 id
const char* dash_button(const char* which);  // "boot"|"user" → 走共享决策语义,返回产生的 EVT 行(见 §6)
```
`state_json()` 是新写的序列化器(读 `agent_state_t` 全部字段),它是**渲染的唯一数据源**,
保证 JS 画的就是固件状态机算出来的东西。

## 5. 数据流(三条独立通路)

```
下行渲染:  bridge.push("snapshot",…) ─┬─→ 真机 sink ─→ 真机屏
                                      └─→ web sink(哑转发原始行)─SSE→ 浏览器
                                                              app.js: dash_feed_line(line)
                                                              → state_json()/drain_signals() → Canvas + 面板
上行决策:  浏览器点 BOOT/USER ─→ dash_button() 得 EVT 行 ─POST /decision→ serve
                                      → WebDeviceSink 构造 ReplyEvent → DevicePusher._on_evt
                                      → _permission_waiters[id] → 解除 CC 阻塞
事件注入:  浏览器点"注入/回放" ─POST /inject→ serve ─→ bridge.handle(raw)
                                      (与真实 CC hook、replay 完全同一入口)
```

## 6. 按键 → 决策语义(共享)

固件中 BOOT=approve(`once`)、USER=deny、60s 超时=deny 的映射目前分散在
`scene_prompt.c` / `buttons.c`(渲染/输入层)。为保证"交互一致",本设计:
- 第一版:在 `wasm_api.c` 的 `dash_button()` 内实现同款映射并产出标准
  `EVT: permission id=<id> decision=<once|deny> session_id=<id>` 行;
  由 `test_wasm_datalayer.py` 锁定与固件取值一致。
- 后续(可选小重构):把该映射从 `scene_prompt.c` 抽成纯函数
  `agent_decision_from_button()` 放入可共享层,真机与 WASM 同时引用,彻底消除重复。
- 60s 超时:web 侧由 `app.js` 计时触发 `dash_button` 的 deny 路径(与固件超时语义对齐)。

## 7. 传输:SSE + POST(零第三方依赖)

- 下行用 **Server-Sent Events**、上行用普通 **POST**,标准库 `http.server` 即可,
  **不引入 `websockets`**(贴合 "CI 只装 pyserial" 的极简依赖)。
- 上行只有按键/注入,低频,POST 足够。
- 端点:`GET /`(静态)、`GET /events`(SSE:dash 行 + 信号)、`POST /decision`、`POST /inject`、
  `GET /healthz`、`GET /static/*`(含 `.wasm`)。

## 8. 错误处理

- web sink 无客户端 → `write_line` 静默 no-op,不报错、不影响真机。
- 浏览器(重)连 SSE → server 发一个 **full-state 重放**(把当前会话已知的最新 snapshot 重发),
  浏览器 `dash_feed_line` 后从 `state_json()` 全量重绘;不依赖增量。
- 注入 JSON 非法 → serve 返回 400 + 错误,显示在面板,不打到 bridge。
- 真机不在(纯 web 调试)→ `--web` 配 `--dry-run` 或不给 `--port`,bridge 降级模式照常(已有逻辑)。
- WASM 加载失败 → 前端显示明确错误并给出 `build_wasm.sh` 指引;dev 面板的"原始信号/协议帧"
  仍可工作(它们不依赖 WASM),保证最基本的可观测性。

## 9. 测试

1. **数据层一致性(回归锁)** `test_wasm_datalayer.py`:用 `build_native.sh`(普通 cc)把同一份
   C 编成原生 `.so`,经 ctypes 喂 `tools/sample_session.jsonl` 派生的 dash 命令序列,断言
   `state_json()` 关键字段;并对若干已知边界(msg 截断、slot prune、awaiting 选项截断)断言,
   作为"web 与固件同 bug/同数据"的护栏。**不需要 emsdk**。
2. **fan-out** `test_fanout.py`:两个假 sink,push 一条断言都收到;一个抛异常断言另一个仍收到;
   EVT 从一个 sink 上来断言 "先到生效、后到丢弃"。
3. **web sink** `test_device_sink.py`:`write_line` 广播、POST 决策 → 构造正确 `EVT`。
4. **端到端冒烟(可进 CI,纯 Python)**:`serve --web --dry-run` → `replay sample_session.jsonl`
   → HTTP 订阅 `/events` 断言收到 snapshot 行 → POST 决策断言 prompt 解除。
5. (可选)Playwright 前端冒烟:断言 Canvas 在收到 snapshot 后切到 sessions 场景。

## 10. 构建与依赖

- **emsdk**:仅生成浏览器 `.wasm` 时需要(`tools/web/wasm/build_wasm.sh`,`emcc`)。
- **原生编译**:`build_native.sh`(`cc`)产出测试用 `.so`,CI 用它跑数据层一致性,**免 emsdk**。
- C 源**引用而非复制** `main/` 的文件(相对路径 / symlink),确保永远跟随固件最新代码。
- 构建产物 `tools/web/static/*.wasm` + JS loader 不入库(`.gitignore`),由脚本生成。

## 11. 启动方式

```bash
# 真机 + web 同时(fan-out):插着 COM9,浏览器也能看
python tools/claude_buddy_bridge.py serve --port-kind serial --port COM9 --web
# 纯 web(没真机):
python tools/claude_buddy_bridge.py serve --web --dry-run
# → 浏览器开 http://127.0.0.1:8765/ ;真机随时插上自动也显示,无需重启
```

## 12. 风险与开放问题

1. **G-7 tokeniser 取用方式**:优先直接编译 esp-harness 的 `console_protocol.c`;若其依赖过重,
   退化为 `wasm_shim.c` 内置等价实现 + 一致性测试。需在实现首步验证。
2. **`agent_commands.c` 中 console 注册流程**:`console_protocol_register` 在固件注册命令表;
   WASM 下 `dash_feed_line` 需要复用同一分发逻辑。需确认命令分发能脱离真实 console runloop 调用。
3. **按键决策语义抽取**(§6)是否纳入首版,影响 `scene_prompt.c` 是否要小重构。
4. **state_json 序列化器**是新代码,本身可能引入 web 独有 bug;以测试覆盖关键字段缓解。
5. emsdk 为新的开发前置依赖;通过"原生编译可独立测试数据层"把它隔离在"出浏览器产物"这一步。

## 13. 实施顺序(供后续 writing-plans 细化)

1. WASM 数据层骨架:`wasm_shim.c` + `wasm_api.c` + `build_native.sh` + `test_wasm_datalayer.py`
   (先用原生编译跑通一致性,不碰浏览器)。
2. `build_wasm.sh` 出浏览器 `.wasm` + 最小 `app.js` 加载并 `dash_feed_line` 一条样本。
3. `DevicePusher` fan-out + `test_fanout.py`。
4. `WebDeviceSink` + `serve.py`(SSE/POST)+ `test_device_sink.py`;`cmd_serve --web` 内嵌。
5. 前端渲染 7 场景 + dev 面板 + 按键/注入;端到端冒烟。
6. 更新 `docs/WEB_MIRROR.md`、README、`docs/CURRENT_ARCHITECTURE.md`。
