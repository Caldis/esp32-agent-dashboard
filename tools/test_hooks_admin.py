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
