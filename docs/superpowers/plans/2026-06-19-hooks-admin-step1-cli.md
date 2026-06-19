# Hooks Admin — 第 1 阶段:CLI(核心 + CC/Codex adapter)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `tools/hooks_admin/` 包:一个可扩展的 agent hook 管理器,通过 adapter 抽象支持 Claude Code 与 Codex,提供 `status`(三态)/`install`/`enable`(软启用)/`disable`(软禁用)+ CLI,全程只动"我们的"条目、改配置前备份、软禁用保留手改。web 面板是下一阶段。

**Architecture:** 核心编排(status/install/enable/disable)只懂通用流程 + 旁置 state + 备份;每个 agent 是一个实现统一接口的 adapter(封装配置文件路径、格式读写、"哪条是我们的"识别)。`AGENTS` 注册表分发。加新 agent = 新 adapter + 注册一行。CC 写 `~/.claude/settings.json`;Codex 写 `~/.codex/hooks.json`(检测时也读 `config.toml`)。三事件 PreToolUse/PostToolUse/Stop 作为一组,command 指向共用的 `tools/hook_dispatch.py`。

**Tech Stack:** Python 3.11+ 标准库(`json`、`tomllib` 只读、`pathlib`、`argparse`、`shutil`、`tempfile`、`unittest`)。无第三方依赖。

## Global Constraints

- **不修改 `main/` 固件、不引入第三方依赖**(读 TOML 用标准库 `tomllib`;写配置一律 JSON)。
- **只操作"我们的"条目**:command 字符串含 `hook_dispatch.py` 视为我们的;绝不增删/改用户其它 hooks。
- **改任何用户配置文件前备份**为 `<file>.esp32bak`。
- 三事件固定一组:`EVENTS = ("PreToolUse", "PostToolUse", "Stop")`;event→snake:`{PreToolUse:"pre_tool_use", PostToolUse:"post_tool_use", Stop:"stop"}`。
- command 行:`"<sys.executable> <abs>/tools/hook_dispatch.py <event_snake> <kind>"`(`hook_dispatch.py` 绝对路径)。
- 软禁用 = 把我们的条目从 agent 配置搬到旁置 state(`enable` 用 state 原样写回,**保留手改**);**不**用 Codex 全局 `[features] hooks` 开关。
- 三态:not-installed / enabled(配置里有我们的条目)/ disabled(state 里有、配置里没有)。
- **可测试注入**:所有 home 路径(`~/.claude`、`~/.codex`、state 目录)必须可通过构造参数注入,测试用临时目录,绝不碰真实用户文件。
- 新代码位于 `tools/hooks_admin/`(包,含 `__main__.py`,用 `python -m tools.hooks_admin` 运行)+ 测试 `tools/test_hooks_admin.py`;并创建空 `tools/__init__.py` 使 `tools` 可作为包导入(不影响现有 `python tools/xxx.py` 直接运行)。

---

### Task 1: 基础设施 — base(接口/类型/命令行)+ state(旁置 state/备份)

**Files:**
- Create: `tools/hooks_admin/__init__.py`(本任务留空或仅版本注释)
- Create: `tools/hooks_admin/base.py`
- Create: `tools/hooks_admin/state.py`
- Create: `tools/test_hooks_admin.py`

**Interfaces:**
- Produces:
  - `base.EVENTS`、`base.EVENT_SNAKE`、`base.command_line(hook_dispatch_path, event, kind) -> str`、`@dataclass base.AgentHookStatus`、`base.HookAgentAdapter`(Protocol)、`base.PROJECT_ROOT`、`base.hook_dispatch_path() -> Path`。
  - `state.State`:`load()`/`save()`、`get(kind)`/`set(kind, enabled, scope, entries)`/`clear(kind)`;`state.backup(path: Path)`。`State(path: Path)` 路径可注入。

- [ ] **Step 1: 写失败测试(命令行 + state round-trip)**

`tools/test_hooks_admin.py`:
```python
"""hooks_admin 测试 —— 全程用临时目录,绝不碰真实用户文件。
运行:python tools/test_hooks_admin.py"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # 仓库根,便于 import tools.*
from tools.hooks_admin import base, state


class TestBase(unittest.TestCase):
    def test_command_line(self):
        cmd = base.command_line(Path("/proj/tools/hook_dispatch.py"), "PreToolUse", "claude-code")
        assert "hook_dispatch.py" in cmd, cmd
        assert "pre_tool_use" in cmd, cmd
        assert "claude-code" in cmd, cmd

    def test_event_snake_covers_all(self):
        for e in base.EVENTS:
            assert e in base.EVENT_SNAKE, e


class TestState(unittest.TestCase):
    def test_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            st = state.State(Path(d) / "hooks-state.json")
            assert st.get("claude-code") is None
            st.set("claude-code", enabled=False, scope="user", entries={"Stop": {"x": 1}})
            st.save()
            st2 = state.State(Path(d) / "hooks-state.json")  # reload
            rec = st2.get("claude-code")
            assert rec and rec["enabled"] is False and rec["entries"]["Stop"]["x"] == 1, rec

    def test_backup(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "settings.json"
            p.write_text('{"a":1}', encoding="utf-8")
            state.backup(p)
            assert (Path(d) / "settings.json.esp32bak").read_text(encoding="utf-8") == '{"a":1}'


if __name__ == "__main__":
    unittest.main(verbosity=2)
```

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/test_hooks_admin.py`
Expected: FAIL/ERROR —— `ModuleNotFoundError: tools.hooks_admin` 或 `base`/`state` 不存在。
(从仓库根运行;`tools/` 下需要 `tools/__init__.py` 存在以支持 `from tools.hooks_admin import ...`;若不存在,Step 3 一并创建空 `tools/__init__.py`。)

- [ ] **Step 3: 创建 tools/__init__.py + base.py / state.py / 包 __init__.py**

`tools/__init__.py`(空文件;使 `tools` 可被 `import tools.hooks_admin`,不影响现有 `python tools/xxx.py` 直接运行):
```python
# tools 包标记(空)
```

`tools/hooks_admin/__init__.py`:
```python
"""hooks_admin — 可扩展的 agent hook 管理器(CC + Codex,核心 + adapter)。"""
```

`tools/hooks_admin/base.py`:
```python
from __future__ import annotations
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Protocol

# tools/hooks_admin/base.py → 仓库根 = parents[2]
PROJECT_ROOT = Path(__file__).resolve().parents[2]

EVENTS = ("PreToolUse", "PostToolUse", "Stop")
EVENT_SNAKE = {"PreToolUse": "pre_tool_use", "PostToolUse": "post_tool_use", "Stop": "stop"}


def hook_dispatch_path() -> Path:
    return PROJECT_ROOT / "tools" / "hook_dispatch.py"


def command_line(hook_dispatch: Path, event: str, kind: str) -> str:
    """生成 hook 命令行:<python> <abs hook_dispatch.py> <event_snake> <kind>。"""
    return f'"{sys.executable}" "{hook_dispatch}" {EVENT_SNAKE[event]} {kind}'


@dataclass
class AgentHookStatus:
    kind: str
    display_name: str
    supported: bool
    installed: bool
    enabled: bool
    events: list[str] = field(default_factory=list)
    config_path: str = ""
    detail: str = ""


class HookAgentAdapter(Protocol):
    kind: str
    display_name: str
    supported: bool
    def config_paths(self, scope: str) -> list[Path]: ...
    def detect(self, scope: str) -> tuple[bool, dict | None]: ...
    def install(self, scope: str, command_for: Callable[[str], str]) -> dict: ...
    def remove(self, scope: str) -> dict: ...
    def restore(self, scope: str, entries: dict) -> None: ...
```

`tools/hooks_admin/state.py`:
```python
from __future__ import annotations
import json
import os
import shutil
from pathlib import Path


def default_state_path() -> Path:
    """~/.config/esp32-dashboard/hooks-state.json(Windows 走 %APPDATA%)。"""
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
        return base / "esp32-dashboard" / "hooks-state.json"
    return Path.home() / ".config" / "esp32-dashboard" / "hooks-state.json"


def backup(path: Path) -> None:
    """改用户配置前备份为 <file>.esp32bak;文件不存在则跳过。"""
    if path.exists():
        shutil.copy2(path, path.with_name(path.name + ".esp32bak"))


class State:
    def __init__(self, path: Path):
        self.path = path
        self._data: dict = {}
        if path.exists():
            try:
                self._data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                self._data = {}

    def get(self, kind: str) -> dict | None:
        return self._data.get(kind)

    def set(self, kind: str, *, enabled: bool, scope: str, entries: dict) -> None:
        self._data[kind] = {"enabled": enabled, "scope": scope, "entries": entries}

    def clear(self, kind: str) -> None:
        self._data.pop(kind, None)

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self._data, indent=2), encoding="utf-8")
```

- [ ] **Step 4: 运行,确认通过**

Run: `python tools/test_hooks_admin.py`
Expected: PASS —— TestBase + TestState 全绿。

- [ ] **Step 5: Commit**

```bash
git add tools/__init__.py tools/hooks_admin/__init__.py tools/hooks_admin/base.py tools/hooks_admin/state.py tools/test_hooks_admin.py
git commit -m "feat(hooks-admin): base interface + side-state + backup with tests"
```

---

### Task 2: ClaudeCodeAdapter + 核心 status/install(CC round-trip)

**Files:**
- Create: `tools/hooks_admin/claude_code.py`
- Modify: `tools/hooks_admin/__init__.py`(加 `AGENTS` 注册表 + `status()` + `install()`)
- Modify: `tools/test_hooks_admin.py`(加 CC install/status 测试)

**Interfaces:**
- Consumes: `base.*`、`state.State`。
- Produces:
  - `claude_code.ClaudeCodeAdapter(home: Path)` 实现 `HookAgentAdapter`;`settings_path(scope)`。
  - `__init__.build_agents(homes: dict | None = None) -> dict[str, HookAgentAdapter]`(homes 可注入,默认真实 `~`)。
  - `__init__.status(agents, st: state.State, scope="user") -> dict[str, base.AgentHookStatus]`。
  - `__init__.install(agents, st, kind, scope="user") -> base.AgentHookStatus`。

- [ ] **Step 1: 写失败测试(CC install → status enabled,保用断言)**

在 `tools/test_hooks_admin.py` 增加:
```python
from tools import hooks_admin
from tools.hooks_admin import claude_code, state as state_mod


def _fake_homes(d: Path):
    return {"claude-code": d / "cc_home", "codex": d / "cx_home"}


class TestClaudeCodeInstall(unittest.TestCase):
    def test_install_then_status_enabled_preserves_user_hook(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            cc_home = d / "cc_home"
            (cc_home / ".claude").mkdir(parents=True)
            # 预置一个用户自己的无关 hook
            settings = cc_home / ".claude" / "settings.json"
            settings.write_text(json.dumps({
                "hooks": {"PreToolUse": [
                    {"matcher": "Read", "hooks": [{"type": "command", "command": "echo user-own"}]}
                ]}
            }), encoding="utf-8")

            agents = hooks_admin.build_agents(_fake_homes(d))
            st = state_mod.State(d / "state.json")

            hooks_admin.install(agents, st, "claude-code", scope="user")

            data = json.loads(settings.read_text(encoding="utf-8"))
            pre = data["hooks"]["PreToolUse"]
            # 用户自己的 hook 仍在
            assert any("echo user-own" in h["hooks"][0]["command"] for h in pre), pre
            # 我们的 hook 已加(三事件)
            for ev in base.EVENTS:
                arr = data["hooks"][ev]
                assert any("hook_dispatch.py" in hh["command"]
                           for it in arr for hh in it["hooks"]), (ev, arr)
            # 备份已建
            assert settings.with_name("settings.json.esp32bak").exists()

            stt = hooks_admin.status(agents, st)["claude-code"]
            assert stt.installed and stt.enabled, stt
            assert set(stt.events) == set(base.EVENTS), stt.events


if __name__ == "__main__":
    unittest.main(verbosity=2)
```

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/test_hooks_admin.py`
Expected: FAIL —— `hooks_admin.build_agents` / `claude_code` 不存在。

- [ ] **Step 3: 实现 claude_code.py + 核心 status/install**

`tools/hooks_admin/claude_code.py`:
```python
from __future__ import annotations
import json
from pathlib import Path
from typing import Callable

from . import base
from .state import backup

MARK = "hook_dispatch.py"  # 识别"我们的"条目


def _matcher_for(event: str) -> str | None:
    # PreToolUse/PostToolUse 针对工具,用 "*";Stop 不针对工具,无 matcher。
    return "*" if event in ("PreToolUse", "PostToolUse") else None


def _is_ours(item: dict) -> bool:
    return any(MARK in (h.get("command") or "") for h in item.get("hooks", []))


class ClaudeCodeAdapter:
    kind = "claude-code"
    display_name = "Claude Code"
    supported = True

    def __init__(self, home: Path):
        self.home = home

    def settings_path(self, scope: str) -> Path:
        # scope=user → ~/.claude/settings.json(本阶段只实现 user)
        return self.home / ".claude" / "settings.json"

    def config_paths(self, scope: str) -> list[Path]:
        return [self.settings_path(scope)]

    def _read(self, scope: str) -> dict:
        p = self.settings_path(scope)
        if p.exists():
            try:
                return json.loads(p.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                return {}
        return {}

    def _write(self, scope: str, data: dict) -> None:
        p = self.settings_path(scope)
        p.parent.mkdir(parents=True, exist_ok=True)
        backup(p)
        p.write_text(json.dumps(data, indent=2), encoding="utf-8")

    def detect(self, scope: str) -> tuple[bool, dict | None]:
        data = self._read(scope)
        hooks = data.get("hooks", {})
        found: dict = {}
        for ev in base.EVENTS:
            for item in hooks.get(ev, []):
                if _is_ours(item):
                    found[ev] = item
                    break
        return (bool(found), found or None)

    def _entry(self, event: str, command: str) -> dict:
        node = {"hooks": [{"type": "command", "command": command}]}
        m = _matcher_for(event)
        if m is not None:
            node = {"matcher": m, **node}
        return node

    def install(self, scope: str, command_for: Callable[[str], str]) -> dict:
        data = self._read(scope)
        hooks = data.setdefault("hooks", {})
        written: dict = {}
        for ev in base.EVENTS:
            arr = hooks.setdefault(ev, [])
            arr[:] = [it for it in arr if not _is_ours(it)]   # 去掉旧的我们的
            entry = self._entry(ev, command_for(ev))
            arr.append(entry)
            written[ev] = entry
        self._write(scope, data)
        return written

    def remove(self, scope: str) -> dict:
        data = self._read(scope)
        hooks = data.get("hooks", {})
        removed: dict = {}
        for ev in base.EVENTS:
            arr = hooks.get(ev, [])
            ours = [it for it in arr if _is_ours(it)]
            if ours:
                removed[ev] = ours[0]
            arr[:] = [it for it in arr if not _is_ours(it)]
            if not arr:
                hooks.pop(ev, None)
        self._write(scope, data)
        return removed

    def restore(self, scope: str, entries: dict) -> None:
        data = self._read(scope)
        hooks = data.setdefault("hooks", {})
        for ev, entry in entries.items():
            arr = hooks.setdefault(ev, [])
            arr[:] = [it for it in arr if not _is_ours(it)]
            arr.append(entry)
        self._write(scope, data)
```

`tools/hooks_admin/__init__.py`(替换为):
```python
"""hooks_admin — 可扩展的 agent hook 管理器(CC + Codex,核心 + adapter)。"""
from __future__ import annotations
from pathlib import Path

from . import base, state as state_mod
from .claude_code import ClaudeCodeAdapter


def build_agents(homes: dict | None = None) -> dict:
    """构造 adapter 注册表;homes 可注入(测试用临时目录),默认真实 ~。"""
    home = Path.home()
    homes = homes or {}
    cc_home = Path(homes.get("claude-code", home))
    agents: dict = {}
    agents["claude-code"] = ClaudeCodeAdapter(cc_home)
    return agents


def _command_for(adapter):
    hd = base.hook_dispatch_path()
    return lambda ev: base.command_line(hd, ev, adapter.kind)


def status(agents: dict, st: state_mod.State, scope: str = "user") -> dict:
    out: dict = {}
    for kind, ad in agents.items():
        in_cfg, found = ad.detect(scope)
        rec = st.get(kind)
        if in_cfg:
            installed, enabled, events = True, True, sorted(found.keys())
        elif rec is not None:
            installed, enabled = True, False
            events = sorted((rec.get("entries") or {}).keys())
        else:
            installed, enabled, events = False, False, []
        out[kind] = base.AgentHookStatus(
            kind=kind, display_name=ad.display_name, supported=ad.supported,
            installed=installed, enabled=enabled, events=events,
            config_path=str(ad.config_paths(scope)[0]),
        )
    return out


def install(agents: dict, st: state_mod.State, kind: str, scope: str = "user"):
    ad = agents[kind]
    written = ad.install(scope, _command_for(ad))
    st.set(kind, enabled=True, scope=scope, entries=written)
    st.save()
    return status(agents, st, scope)[kind]
```

- [ ] **Step 4: 运行,确认通过**

Run: `python tools/test_hooks_admin.py`
Expected: PASS —— 含 `TestClaudeCodeInstall`(用户 hook 保留 + 三事件装入 + 备份 + status enabled)。

- [ ] **Step 5: Commit**

```bash
git add tools/hooks_admin/claude_code.py tools/hooks_admin/__init__.py tools/test_hooks_admin.py
git commit -m "feat(hooks-admin): Claude Code adapter + status/install (preserves user hooks)"
```

---

### Task 3: 软禁用/启用(disable/enable)+ CC 全 round-trip

**Files:**
- Modify: `tools/hooks_admin/__init__.py`(加 `disable()` / `enable()`)
- Modify: `tools/test_hooks_admin.py`(加 CC disable→enable round-trip + 保用断言)

**Interfaces:**
- Produces: `__init__.disable(agents, st, kind, scope) -> AgentHookStatus`、`__init__.enable(agents, st, kind, scope) -> AgentHookStatus`。

- [ ] **Step 1: 写失败测试(install→disable→enable,全程保用)**

在 `tools/test_hooks_admin.py` 增加:
```python
class TestSoftDisableEnable(unittest.TestCase):
    def test_cc_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            cc_home = d / "cc_home"
            (cc_home / ".claude").mkdir(parents=True)
            settings = cc_home / ".claude" / "settings.json"
            settings.write_text(json.dumps({"hooks": {"PreToolUse": [
                {"matcher": "Read", "hooks": [{"type": "command", "command": "echo user-own"}]}
            ]}}), encoding="utf-8")
            agents = hooks_admin.build_agents(_fake_homes(d))
            st = state_mod.State(d / "state.json")

            hooks_admin.install(agents, st, "claude-code")
            # 手改我们的 Stop 条目(模拟用户调了 timeout),验证软禁用保留
            data = json.loads(settings.read_text(encoding="utf-8"))
            for it in data["hooks"]["Stop"]:
                if any("hook_dispatch.py" in h["command"] for h in it["hooks"]):
                    it["hooks"][0]["timeout"] = 99
            settings.write_text(json.dumps(data), encoding="utf-8")

            s = hooks_admin.disable(agents, st, "claude-code")
            assert s.installed and not s.enabled, s
            data = json.loads(settings.read_text(encoding="utf-8"))
            # 我们的条目已从配置移除,用户的仍在
            assert all("hook_dispatch.py" not in (h.get("command") or "")
                       for arr in data.get("hooks", {}).values() for it in arr for h in it["hooks"]), data
            assert any("echo user-own" in h["command"]
                       for it in data["hooks"]["PreToolUse"] for h in it["hooks"]), data

            s = hooks_admin.enable(agents, st, "claude-code")
            assert s.installed and s.enabled, s
            data = json.loads(settings.read_text(encoding="utf-8"))
            # 手改的 timeout=99 被原样恢复(软禁用保留手改)
            stop_ours = [it for it in data["hooks"]["Stop"]
                         if any("hook_dispatch.py" in h["command"] for h in it["hooks"])]
            assert stop_ours and stop_ours[0]["hooks"][0].get("timeout") == 99, stop_ours
```

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/test_hooks_admin.py`
Expected: FAIL —— `hooks_admin.disable` / `enable` 不存在。

- [ ] **Step 3: 实现 disable / enable**

在 `tools/hooks_admin/__init__.py` 增加:
```python
def disable(agents: dict, st: state_mod.State, kind: str, scope: str = "user"):
    ad = agents[kind]
    removed = ad.remove(scope)
    rec = st.get(kind)
    # 优先保留已有 state 里的 entries(含历史手改);否则用本次移除的
    entries = removed or ((rec or {}).get("entries") or {})
    st.set(kind, enabled=False, scope=scope, entries=entries)
    st.save()
    return status(agents, st, scope)[kind]


def enable(agents: dict, st: state_mod.State, kind: str, scope: str = "user"):
    ad = agents[kind]
    rec = st.get(kind)
    entries = (rec or {}).get("entries") or {}
    if entries:
        ad.restore(scope, entries)
        st.set(kind, enabled=True, scope=scope, entries=entries)
        st.save()
        return status(agents, st, scope)[kind]
    return install(agents, st, kind, scope)  # 无 state 记录 → 全新安装
```

- [ ] **Step 4: 运行,确认通过**

Run: `python tools/test_hooks_admin.py`
Expected: PASS —— `TestSoftDisableEnable` 绿(disable 移除我们的/保留用户的;enable 恢复且 timeout=99 手改保留)。

- [ ] **Step 5: Commit**

```bash
git add tools/hooks_admin/__init__.py tools/test_hooks_admin.py
git commit -m "feat(hooks-admin): soft disable/enable preserving hand-edits"
```

---

### Task 4: CodexAdapter + 注册表遍历测试

**Files:**
- Create: `tools/hooks_admin/codex.py`
- Modify: `tools/hooks_admin/__init__.py`(`build_agents` 注册 Codex)
- Modify: `tools/test_hooks_admin.py`(Codex round-trip + 注册表遍历)

**Interfaces:**
- Produces: `codex.CodexAdapter(home: Path)` 实现 `HookAgentAdapter`;写 `~/.codex/hooks.json`,检测时也读 `~/.codex/config.toml`(`tomllib`)。

- [ ] **Step 1: 写失败测试(Codex round-trip + 遍历所有 supported adapter)**

在 `tools/test_hooks_admin.py` 增加:
```python
class TestCodex(unittest.TestCase):
    def test_codex_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "cx_home" / ".codex").mkdir(parents=True)
            agents = hooks_admin.build_agents(_fake_homes(d))
            st = state_mod.State(d / "state.json")

            hooks_admin.install(agents, st, "codex")
            hj = d / "cx_home" / ".codex" / "hooks.json"
            data = json.loads(hj.read_text(encoding="utf-8"))
            for ev in base.EVENTS:
                arr = data["hooks"][ev]
                assert any("hook_dispatch.py" in hh["command"]
                           for it in arr for hh in it["hooks"]), (ev, arr)
            assert hooks_admin.status(agents, st)["codex"].enabled

            hooks_admin.disable(agents, st, "codex")
            assert not hooks_admin.status(agents, st)["codex"].enabled
            hooks_admin.enable(agents, st, "codex")
            assert hooks_admin.status(agents, st)["codex"].enabled


class TestRegistrySweep(unittest.TestCase):
    """每个 supported adapter 都应满足同一组 round-trip(新增 agent 自动纳入)。"""
    def test_all_supported_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for sub in ("cc_home/.claude", "cx_home/.codex"):
                (d / sub).mkdir(parents=True, exist_ok=True)
            agents = hooks_admin.build_agents(_fake_homes(d))
            st = state_mod.State(d / "state.json")
            for kind, ad in agents.items():
                if not ad.supported:
                    continue
                assert hooks_admin.install(agents, st, kind).enabled, kind
                assert not hooks_admin.disable(agents, st, kind).enabled, kind
                assert hooks_admin.enable(agents, st, kind).enabled, kind
```

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/test_hooks_admin.py`
Expected: FAIL —— `codex` 不存在 / `build_agents` 未注册 codex。

- [ ] **Step 3: 实现 codex.py + 注册**

`tools/hooks_admin/codex.py`:
```python
from __future__ import annotations
import json
import tomllib
from pathlib import Path
from typing import Callable

from . import base
from .state import backup

MARK = "hook_dispatch.py"


def _is_ours(item: dict) -> bool:
    return any(MARK in (h.get("command") or "") for h in item.get("hooks", []))


class CodexAdapter:
    kind = "codex"
    display_name = "Codex"
    supported = True

    def __init__(self, home: Path):
        self.home = home

    def hooks_json_path(self, scope: str) -> Path:
        return self.home / ".codex" / "hooks.json"

    def config_toml_path(self, scope: str) -> Path:
        return self.home / ".codex" / "config.toml"

    def config_paths(self, scope: str) -> list[Path]:
        return [self.hooks_json_path(scope), self.config_toml_path(scope)]

    def _read_json(self, scope: str) -> dict:
        p = self.hooks_json_path(scope)
        if p.exists():
            try:
                return json.loads(p.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                return {}
        return {}

    def _write_json(self, scope: str, data: dict) -> None:
        p = self.hooks_json_path(scope)
        p.parent.mkdir(parents=True, exist_ok=True)
        backup(p)
        p.write_text(json.dumps(data, indent=2), encoding="utf-8")

    def _toml_has_ours(self, scope: str) -> bool:
        p = self.config_toml_path(scope)
        if not p.exists():
            return False
        try:
            doc = tomllib.loads(p.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError):
            return False
        hooks = doc.get("hooks", {})
        for ev in base.EVENTS:
            for item in hooks.get(ev, []) or []:
                if _is_ours(item):
                    return True
        return False

    def detect(self, scope: str) -> tuple[bool, dict | None]:
        data = self._read_json(scope)
        hooks = data.get("hooks", {})
        found: dict = {}
        for ev in base.EVENTS:
            for item in hooks.get(ev, []):
                if _is_ours(item):
                    found[ev] = item
                    break
        # config.toml 里若也有我们的条目,纳入"已装"判断(只读检测)
        if not found and self._toml_has_ours(scope):
            return (True, None)
        return (bool(found), found or None)

    def _entry(self, event: str, command: str) -> dict:
        node = {"hooks": [{"type": "command", "command": command}]}
        if event in ("PreToolUse", "PostToolUse"):
            node = {"matcher": "*", **node}
        return node

    def install(self, scope: str, command_for: Callable[[str], str]) -> dict:
        data = self._read_json(scope)
        hooks = data.setdefault("hooks", {})
        written: dict = {}
        for ev in base.EVENTS:
            arr = hooks.setdefault(ev, [])
            arr[:] = [it for it in arr if not _is_ours(it)]
            entry = self._entry(ev, command_for(ev))
            arr.append(entry)
            written[ev] = entry
        self._write_json(scope, data)
        return written

    def remove(self, scope: str) -> dict:
        data = self._read_json(scope)
        hooks = data.get("hooks", {})
        removed: dict = {}
        for ev in base.EVENTS:
            arr = hooks.get(ev, [])
            ours = [it for it in arr if _is_ours(it)]
            if ours:
                removed[ev] = ours[0]
            arr[:] = [it for it in arr if not _is_ours(it)]
            if not arr:
                hooks.pop(ev, None)
        self._write_json(scope, data)
        return removed

    def restore(self, scope: str, entries: dict) -> None:
        data = self._read_json(scope)
        hooks = data.setdefault("hooks", {})
        for ev, entry in entries.items():
            arr = hooks.setdefault(ev, [])
            arr[:] = [it for it in arr if not _is_ours(it)]
            arr.append(entry)
        self._write_json(scope, data)
```

在 `tools/hooks_admin/__init__.py` 的 `build_agents` 里注册 Codex:
```python
from .codex import CodexAdapter
# ... 在 agents["claude-code"] = ... 之后:
    cx_home = Path(homes.get("codex", home))
    agents["codex"] = CodexAdapter(cx_home)
```

- [ ] **Step 4: 运行,确认通过**

Run: `python tools/test_hooks_admin.py`
Expected: PASS —— `TestCodex`(hooks.json round-trip)+ `TestRegistrySweep`(所有 supported adapter 同组 round-trip)绿,既有全部不回归。

- [ ] **Step 5: Commit**

```bash
git add tools/hooks_admin/codex.py tools/hooks_admin/__init__.py tools/test_hooks_admin.py
git commit -m "feat(hooks-admin): Codex adapter (hooks.json + config.toml detect) + registry sweep test"
```

---

### Task 5: CLI(argparse)+ 入口 + CLI 冒烟测试

**Files:**
- Create: `tools/hooks_admin/cli.py`
- Create: `tools/hooks_admin/__main__.py`(`python -m tools.hooks_admin` 入口)
- Modify: `tools/test_hooks_admin.py`(CLI status --json 冒烟)

**Interfaces:**
- Produces: `cli.main(argv=None) -> int`;`python -m tools.hooks_admin <status|install|enable|disable> [--agent claude-code|codex|all] [--scope user] [--json]`。

- [ ] **Step 1: 写失败测试(CLI status --json)**

在 `tools/test_hooks_admin.py` 增加:
```python
import io
import contextlib
from tools.hooks_admin import cli


class TestCli(unittest.TestCase):
    def test_status_json_smoke(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "cc_home" / ".claude").mkdir(parents=True)
            (d / "cx_home" / ".codex").mkdir(parents=True)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = cli.main([
                    "status", "--json",
                    "--cc-home", str(d / "cc_home"),
                    "--codex-home", str(d / "cx_home"),
                    "--state", str(d / "state.json"),
                ])
            assert rc == 0, rc
            out = json.loads(buf.getvalue())
            assert "claude-code" in out and "codex" in out, out
            assert out["claude-code"]["installed"] is False, out
```

- [ ] **Step 2: 运行,确认失败**

Run: `python tools/test_hooks_admin.py`
Expected: FAIL —— `cli` 不存在。

- [ ] **Step 3: 实现 cli.py + 薄入口**

`tools/hooks_admin/cli.py`:
```python
from __future__ import annotations
import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

from . import build_agents, status, install, enable, disable, base
from . import state as state_mod


def _agents_and_state(args):
    homes = {}
    if args.cc_home:
        homes["claude-code"] = args.cc_home
    if args.codex_home:
        homes["codex"] = args.codex_home
    agents = build_agents(homes)
    st_path = Path(args.state) if args.state else state_mod.default_state_path()
    return agents, state_mod.State(st_path)


def _targets(args, agents) -> list[str]:
    if args.agent in (None, "all"):
        return list(agents.keys())
    return [args.agent]


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="hooks_admin",
                                description="管理 CC/Codex 上报 hook 的安装与启用")
    p.add_argument("action", choices=["status", "install", "enable", "disable"])
    p.add_argument("--agent", choices=["claude-code", "codex", "all"], default="all")
    p.add_argument("--scope", choices=["user"], default="user")  # 本阶段仅 user
    p.add_argument("--json", action="store_true")
    p.add_argument("--cc-home", default=None, help="覆盖 CC home(测试用)")
    p.add_argument("--codex-home", default=None, help="覆盖 Codex home(测试用)")
    p.add_argument("--state", default=None, help="覆盖 state 文件路径(测试用)")
    args = p.parse_args(argv)

    agents, st = _agents_and_state(args)

    if args.action != "status":
        fn = {"install": install, "enable": enable, "disable": disable}[args.action]
        for kind in _targets(args, agents):
            fn(agents, st, kind, args.scope)

    result = {k: asdict(v) for k, v in status(agents, st, args.scope).items()}

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for k, v in result.items():
            state_str = ("未装" if not v["installed"]
                         else ("启用" if v["enabled"] else "禁用"))
            evs = ",".join(v["events"]) or "-"
            print(f"{v['display_name']:<12} {state_str:<4} events={evs}  {v['config_path']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

`tools/hooks_admin/__main__.py`(包入口,`python -m tools.hooks_admin` 运行;避免与包同名文件冲突):
```python
"""包入口:python -m tools.hooks_admin <action> ..."""
import sys
from .cli import main

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: 运行测试 + 手动冒烟**

Run: `python tools/test_hooks_admin.py`
Expected: PASS —— 含 `TestCli`,全部绿。

手动冒烟(只读、安全,从仓库根运行):
Run: `python -m tools.hooks_admin status`
Expected: 打印两行(Claude Code / Codex 的真实状态),退出 0。

- [ ] **Step 5: Commit**

```bash
git add tools/hooks_admin/cli.py tools/hooks_admin/__main__.py tools/test_hooks_admin.py
git commit -m "feat(hooks-admin): argparse CLI (status/install/enable/disable) + smoke test"
```

---

## 验收(第 1 阶段)

- `python tools/test_hooks_admin.py` 全绿:base/state、CC install+status、软禁用/启用(保留手改)、Codex round-trip、注册表遍历、CLI status --json。
- `python -m tools.hooks_admin status` 在真实机器上打印 CC/Codex 三态。
- 全程不碰用户其它 hooks(保用断言守护);改配置前 `.esp32bak` 备份;`main/` 零改动;无第三方依赖。
- **结论**:CLI 版 hooks-admin 可用,adapter 抽象就位(加新 agent = 新 adapter + 注册一行,被遍历测试自动覆盖)。

## 后续(不在本计划)

- 第 2 阶段:`tools/web/hooks_server.py`(标准库 http.server,GET /hooks + POST /hooks/{install,enable,disable})+ dev tools hooks 面板(spec §7)。
- `--scope project`(spec §10 风险 3)。
- 文档:README/CLAUDE.md hooks 安装段改指向 hooks_admin。
- 实施首步验证 spec §10 的两个开放项(CC settings.json 真实格式被识别、Codex `~/.codex/hooks.json` 被 Codex 接受)。
