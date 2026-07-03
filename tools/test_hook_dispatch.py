"""Regression tests for hook_dispatch logic:
  - _read_turn_output_tokens: pull this turn's output tokens from a transcript
  - _forward_allowed: targeted-debug session filtering
  - _forward: offline / wedged / ok classification (self-healing v3)
  - circuit breaker, autostart cooldown, transition-aware notes
Run: python -m pytest tools/test_hook_dispatch.py
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hook_dispatch  # noqa: E402


def _write_jsonl(rows):
    f = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False, encoding="utf-8")
    for r in rows:
        f.write(json.dumps(r) + "\n")
    f.close()
    return f.name


# ── _read_turn_output_tokens ─────────────────────────────────────────────────

def test_turn_tokens_sums_assistant_output():
    """Sum output_tokens of assistant msgs in the latest turn (CC's usage is
    nested under message.usage)."""
    p = _write_jsonl([
        {"type": "user", "message": {"role": "user"}},
        {"type": "assistant", "message": {"role": "assistant", "usage": {"output_tokens": 100}}},
        {"type": "assistant", "message": {"role": "assistant", "usage": {"output_tokens": 50}}},
    ])
    try:
        assert hook_dispatch._read_turn_output_tokens(p) == 150
    finally:
        os.unlink(p)


def test_turn_tokens_stops_at_user_boundary():
    """Tokens from a PREVIOUS turn (before the last user msg) must not count."""
    p = _write_jsonl([
        {"type": "assistant", "message": {"usage": {"output_tokens": 999}}},  # prev turn
        {"type": "user", "message": {}},
        {"type": "assistant", "message": {"usage": {"output_tokens": 42}}},
    ])
    try:
        assert hook_dispatch._read_turn_output_tokens(p) == 42
    finally:
        os.unlink(p)


def test_turn_tokens_missing_file_is_zero():
    assert hook_dispatch._read_turn_output_tokens("/no/such/file.jsonl") == 0
    assert hook_dispatch._read_turn_output_tokens("") == 0


def test_turn_tokens_skips_tool_result_user_records():
    """A `user` record carrying a tool_result is mid-turn tool output, NOT a
    turn boundary. Tokens across the tool_result must still be summed (this was
    the systematic under-count bug)."""
    p = _write_jsonl([
        {"type": "user", "message": {"content": "real prompt"}},   # turn start
        {"type": "assistant", "message": {"usage": {"output_tokens": 30}}},
        # CC delivers tool output as a user-role record — NOT a boundary:
        {"type": "user", "message": {"content": [
            {"type": "tool_result", "content": "ok"}]}},
        {"type": "assistant", "message": {"usage": {"output_tokens": 70}}},
    ])
    try:
        assert hook_dispatch._read_turn_output_tokens(p) == 100
    finally:
        os.unlink(p)


# ── _forward_allowed (targeted debug) ────────────────────────────────────────

def _with_target(cfg):
    """Point hook_dispatch at a temp target file holding cfg; returns (path, restore)."""
    tf = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
    if cfg is not None:
        tf.write(json.dumps(cfg))
    tf.close()
    orig = hook_dispatch._TARGET_FILE
    hook_dispatch._TARGET_FILE = tf.name
    return tf.name, orig


def test_forward_allowed_no_file_allows_all():
    orig = hook_dispatch._TARGET_FILE
    hook_dispatch._TARGET_FILE = "/no/such/target.json"
    try:
        assert hook_dispatch._forward_allowed({"session_id": "anything"})
    finally:
        hook_dispatch._TARGET_FILE = orig


def test_forward_allowed_exclude():
    path, orig = _with_target({"exclude": ["fb0f"]})
    try:
        assert not hook_dispatch._forward_allowed({"session_id": "fb0f6612-xyz"})  # excluded
        assert hook_dispatch._forward_allowed({"session_id": "other-sess"})        # allowed
    finally:
        hook_dispatch._TARGET_FILE = orig
        os.unlink(path)


def test_forward_allowed_only():
    path, orig = _with_target({"only": ["test-"]})
    try:
        assert hook_dispatch._forward_allowed({"session_id": "test-123"})          # in allowlist
        assert not hook_dispatch._forward_allowed({"session_id": "fb0f6612"})      # not listed
    finally:
        hook_dispatch._TARGET_FILE = orig
        os.unlink(path)


# ── _forward: offline / wedged / ok classification ───────────────────────────

def _fake_bridge(reply: bytes | None):
    """Start a one-shot loopback listener. reply=None → accept but never answer
    (simulates a wedged daemon). Returns (host, port, stop)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    host, port = srv.getsockname()

    def _serve():
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        with conn:
            try:
                conn.recv(65536)
                if reply is not None:
                    conn.sendall(reply)
                else:
                    time.sleep(2.0)   # hold the socket open, never reply
            except OSError:
                pass

    th = threading.Thread(target=_serve, daemon=True)
    th.start()

    def stop():
        try:
            srv.close()
        except OSError:
            pass

    return host, port, stop


def _point_at(host, port):
    o = (hook_dispatch.DEFAULT_HOST, hook_dispatch.DEFAULT_PORT)
    hook_dispatch.DEFAULT_HOST, hook_dispatch.DEFAULT_PORT = host, port
    return o


def test_forward_ok_with_live_bridge():
    host, port, stop = _fake_bridge(b'{"continue": true, "ok": 1}\n')
    orig = _point_at(host, port)
    try:
        outcome, line = hook_dispatch._forward({"type": "stop"}, read_timeout=1.0)
        assert outcome == "ok"
        assert json.loads(line)["ok"] == 1
    finally:
        hook_dispatch.DEFAULT_HOST, hook_dispatch.DEFAULT_PORT = orig
        stop()


def test_forward_offline_when_nothing_listens():
    # A closed loopback port: refused OR SYN-dropped (firewall) — both 'offline',
    # never 'wedged'. Short connect timeout keeps this fast.
    orig = _point_at("127.0.0.1", 1)   # port 1: nothing listens
    old_ct = hook_dispatch.CONNECT_TIMEOUT
    hook_dispatch.CONNECT_TIMEOUT = 0.4
    try:
        outcome, _ = hook_dispatch._forward({"type": "stop"}, read_timeout=1.0)
        assert outcome == "offline"
    finally:
        hook_dispatch.DEFAULT_HOST, hook_dispatch.DEFAULT_PORT = orig
        hook_dispatch.CONNECT_TIMEOUT = old_ct


def test_forward_wedged_when_no_reply():
    host, port, stop = _fake_bridge(None)   # accepts, never answers
    orig = _point_at(host, port)
    try:
        outcome, _ = hook_dispatch._forward({"type": "stop"}, read_timeout=0.4)
        assert outcome == "wedged"
    finally:
        hook_dispatch.DEFAULT_HOST, hook_dispatch.DEFAULT_PORT = orig
        stop()


# ── circuit breaker (guards ONLY the wedged case) ────────────────────────────

def test_cb_trips_after_threshold_then_open():
    st = {}
    for _ in range(hook_dispatch.CB_THRESHOLD):
        assert not hook_dispatch._cb_is_open(st)
        hook_dispatch._cb_record_timeout(st)
    assert hook_dispatch._cb_is_open(st)      # opened after N wedges
    hook_dispatch._cb_clear(st)
    assert not hook_dispatch._cb_is_open(st)


# ── autostart cooldown (no spawn stampede) ───────────────────────────────────

def test_maybe_autostart_respects_cooldown(monkeypatch):
    calls = []
    monkeypatch.setattr(hook_dispatch, "_spawn_bridge", lambda: calls.append(1) or True)
    monkeypatch.setattr(hook_dispatch, "AUTOSTART", True)
    st = {}
    assert hook_dispatch._maybe_autostart(st) == "started"
    assert len(calls) == 1
    # Immediately again → cooldown, no second spawn.
    assert hook_dispatch._maybe_autostart(st) == "cooldown"
    assert len(calls) == 1


def test_maybe_autostart_disabled(monkeypatch):
    monkeypatch.setattr(hook_dispatch, "AUTOSTART", False)
    calls = []
    monkeypatch.setattr(hook_dispatch, "_spawn_bridge", lambda: calls.append(1) or True)
    assert hook_dispatch._maybe_autostart({}) == "disabled"
    assert not calls


# ── transition-aware notes (no per-event spam) ───────────────────────────────

def test_note_speaks_on_transition_then_throttles():
    st = {"conn": "online", "msg_ts": time.time()}
    # online → offline is a transition: speak.
    assert hook_dispatch._note(st, "offline", "down") == "down"
    # still offline, within NOTE_INTERVAL: stay silent.
    assert hook_dispatch._note(st, "offline", "down") == ""


def test_note_silent_when_no_message():
    st = {"conn": "online"}
    assert hook_dispatch._note(st, "online", "") == ""


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except TypeError:
                continue   # skip fixture-requiring tests in bare mode
            print("ok", name)
    print("ALL PASS")
