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
