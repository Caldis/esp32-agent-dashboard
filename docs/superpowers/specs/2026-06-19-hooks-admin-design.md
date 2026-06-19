# Hooks Admin(agent hooks 管理)— 设计

- 状态:草案,待用户审阅
- 日期:2026-06-19
- 相关:`tools/hook_dispatch.py`(共用转发脚本)、`tools/claude_buddy_bridge.py`、web dev tools(spec `2026-06-19-web-devtools-design.md`)

## 1. 目标与非目标

### 一句话
给 web dev tools 加一个 **hooks 管理**能力:检测 agent 是否装了上报 hook、一键安装、软启用/禁用,方便改完 hook 立刻调试。**第一版覆盖 Claude Code 与 Codex,但架构必须让"加新 agent"是低成本的增量。**

### 目标
1. **检测(status)**:每个 agent 显示三态——未装 / 已装启用 / 已装禁用,以及覆盖了哪些事件、配置文件在哪。
2. **安装(install)**:把 PreToolUse/PostToolUse/Stop 三个 hook 写进 agent 配置,指向本项目 `hook_dispatch.py`。
3. **软禁用/启用**:disable 保留配置但不生效(条目搬到旁置 state),enable 一键恢复。频繁开关不丢手改过的细节。
4. **两个入口**:CLI(`hooks_admin`)+ web(dev tools 面板),共用同一核心。
5. **可扩展**:加新 agent = 新增一个 adapter + 注册一行,核心零改动。这是首要架构约束(用户明确:不是一锤子买卖)。
6. **安全**:改用户配置前备份;只动"我们的"条目,绝不碰用户其它 hooks。

### 非目标
- 不做单事件粒度的开关(第一版以 agent 为单位整体 install/enable/disable;三事件作为一组)。
- 不接管 Codex 的全局 `[features] hooks` 开关(那会误伤用户其它 hooks)。
- 不在第一版实现"无声明式 hook 的 agent"的自动安装(adapter 接口为其预留 `supports=False` 的降级路径,但具体 agent 留待后续)。
- 不改 `main/` 固件、不引入第三方依赖(读 TOML 用标准库 `tomllib`;写配置一律 JSON)。

## 2. 架构

```
                         ┌─────────────────────────────────────┐
 CLI  hooks_admin.py ───▶│  core: orchestration                │
 web  hooks_server.py ──▶│   status / install / enable / disable│
                         │   + 旁置 state + 备份 + 三态判定      │
                         └───────────────┬─────────────────────┘
                                         │ 通过统一接口调用
                         ┌───────────────▼─────────────────────┐
                         │  AGENTS 注册表(kind → adapter)      │
                         │   claude_code | codex | <future...>  │
                         └──────────────────────────────────────┘
   每个 adapter 封装:配置文件路径、格式读写、"哪条是我们的"识别、supports
```

### 文件结构(包,便于每 agent 一文件扩展)
```
tools/hooks_admin/
  __init__.py        # 核心编排 + AGENTS 注册表 + status/install/enable/disable
  base.py            # adapter 接口(Protocol) + 数据类型 AgentHookStatus + 通用 helper
  state.py           # 旁置 state 读写 + 配置文件备份
  claude_code.py     # ClaudeCodeAdapter
  codex.py           # CodexAdapter
  cli.py             # argparse CLI(被 `python -m tools.hooks_admin` 或 tools/hooks_admin.py 调)
tools/hooks_admin.py # 薄入口:from tools.hooks_admin.cli import main; main()
tools/web/hooks_server.py   # 标准库 http.server,GET /hooks + POST /hooks/{install,enable,disable}
tools/web/static/hooks.html + hooks.js  # dev tools hooks 面板
tools/test_hooks_admin.py   # round-trip 测试(CC + Codex,临时目录)
```
> 用包而非单文件,是因为用户明确要"后续兼容更多 agent":每个 agent 一个 adapter 文件,互不干扰,核心稳定。

## 3. Adapter 接口(扩展点,核心)

`tools/hooks_admin/base.py`:
```python
EVENTS = ("PreToolUse", "PostToolUse", "Stop")   # 第一版安装的三个事件

@dataclass
class AgentHookStatus:
    kind: str                  # "claude-code" / "codex" / ...
    display_name: str
    supported: bool            # 该 agent 是否支持声明式 hook
    installed: bool            # 我们的条目存在于 agent 配置 或 旁置 state
    enabled: bool              # 我们的条目当前在 agent 配置里生效
    events: list[str]          # 当前已覆盖的事件(已启用时)
    config_path: str           # 主配置文件路径(给 UI 显示)
    detail: str = ""           # 人类可读补充(如 "manual: see docs")

class HookAgentAdapter(Protocol):
    kind: str
    display_name: str
    supported: bool            # 声明式 hook 是否可自动管理(否 → install 仅给提示)
    def config_paths(self, scope: str) -> list[Path]: ...   # 检测要读的文件(可多个)
    def detect(self, scope: str) -> tuple[bool, dict | None]: ...
        # (agent 配置里是否存在我们的条目, 这些条目的原生内容或 None)
    def install(self, scope: str, command_for: Callable[[str], str]) -> dict: ...
        # 用 command_for(event) 生成本 agent 原生格式的三事件条目并写入,返回写入的条目
    def remove(self, scope: str) -> dict: ...               # 移除我们的条目,返回被移除的原生条目(供 state 保存)
    def restore(self, scope: str, entries: dict) -> None: ...  # 把 state 保存的原生条目原样写回(enable;保留用户手改)
```

核心通过 `AGENTS: dict[str, HookAgentAdapter]` 注册表分发。**加新 agent**:实现一个 adapter、在注册表加一行;若该 agent 无声明式 hook,设 `supported=False` 并让 `write_our_entries` 写文档提示(核心据 `supported` 调整 status/CLI 行为)。

## 4. 核心编排语义

- **命令行生成**:核心 `_command_line(event, kind) -> "<sys.executable> <abs>/tools/hook_dispatch.py <event_snake> <kind>"`(同一转发脚本服务所有 agent;event_snake 如 `pre_tool_use`)。调 adapter 时传绑定了该 kind 的 `command_for = lambda e: _command_line(e, adapter.kind)`。
- **status(agent=all, scope)**:对每个 adapter:`detect()` + 读旁置 state →
  - 配置里有 → enabled;配置里无但 state 里有 → disabled;两边都无 → not-installed。
- **install(agent, scope)**:`adapter.install(scope, command_for)` 返回写入的条目 → 记入 state(enabled=true)。幂等(已装则覆盖更新)。
- **disable(agent, scope)**:`adapter.remove(scope)` → 返回的条目存入 state(enabled=false)。
- **enable(agent, scope)**:从 state 取条目 → `adapter.restore(scope, entries)` 原样写回(**保留你手改的 matcher/timeout 等**)。state 无记录时回退为 install。

## 5. CC / Codex adapter 细节

### ClaudeCodeAdapter（supported=True）
- 配置:`~/.claude/settings.json`(scope=user)/ `<project>/.claude/settings.json`(scope=project)。
- 格式:`hooks.<Event>` 是数组,元素 `{"matcher": "...", "hooks": [{"type":"command","command":"..."}]}`。PreToolUse/PostToolUse 用 `matcher:"*"`(全工具),Stop 用空 matcher。
- "我们的"识别:command 字符串含 `hook_dispatch.py` 且其绝对路径解析到本项目根。
- 写入时保留用户其它数组元素,只增/删我们的那一个 per event。

### CodexAdapter（supported=True）
- 检测读:`~/.codex/config.toml`(`tomllib` 只读,解析 `[[hooks.<Event>]]`)**和** `~/.codex/hooks.json`。
- 写入:`~/.codex/hooks.json`(JSON,与 CC 结构同构;避免 TOML 写依赖)。若检测到用户已在 `config.toml` 配了我们的条目,status 标注、install 提示"已存在于 config.toml"避免重复/双写告警。
- "我们的"识别同 CC(command 含本项目 hook_dispatch.py)。
- hook IO 与 CC 相同(stdin JSON → stdout `{continue,...}`),故 `hook_dispatch.py` 直接复用,无需改。

## 6. 旁置 state 与安全

- state:`~/.config/esp32-dashboard/hooks-state.json`(跨项目共用;Windows 走 `%APPDATA%` 回退)。结构:`{ "<kind>": {"enabled": bool, "scope": str, "entries": {<event>: <full entry>}} }`。
- 备份:写任何用户配置文件前复制为 `<file>.esp32bak`(每次覆盖最近一次)。
- 只匹配/改"我们的"条目;用户其它 hooks 原样保留(round-trip 测试强制验证)。
- install/enable/disable 幂等且可重入。

## 7. CLI 与 web

- CLI:`python tools/hooks_admin.py <status|install|enable|disable> [--agent claude-code|codex|all] [--scope user|project] [--json]`。`status` 默认人类可读表格,`--json` 输出结构化(给 web/脚本)。
- web 后端 `tools/web/hooks_server.py`(标准库 http.server,独立端口,不依赖 Step 3 的 serve.py):
  - `GET /hooks` → `status(all)` 的 JSON。
  - `POST /hooks/install|enable|disable`,body `{"agent": "...", "scope": "..."}` → 调核心 → 返回新 status。
- 前端 `hooks.html`+`hooks.js`:每 agent 一张卡(display_name、三态 pill、events、config_path)+ Install/Enable/Disable 按钮;按钮 POST 后刷新 status。

## 8. 测试

`tools/test_hooks_admin.py`(标准库,临时目录,免真实改用户文件):
- 用注入的 fake home(adapter 的 config_paths 接受可覆盖的根)对 **CC 与 Codex 各跑 round-trip**:install → status(enabled,events 齐)→ disable → status(disabled)→ enable → status(enabled)。
- **保用断言**:配置文件里预置一个"用户自己的"无关 hook,确认 install/disable/enable 全程不动它。
- command_for 生成的命令行格式断言(含 hook_dispatch.py 绝对路径 + kind)。
- adapter 注册表可发现性:遍历 AGENTS 对每个 supported adapter 跑同一组 round-trip(新增 agent 自动纳入测试)。

## 9. 扩展:如何加一个新 agent

1. 在 `tools/hooks_admin/<agent>.py` 实现 `HookAgentAdapter`(config_paths / read_our_entries / write_our_entries / remove_our_entries,设 `supported`)。
2. 在 `__init__.py` 的 `AGENTS` 注册表加一行。
3. 第 8 节的注册表遍历测试自动覆盖它的 round-trip;若该 agent 无声明式 hook(`supported=False`),只需让 write_our_entries 产出文档提示,status 显示 "manual"。
4. 前端面板自动多出一张卡(由 `GET /hooks` 数据驱动,无需改前端)。

## 10. 风险与开放问题

1. **CC settings.json hooks 确切格式**:第一版用数组 `[{matcher,hooks:[...]}]`(当前 CC 格式);实施首步用一次真实 CC 验证写入被识别。README 里的简化 string 形式若是旧格式,以真实格式为准。
2. **Codex hooks.json 在 ~/.codex/ 的 user-level 支持**:文档称"alongside config files";实施首步验证 `~/.codex/hooks.json` 被 Codex 识别;若只认 config.toml,则 CodexAdapter 改写 config.toml(引入 `tomli_w` 或最小手写,届时再定)。
3. **scope=project 的 Codex 配置层**位置需在实现时确认(Codex 支持多 config 层)。
4. state 与多机/同步:`~/.config/esp32-dashboard/` 不随仓库走,换机需重新 install——可接受(hooks 本就是机器本地配置)。

## 11. 实施顺序(供 writing-plans 细化)

1. `base.py`(接口+类型)+ `state.py`(state/备份)+ `claude_code.py` + 核心 `status/install/enable/disable` + CLI + `test_hooks_admin.py`(先只 CC 跑通 round-trip + 保用断言)。
2. `codex.py`(Codex adapter)+ 注册表遍历测试纳入 Codex。
3. `hooks_server.py`(web 后端)+ `hooks.html`/`hooks.js`(面板)。
4. 文档更新(README/CLAUDE.md 的 hooks 安装段改为指向 hooks_admin)。
