"""test_bridge_snapshot.py — snapshot_v1 totals stay consistent with the
agents the wire snapshot actually carries, even when wire-trimming drops some.

Regression for the "waiting=2 but only 1 waiting agent shown" bug: totals were
computed over the full session set BEFORE wire-trimming, so a dropped agent was
still counted. The device then showed counts that didn't match its own list.

Run: python tools/test_bridge_snapshot.py   (or via pytest)
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from claude_buddy_bridge import SessionRegistry, WIRE_MAX_BYTES
except Exception as exc:  # esp_harness not importable in this env → skip cleanly
    print(f"[SKIP] cannot import claude_buddy_bridge ({exc})")
    SessionRegistry = None  # type: ignore


def _wire_size(snap: dict) -> int:
    return len(json.dumps(snap, separators=(",", ":")))


def _fat_registry() -> "SessionRegistry":
    """Build enough agents (with bulky cwd/entries) that the pre-trim snapshot
    exceeds the wire cap and Step-4 dropping kicks in."""
    reg = SessionRegistry()
    longcwd = "D:\\Code\\esp32-agent-dashboard\\" + "sub\\" * 30
    for i in range(5):
        sid = f"sess{i}"
        reg.upsert("claude-code", sid, status="running", cwd=longcwd,
                   tool="Bash", summary=f"$ run task number {i} " + "x" * 40)
        for j in range(6):
            reg.upsert("claude-code", sid, tool="Edit",
                       summary=f"edit file {i}-{j} " + "y" * 30)
    # Make two of them waiting WITHOUT awaiting_kind (the droppable-but-counted
    # case that caused the bug).
    reg.upsert("claude-code", "sess1", status="waiting")
    reg.upsert("claude-code", "sess3", status="waiting")
    return reg


def test_totals_match_carried_agents_after_trim():
    snap = _fat_registry().snapshot_v1()
    assert _wire_size(snap) <= WIRE_MAX_BYTES, "snapshot should fit the wire cap"
    agents = snap["agents"]
    totals = snap["totals"]
    # totals describe exactly the agents the snapshot carries
    assert totals["total"] == len(agents)
    assert totals["running"] == sum(1 for a in agents if a["status"] == "running")
    assert totals["waiting"] == sum(1 for a in agents if a["status"] == "waiting")
    # the bug: more counted than shown
    assert totals["waiting"] <= len(agents)
    assert totals["total"] <= len(agents)


def test_small_fleet_untrimmed_is_exact():
    reg = SessionRegistry()
    reg.upsert("claude-code", "a", status="running")
    reg.upsert("codex", "b", status="waiting")
    snap = reg.snapshot_v1()
    assert snap["totals"]["total"] == 2
    assert snap["totals"]["running"] == 1
    assert snap["totals"]["waiting"] == 1
    assert len(snap["agents"]) == 2


def test_dash_passthrough_and_pause():
    """Screen-test-driver control messages: __dash__ pushes a raw dash line to
    the device; __pause__ freezes/unfreezes the snapshot publisher."""
    from claude_buddy_bridge import _build_stack, Settings
    settings = Settings(
        throttle_ms=250, keepalive_ms=10000, permission_timeout_s=60.0,
        device_name="x", owner="y", theme="noir", port_kind="tcp",
        port="127.0.0.1:9999", listen="127.0.0.1:7399", health_poll_s=5.0,
        dry_run=True)
    bridge, pusher, publisher, *_ = _build_stack(settings)
    res = bridge.handle({"type": "__dash__", "cmd": "snapshot",
                         "payload": {"agents": [], "totals": {"total": 0}}})
    assert res["ok"] is True and res["sent"] == "snapshot", res
    assert bridge.handle({"type": "__pause__", "on": True})["paused"] is True
    assert publisher.paused is True
    assert bridge.handle({"type": "__pause__", "on": False})["paused"] is False
    assert publisher.paused is False


def test_device_pusher_real_transport_constructs():
    """Guard the _port_arg regression: a non-dry DevicePusher (real transport
    path, as used on serial/COM) must construct with _port_arg + mirror set.
    The dry_run tests never touch _open_session so they missed this."""
    from claude_buddy_bridge import DevicePusher, DeviceHealth
    p = DevicePusher(port_kind="tcp", port="127.0.0.1:9999", dry_run=False,
                     health=DeviceHealth(), mirror="127.0.0.1:9876")
    assert p._port_arg == "127.0.0.1:9999"
    assert p._mirror_addr == "127.0.0.1:9876"
    assert p._mirror_sock is None


def test_idle_turn_sweep_flips_silent_running_to_your_turn():
    """ESC/stall coverage: a 'running' session silent past the timeout flips to
    awaiting continue ('your turn'); a tool-in-flight one is exempt."""
    import time as _t
    reg = SessionRegistry()
    reg.upsert("claude-code", "idle1", status="running")
    reg.upsert("claude-code", "busy1", status="running")
    reg.set_tool_in_flight("claude-code", "busy1", True)
    # Force both to look old.
    for k in ("claude-code:idle1", "claude-code:busy1"):
        reg._sessions[k].last_active_unix = int(_t.time()) - 999
    flipped = reg.sweep_idle_turns(timeout_s=60)
    assert flipped == 1, flipped
    snap = {a["session_id"]: a for a in reg.snapshot_v1()["agents"]}
    assert snap["idle1"]["status"] == "waiting"
    assert snap["idle1"].get("awaiting_kind") == "continue"
    assert snap["busy1"]["status"] == "running"        # tool in flight → exempt
    assert "awaiting_kind" not in snap["busy1"]


def test_idle_turn_sweep_leaves_fresh_running_alone():
    reg = SessionRegistry()
    reg.upsert("claude-code", "fresh", status="running")
    assert reg.sweep_idle_turns(timeout_s=60) == 0
    assert reg.snapshot_v1()["agents"][0]["status"] == "running"


def test_multiple_waiting_agents_still_fit_wire_cap():
    """Two waiting agents (each with awaiting content) must still fit under the
    wire cap — else the device rejects the oversized line and freezes. Regresses
    the overnight 'line too long' freeze (phantom + real waiting agent)."""
    reg = SessionRegistry()
    for sid in ("aaaa1111", "bbbb2222", "cccc3333"):
        reg.upsert("claude-code", sid, status="waiting",
                   cwd="D:\\Code\\esp32-agent-dashboard", msg="> a fairly long user prompt here")
        reg.set_awaiting("claude-code", sid, kind="continue",
                         context=["finished its turn"],
                         summary="x" * 200, options=["opt one", "opt two", "opt three", "opt four"])
    snap = reg.snapshot_v1()
    size = len(json.dumps(snap, separators=(",", ":")))
    assert size <= WIRE_MAX_BYTES, f"snapshot {size} > cap {WIRE_MAX_BYTES}"
    assert len(snap["agents"]) >= 1


def test_sweep_stale_drops_dead_keeps_fresh_and_recent_awaiting():
    """Dead sessions get reaped; an awaiting 'your turn' is kept longer but
    eventually dropped (CC that ended without SessionEnd would else linger)."""
    import time as _t
    reg = SessionRegistry()
    reg.upsert("claude-code", "fresh", status="running")
    reg.upsert("claude-code", "deadrun", status="running")
    reg.upsert("claude-code", "yourturn", status="waiting")
    reg.set_awaiting("claude-code", "yourturn", kind="continue")
    now = int(_t.time())
    reg._sessions["claude-code:deadrun"].last_active_unix = now - 400    # >300, no awaiting
    reg._sessions["claude-code:yourturn"].last_active_unix = now - 400   # >300 but awaiting → keep
    assert reg.sweep_stale() == 1
    ids = {a["session_id"] for a in reg.snapshot_v1()["agents"]}
    assert ids == {"fresh", "yourturn"}, ids
    reg._sessions["claude-code:yourturn"].last_active_unix = now - 1000  # >900 → drop
    assert reg.sweep_stale() == 1
    assert {a["session_id"] for a in reg.snapshot_v1()["agents"]} == {"fresh"}


def _dry_settings(**over):
    from claude_buddy_bridge import Settings
    base = dict(throttle_ms=250, keepalive_ms=10000, permission_timeout_s=60.0,
                device_name="x", owner="y", theme="noir", port_kind="tcp",
                port="127.0.0.1:9999", listen="127.0.0.1:7399", health_poll_s=5.0,
                dry_run=True)
    base.update(over)
    return Settings(**base)


def test_full_lifecycle_state_transitions():
    """End-to-end state machine: every transition the device shows must be
    correct. This is the regression net so we stop relying on the user to spot
    'wrong state' — if any of these breaks, CI catches it."""
    from claude_buddy_bridge import _build_stack
    bridge, _p, _pub, reg, *_ = _build_stack(_dry_settings())

    def state(sid):
        for a in reg.snapshot_v1()["agents"]:
            if a["session_id"] == sid:
                return (a["status"], a.get("awaiting_kind"))
        return None

    A = "life"
    def h(**k):
        bridge.handle({**k, "agent": "claude-code", "session_id": A})

    h(type="session_start", source="startup")
    assert state(A) == ("waiting", "continue"), state(A)          # appears, ball in user's court
    h(type="user_prompt_submit", prompt="x")
    assert state(A) == ("running", None), state(A)                # thinking
    h(type="pre_tool_use", tool_name="Bash", tool_input={"command": "ls"})
    assert state(A) == ("running", None), state(A)
    h(type="post_tool_use", tool_name="Bash", summary="ok")
    assert state(A) == ("running", None), state(A)
    h(type="stop", last_assistant_text="done, your move.")
    assert state(A) == ("waiting", "continue"), state(A)          # your turn
    h(type="user_prompt_submit", prompt="again")
    assert state(A) == ("running", None), state(A)                # ★ ups clears your turn → thinking
    h(type="stop", last_assistant_text="pick one:\n1. a\n2. b\n3. c")
    assert state(A) == ("waiting", "pick"), state(A)
    h(type="stop", last_assistant_text="which environment?")
    assert state(A) == ("waiting", "type"), state(A)
    h(type="stop", last_assistant_text="Could you clarify which file?")
    assert state(A) == ("waiting", "clarify"), state(A)
    h(type="session_end")
    assert state(A) is None, state(A)                             # gone → idle


def test_sessionless_stop_creates_no_phantom():
    """A Stop with no session_id must NOT create a pid-phantom that sticks on
    'your turn' (regression for the recurring stuck-zzz/your-turn)."""
    from claude_buddy_bridge import _build_stack
    bridge, _p, _pub, reg, *_ = _build_stack(_dry_settings())
    bridge.handle({"type": "stop", "agent": "claude-code", "last_assistant_text": "x"})
    agents = reg.snapshot_v1()["agents"]
    assert agents == [], agents
    assert all(not a["session_id"].isdigit() for a in agents)


def test_observe_mode_does_not_gate_permissions():
    """Default (observe): a dangerous command is NOT gated by the dashboard —
    returns plain continue (Claude Code's own prompt handles approval), so the
    agent never stalls waiting on a device button."""
    from claude_buddy_bridge import _build_stack
    bridge, *_ = _build_stack(_dry_settings(gate_permissions=False))
    res = bridge.handle({"type": "pre_tool_use", "agent": "claude-code",
                         "session_id": "S", "tool_name": "Bash",
                         "tool_input": {"command": "rm -rf /tmp/x"}})
    assert res == {"continue": True}, res
    assert "hookSpecificOutput" not in res


def test_gate_mode_gates_permissions():
    """gate_permissions=True restores the approve/deny interaction: a dangerous
    command goes through the permission round-trip (dry-run → deny)."""
    from claude_buddy_bridge import _build_stack
    bridge, *_ = _build_stack(_dry_settings(gate_permissions=True))
    res = bridge.handle({"type": "pre_tool_use", "agent": "claude-code",
                         "session_id": "S", "tool_name": "Bash",
                         "tool_input": {"command": "rm -rf /tmp/x"}})
    assert res.get("hookSpecificOutput", {}).get("permissionDecision") == "deny", res


def test_idle_sweep_skips_active_transcript():
    """A long-thinking agent (no tool calls for >timeout) must NOT flip to your
    turn while its transcript is still being written; it flips only once the
    transcript goes static (real idle/ESC). Regression for "your turn shown
    mid-think"."""
    import tempfile, os as _os, time as _t
    reg = SessionRegistry()
    reg.upsert("claude-code", "thinker", status="running")
    reg._sessions["claude-code:thinker"].last_active_unix = int(_t.time()) - 999
    f = tempfile.NamedTemporaryFile(delete=False, suffix=".jsonl")
    f.write(b"x"); f.close()
    try:
        reg.set_transcript_path("claude-code", "thinker", f.name)
        assert reg.sweep_idle_turns(timeout_s=60) == 0      # fresh transcript → thinking
        old = int(_t.time()) - 999
        _os.utime(f.name, (old, old))
        assert reg.sweep_idle_turns(timeout_s=60) == 1      # static transcript → flip
    finally:
        _os.unlink(f.name)


def test_idle_sweep_disabled_when_timeout_zero():
    import time as _t
    reg = SessionRegistry()
    reg.upsert("claude-code", "x", status="running")
    reg._sessions["claude-code:x"].last_active_unix = int(_t.time()) - 999
    assert reg.sweep_idle_turns(timeout_s=0) == 0           # disabled


def test_session_end_drops_session():
    from claude_buddy_bridge import _build_stack, Settings
    settings = Settings(throttle_ms=250, keepalive_ms=10000, permission_timeout_s=60.0,
                        device_name="x", owner="y", theme="noir", port_kind="tcp",
                        port="127.0.0.1:9999", listen="127.0.0.1:7399", health_poll_s=5.0,
                        dry_run=True)
    bridge, pusher, publisher, registry, *_ = _build_stack(settings)
    bridge.handle({"type": "user_prompt_submit", "agent": "claude-code",
                   "session_id": "S1", "prompt": "hi"})
    assert any(a["session_id"] == "S1" for a in registry.snapshot_v1()["agents"])
    bridge.handle({"type": "session_end", "agent": "claude-code", "session_id": "S1"})
    assert not any(a["session_id"] == "S1" for a in registry.snapshot_v1()["agents"])


def main() -> int:
    if SessionRegistry is None:
        return 0  # skipped (no esp_harness) — not a failure
    tests = [test_totals_match_carried_agents_after_trim,
             test_small_fleet_untrimmed_is_exact,
             test_dash_passthrough_and_pause,
             test_device_pusher_real_transport_constructs,
             test_idle_turn_sweep_flips_silent_running_to_your_turn,
             test_idle_turn_sweep_leaves_fresh_running_alone,
             test_idle_sweep_skips_active_transcript,
             test_idle_sweep_disabled_when_timeout_zero,
             test_session_end_drops_session,
             test_multiple_waiting_agents_still_fit_wire_cap,
             test_sweep_stale_drops_dead_keeps_fresh_and_recent_awaiting,
             test_observe_mode_does_not_gate_permissions,
             test_gate_mode_gates_permissions,
             test_full_lifecycle_state_transitions,
             test_sessionless_stop_creates_no_phantom]
    failures = 0
    for t in tests:
        try:
            t()
        except Exception as exc:
            failures += 1
            print(f"[FAIL] {t.__name__}: {exc}")
        else:
            print(f"[PASS] {t.__name__}")
    print(f"{'all ' + str(len(tests)) + ' passed' if not failures else str(failures) + ' failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
