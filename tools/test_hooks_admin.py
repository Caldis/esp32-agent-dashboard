"""hooks_admin 测试 —— 全程用临时目录,绝不碰真实用户文件。
运行:python tools/test_hooks_admin.py"""
import io
import contextlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # 仓库根,便于 import tools.*
from tools.hooks_admin import base, state
from tools import hooks_admin
from tools.hooks_admin import state as state_mod
from tools.hooks_admin import cli


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


if __name__ == "__main__":
    unittest.main(verbosity=2)
