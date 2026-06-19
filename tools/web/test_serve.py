"""test_serve.py — web dev-tools server + mock-device interaction symmetry.

Covers the new behaviour added for the off-device dev panel:
  - mock_device_v1 gained the `push` verb (was logged as unknown by the bridge)
  - mock_device_v1 gained an `on_prompt` callback (browser-driven decisions
    instead of auto-approve)
  - serve.py routes a browser decision/reply back to the bridge as an EVT, and
    forwards /inject events to the bridge.

These are pure host-side tests (no browser, no WASM, no real bridge): we stand
in for the bridge with a plain socket and assert the wire bytes.
"""

from __future__ import annotations

import http.client
import json
import socket
import sys
import threading
import time
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "web"))

from mock_device_v1 import MockDeviceV1, VERBS  # noqa: E402
import serve  # noqa: E402


def _wire(verb: str, payload: dict) -> str:
    """Frame a dash line exactly like DevicePusher.push (outer-quoted JSON so
    the G-7 tokeniser preserves inner quotes)."""
    return f'dash {verb} "{json.dumps(payload, separators=(",", ":"))}"'


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _wait(pred, timeout=3.0, step=0.02):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def _recv_until(sock, needle: bytes, timeout=3.0) -> bytes:
    """Accumulate from *sock* until *needle* appears or *timeout* elapses."""
    sock.settimeout(0.3)
    buf = b""
    end = time.monotonic() + timeout
    while time.monotonic() < end and needle not in buf:
        buf += _safe_recv(sock)
    return buf


# ── mock_device_v1 unit tests ────────────────────────────────────────────────

def test_push_verb_is_known():
    assert "push" in VERBS
    out = []
    MockDeviceV1(decision_delay_ms=0, auto_deny=False, verbose=False).handle_line(
        _wire("push", {"tool": "Read", "hint": "x"}), out.append)
    assert out and "banner" in out[0]
    assert not any("ERR" in o for o in out)


def test_on_prompt_suppresses_auto_decision():
    got = []
    m = MockDeviceV1(decision_delay_ms=0, auto_deny=False, verbose=False,
                     on_prompt=lambda p, s: got.append(p))
    out = []
    m.handle_line(_wire("prompt", {"id": "req_1", "tool": "Bash"}), out.append)
    time.sleep(0.05)
    assert got and got[0]["id"] == "req_1"
    assert not any("EVT" in o for o in out)        # browser must decide


def test_legacy_auto_decision_without_callback():
    out = []
    m = MockDeviceV1(decision_delay_ms=0, auto_deny=False, verbose=False)
    m.handle_line(_wire("prompt", {"id": "req_2", "tool": "Bash"}), out.append)
    assert _wait(lambda: any("decision=once" in o for o in out))


def test_snapshot_with_escaped_quotes_accepted():
    """Backslash-aware tokeniser: a snapshot value containing an escaped quote
    followed by whitespace (e.g. a tool summary `$ echo "hi" world`) must parse,
    not be rejected as malformed JSON. Regression for the state-sync freeze."""
    out = []
    m = MockDeviceV1(decision_delay_ms=0, auto_deny=False, verbose=False)
    payload = {"agents": [{"kind": "claude-code", "session_id": "s1",
                           "status": "running", "msg": '$ echo "hi" world'}],
               "totals": {"total": 1}}
    m.handle_line(_wire("snapshot", payload), out.append)
    reply = "".join(out)
    assert reply.startswith("OK"), reply
    assert "malformed" not in reply, reply


def test_legacy_auto_deny():
    out = []
    m = MockDeviceV1(decision_delay_ms=0, auto_deny=True, verbose=False)
    m.handle_line(_wire("prompt", {"id": "req_3", "tool": "Bash"}), out.append)
    assert _wait(lambda: any("decision=deny" in o for o in out))


# ── serve.py integration ─────────────────────────────────────────────────────

@pytest.fixture
def server():
    """Bring up DeviceServer + HTTP server on free ports. bridge_port points at
    a port nothing listens on by default (tests that need a fake bridge bind it
    themselves)."""
    host = "127.0.0.1"
    device_port = _free_port()
    http_port = _free_port()
    bridge_port = _free_port()

    # reset module globals so tests don't leak into each other
    with serve._clients_lock:
        serve._clients.clear()
    serve._link.detach()

    serve._Handler.bridge_addr = (host, bridge_port)
    serve._Handler.auto = "off"
    serve._Handler.inject_timeout = 3.0

    dev = serve.DeviceServer(host, device_port, auto="off")
    dev.start()
    httpd = serve._ThreadingHTTP((host, http_port), serve._Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    time.sleep(0.15)

    ctx = {"host": host, "device_port": device_port,
           "http_port": http_port, "bridge_port": bridge_port}
    try:
        yield ctx
    finally:
        httpd.shutdown()


def _http_get(ctx, path):
    c = http.client.HTTPConnection(ctx["host"], ctx["http_port"], timeout=3)
    c.request("GET", path)
    r = c.getresponse()
    body = r.read().decode("utf-8")
    c.close()
    return r.status, body


def _http_post(ctx, path, obj):
    c = http.client.HTTPConnection(ctx["host"], ctx["http_port"], timeout=5)
    data = json.dumps(obj)
    c.request("POST", path, body=data, headers={"Content-Type": "application/json"})
    r = c.getresponse()
    body = r.read().decode("utf-8")
    c.close()
    return r.status, json.loads(body or "{}")


def test_state_endpoint(server):
    status, body = _http_get(server, "/state")
    assert status == 200
    st = json.loads(body)
    assert st["auto"] == "off"
    assert st["bridge_addr"].endswith(str(server["bridge_port"]))
    assert st["device_connected"] is False     # no bridge connected yet


def test_prompt_broadcast_and_decision_roundtrip(server):
    # 1. a fake "bridge" connects to the device port
    bridge = socket.create_connection((server["host"], server["device_port"]), timeout=3)
    assert _wait(lambda: serve._link.connected)

    # 2. an SSE client subscribes
    sse = socket.create_connection((server["host"], server["http_port"]), timeout=3)
    sse.sendall(b"GET /events HTTP/1.1\r\nHost: x\r\n\r\n")
    sse.settimeout(3.0)
    _wait(lambda: serve._client_count() >= 1)

    # 3. bridge pushes a permission prompt
    bridge.sendall((_wire("prompt", {"id": "req_x", "tool": "Bash",
                                     "hint": "rm -rf"}) + "\n").encode())

    # 4. it is broadcast to the SSE client AND registered as pending
    assert b"req_x" in _recv_until(sse, b"req_x")
    assert _wait(lambda: any(p.get("id") == "req_x" for p in serve._link.pending_list()))

    # 5. browser approves via /decision → bridge gets the EVT
    status, resp = _http_post(server, "/decision", {"id": "req_x", "decision": "once"})
    assert status == 200 and resp["ok"] is True
    assert b"EVT: permission id=req_x decision=once" in \
        _recv_until(bridge, b"EVT: permission id=req_x decision=once")
    # pending cleared after answering
    assert not any(p.get("id") == "req_x" for p in serve._link.pending_list())

    sse.close(); bridge.close()


def test_reply_roundtrip(server):
    bridge = socket.create_connection((server["host"], server["device_port"]), timeout=3)
    assert _wait(lambda: serve._link.connected)
    bridge.sendall((_wire("prompt", {"id": "rpl_1", "mode": "reply",
                                     "tool": "A", "hint": "B"}) + "\n").encode())
    assert _wait(lambda: any(p.get("id") == "rpl_1" for p in serve._link.pending_list()))
    status, resp = _http_post(server, "/reply", {"id": "rpl_1", "choice": 1})
    assert status == 200 and resp["ok"] is True
    assert b"EVT: reply id=rpl_1 choice=1" in \
        _recv_until(bridge, b"EVT: reply id=rpl_1 choice=1")
    bridge.close()


def test_decision_without_bridge_returns_not_connected(server):
    status, resp = _http_post(server, "/decision", {"id": "nope", "decision": "deny"})
    assert status == 200
    assert resp["ok"] is False and resp["device_connected"] is False


def test_inject_forwards_to_bridge(server):
    # stand up a fake bridge that echoes a canned response
    received = []

    def fake_bridge():
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((server["host"], server["bridge_port"]))
        srv.listen(1)
        conn, _ = srv.accept()
        line = conn.makefile("r").readline()
        received.append(json.loads(line))
        conn.sendall(b'{"continue": true, "ok": "injected"}\n')
        conn.close(); srv.close()

    threading.Thread(target=fake_bridge, daemon=True).start()
    time.sleep(0.1)

    status, resp = _http_post(server, "/inject",
                              {"type": "post_tool_use", "tool_name": "Edit"})
    assert status == 200
    assert resp.get("ok") == "injected"
    assert _wait(lambda: received and received[0]["type"] == "post_tool_use")


def test_inject_rejects_non_event(server):
    status, resp = _http_post(server, "/inject", {"no": "type"})
    assert status == 400 and "error" in resp


def test_dash_and_hold_forward_to_bridge(server):
    """Screen test driver: /dash and /hold forward control messages to the
    bridge (type __dash__ / __pause__)."""
    received = []

    def fake_bridge():
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((server["host"], server["bridge_port"]))
        srv.listen(2)
        for _ in range(2):
            conn, _a = srv.accept()
            line = conn.makefile("r").readline()
            received.append(json.loads(line))
            conn.sendall(b'{"ok": true}\n')
            conn.close()
        srv.close()

    threading.Thread(target=fake_bridge, daemon=True).start()
    time.sleep(0.1)

    s1, r1 = _http_post(server, "/dash", {"cmd": "snapshot", "payload": {"agents": []}})
    assert s1 == 200 and r1.get("ok") is True
    s2, r2 = _http_post(server, "/hold", {"on": True})
    assert s2 == 200 and r2.get("ok") is True

    assert _wait(lambda: len(received) >= 2)
    kinds = {m["type"] for m in received}
    assert "__dash__" in kinds and "__pause__" in kinds, received
    dash_msg = next(m for m in received if m["type"] == "__dash__")
    assert dash_msg["cmd"] == "snapshot"
    pause_msg = next(m for m in received if m["type"] == "__pause__")
    assert pause_msg["on"] is True


def test_dash_rejects_missing_cmd(server):
    status, resp = _http_post(server, "/dash", {"payload": {}})
    assert status == 400 and "error" in resp


def _safe_recv(sock) -> bytes:
    try:
        return sock.recv(4096)
    except (socket.timeout, OSError):
        return b""


if __name__ == "__main__":
    # These are pytest-fixture-based integration tests; run them via pytest so
    # `python tools/web/test_serve.py` works like the other test files.
    raise SystemExit(pytest.main([__file__, "-q"]))
