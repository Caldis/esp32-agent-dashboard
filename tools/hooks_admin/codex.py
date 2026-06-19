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
        # [0] hooks.json — 可写路径(install/remove/restore 操作此文件)
        # [1] config.toml — 只读 detect 用途,消费方勿当可写路径
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
            for item in hooks.get(ev, []):
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
            return (True, {})
        return (bool(found), found)

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
