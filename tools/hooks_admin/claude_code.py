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
        return (bool(found), found)

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
