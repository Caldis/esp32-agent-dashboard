"""test_agent_liveness.py — pid-based session liveness (ctrl+c fix).

ctrl+c / kill / closed terminal often ends a Claude Code session WITHOUT a
SessionEnd hook (esp. on Windows). Before this feature the corpse session sat
on the device for up to 15 minutes (60 s idle-turn flip → "your turn", then the
900 s awaiting stale window) and the footer "active" count never decremented.

Covers:
  - hook_dispatch._agent_pid_from_table: ancestor walk skips intermediaries
  - claude_buddy_bridge._pid_alive: our own pid alive, exited child dead
  - SessionRegistry.set_agent_pid / sweep_dead semantics
  - Bridge.handle() end-to-end: event carries agent_pid → registry stores it

Run: python -m pytest tools/test_agent_liveness.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hook_dispatch  # noqa: E402

try:
    from claude_buddy_bridge import SessionRegistry, _pid_alive
except Exception as exc:  # esp_harness not importable in this env → skip cleanly
    print(f"[SKIP] cannot import claude_buddy_bridge ({exc})")
    SessionRegistry = None  # type: ignore
    _pid_alive = None  # type: ignore


# ── hook_dispatch: ancestor walk ─────────────────────────────────────────────

def test_agent_pid_walk_skips_interpreter_and_shell():
    """python(100) ← cmd(90) ← node(80): the walk must land on node."""
    table = {
        100: (90, "python.exe"),
        90: (80, "cmd.exe"),
        80: (1, "node.exe"),
        1: (0, "wininit.exe"),
    }
    assert hook_dispatch._agent_pid_from_table(table, 100) == 80


def test_agent_pid_walk_direct_parent_is_host():
    """python(100) ← claude(50): direct spawn without a shell."""
    table = {100: (50, "python.exe"), 50: (1, "claude.exe"), 1: (0, "init")}
    assert hook_dispatch._agent_pid_from_table(table, 100) == 50


def test_agent_pid_walk_unknown_parent_is_zero():
    """Parent missing from the table (already exited) → 0, never a guess."""
    table = {100: (90, "python.exe")}
    assert hook_dispatch._agent_pid_from_table(table, 100) == 0


def test_agent_pid_walk_self_cycle_is_zero():
    """A pid that is its own parent (pid 0/4 quirks) must not loop forever."""
    table = {100: (100, "python.exe")}
    assert hook_dispatch._agent_pid_from_table(table, 100) == 0


def test_find_agent_pid_returns_int():
    """Smoke: on the real system the walk returns an int ≥ 0 and is fast."""
    t0 = time.monotonic()
    pid = hook_dispatch._find_agent_pid()
    assert isinstance(pid, int) and pid >= 0
    assert time.monotonic() - t0 < 1.0


# ── bridge: _pid_alive ───────────────────────────────────────────────────────

def test_pid_alive_own_process():
    if _pid_alive is None:
        return
    assert _pid_alive(os.getpid()) is True


def test_pid_alive_exited_child_is_false():
    if _pid_alive is None:
        return
    p = subprocess.Popen([sys.executable, "-c", "pass"])
    p.wait(timeout=30)
    assert _pid_alive(p.pid) is False


def test_pid_alive_zero_is_unknown():
    if _pid_alive is None:
        return
    assert _pid_alive(0) is None
    assert _pid_alive(-5) is None


# ── registry: set_agent_pid / sweep_dead ─────────────────────────────────────

def _aged_session(reg, sid, *, pid, age_s=60):
    reg.upsert("claude-code", sid, status="running")
    reg.set_agent_pid("claude-code", sid, pid)
    reg._sessions[f"claude-code:{sid}"].last_active_unix = int(time.time()) - age_s


def test_sweep_dead_drops_only_dead_pids():
    if SessionRegistry is None:
        return
    reg = SessionRegistry()
    _aged_session(reg, "dead", pid=11111)
    _aged_session(reg, "alive", pid=22222)
    reg.upsert("claude-code", "nopid", status="running")  # pid 0 → untouched
    reg._sessions["claude-code:nopid"].last_active_unix = int(time.time()) - 60

    dropped = reg.sweep_dead(alive_fn=lambda pid: pid == 22222)
    assert dropped == 1
    keys = set(reg._sessions.keys())
    assert keys == {"claude-code:alive", "claude-code:nopid"}


def test_sweep_dead_respects_grace_window():
    """A dead pid inside the grace window is NOT dropped yet (its own events
    may still be in flight, e.g. right after a pid re-attach)."""
    if SessionRegistry is None:
        return
    reg = SessionRegistry()
    reg.upsert("claude-code", "fresh", status="running")
    reg.set_agent_pid("claude-code", "fresh", 33333)   # last_active = now
    assert reg.sweep_dead(alive_fn=lambda pid: False) == 0
    assert "claude-code:fresh" in reg._sessions


def test_sweep_dead_unknown_verdict_keeps_session():
    """alive_fn returning None (probe failed) must NOT drop — only an
    unambiguous dead verdict does; sweep_stale covers the rest."""
    if SessionRegistry is None:
        return
    reg = SessionRegistry()
    _aged_session(reg, "murky", pid=44444)
    assert reg.sweep_dead(alive_fn=lambda pid: None) == 0
    assert "claude-code:murky" in reg._sessions


def test_set_agent_pid_does_not_create_sessions():
    if SessionRegistry is None:
        return
    reg = SessionRegistry()
    reg.set_agent_pid("claude-code", "ghost", 55555)
    assert reg._sessions == {}


# ── end-to-end: event → registry pid ─────────────────────────────────────────

def test_bridge_event_attaches_agent_pid():
    if SessionRegistry is None:
        return
    from claude_buddy_bridge import _build_stack, Settings
    settings = Settings(throttle_ms=250, keepalive_ms=10000,
                        permission_timeout_s=60.0, device_name="x", owner="y",
                        theme="noir", port_kind="tcp", port="127.0.0.1:9999",
                        listen="127.0.0.1:7398", health_poll_s=5.0, dry_run=True)
    bridge, pusher, publisher, registry, *_ = _build_stack(settings)
    bridge.handle({"type": "user_prompt_submit", "agent": "claude-code",
                   "session_id": "S9", "prompt": "hi", "agent_pid": 4242})
    assert registry._sessions["claude-code:S9"].agent_pid == 4242
    # A later event without the field must not clobber the stored pid.
    bridge.handle({"type": "post_tool_use", "agent": "claude-code",
                   "session_id": "S9", "tool_name": "Bash"})
    assert registry._sessions["claude-code:S9"].agent_pid == 4242


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"[ OK ] {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"[FAIL] {t.__name__}: {e}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
