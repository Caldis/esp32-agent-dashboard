from __future__ import annotations
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Protocol

# tools/hooks_admin/base.py → 仓库根 = parents[2]
PROJECT_ROOT = Path(__file__).resolve().parents[2]

# UserPromptSubmit is what tells the device "the user just submitted — the agent
# is now working", clearing the lingering "your turn" (AWAITING_CONTINUE) takeover
# the moment a turn starts (the bridge handles it; the device auto-exits the
# awaiting scene). Without it the screen stays on "your turn" until the next Stop.
EVENTS = ("UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop")
EVENT_SNAKE = {
    "UserPromptSubmit": "user_prompt_submit",
    "PreToolUse": "pre_tool_use",
    "PostToolUse": "post_tool_use",
    "Stop": "stop",
}


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
