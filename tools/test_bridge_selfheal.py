"""test_bridge_selfheal.py — self-healing bridge behaviour:
  - __ping__ control message identifies a live bridge
  - bridge_already_running single-instance guard (true against a live listener,
    false against a dead port)
  - serial-open watchdog: a blocking open raises TransportError within the
    timeout instead of freezing the caller forever

Run: python tools/test_bridge_selfheal.py   (or via pytest)
"""

from __future__ import annotations

import json
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import claude_buddy_bridge as cbb
except Exception as exc:  # esp_harness not importable → skip cleanly
    print(f"[SKIP] cannot import claude_buddy_bridge ({exc})")
    cbb = None  # type: ignore


def _skip() -> bool:
    if cbb is None:
        print("[SKIP] bridge import unavailable")
        return True
    return False


def test_ping_handler_identifies_bridge():
    if _skip():
        return
    # A __ping__ control message must be answered even without a device: it
    # never touches the pusher. Build a Bridge with dummy collaborators.
    reg = cbb.SessionRegistry()

    class _DummyPusher:
        def push(self, *a, **k):
            return {"ok": True}

        def cancel_pending_replies(self):
            pass

    class _DummyPub:
        paused = False

        def bump(self):
            pass

        def pause(self):
            pass

        def resume(self):
            pass

    b = cbb.Bridge(registry=reg, pusher=_DummyPusher(), publisher=_DummyPub(),
                   permission_timeout_s=1.0)
    resp = b.handle({"type": "__ping__"})
    assert resp.get("role") == "claude_buddy_bridge"
    assert "pid" in resp


class _PingServer(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            self.rfile.readline()
            self.wfile.write(
                json.dumps({"ok": True, "role": "claude_buddy_bridge",
                            "pid": 1}).encode() + b"\n")
        except OSError:
            pass


class _ThreadedServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def test_single_instance_guard_detects_live_bridge():
    if _skip():
        return
    srv = _ThreadedServer(("127.0.0.1", 0), _PingServer)
    host, port = srv.server_address
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    try:
        assert cbb.bridge_already_running(f"{host}:{port}") is True
    finally:
        srv.shutdown()
        srv.server_close()


def test_single_instance_guard_false_on_dead_port():
    if _skip():
        return
    # Port 1: nothing listens. Must return False fast (short timeout).
    assert cbb.bridge_already_running("127.0.0.1:1", timeout=0.4) is False


def test_serial_open_watchdog_bounds_a_blocking_open(monkeypatch):
    if _skip():
        return
    # Simulate a wedged port: open_persistent_session blocks far longer than the
    # watchdog timeout. The watchdog must raise TransportError promptly rather
    # than let the caller hang forever.
    def _blocking_open(*a, **k):
        time.sleep(30)  # would freeze the whole bridge without the watchdog

    monkeypatch.setattr(cbb, "open_persistent_session", _blocking_open)
    monkeypatch.setattr(cbb, "SERIAL_OPEN_TIMEOUT_S", 0.5)

    health = cbb.DeviceHealth()
    pusher = cbb.DevicePusher(port_kind="serial", port="COM_TEST",
                              dry_run=False, health=health)
    t0 = time.monotonic()
    raised = False
    try:
        pusher._open_session()
    except cbb.TransportError:
        raised = True
    elapsed = time.monotonic() - t0
    assert raised, "watchdog must raise TransportError on a blocking open"
    assert elapsed < 3.0, f"watchdog took too long ({elapsed:.1f}s)"


def test_wire_safe_strips_lone_surrogates():
    if _skip():
        return
    bad = "ok\udca5tail"          # lone low surrogate — unencodable as UTF-8
    cleaned = cbb._wire_safe(bad)
    cleaned.encode("utf-8")       # must not raise
    assert "\udca5" not in cleaned
    # Clean text (incl. CJK) round-trips untouched.
    assert cbb._wire_safe("你好 hello") == "你好 hello"


def test_snapshot_with_surrogate_serialises_without_crashing():
    if _skip():
        return
    # A mis-decoded event string carrying a lone surrogate must never crash the
    # snapshot publisher — it would loop forever and freeze the device.
    reg = cbb.SessionRegistry()
    reg.upsert("claude-code", "s1", status="waiting",
               msg="broken\udca5msg", tool="Bash", summary="cmd\udca5")
    snap = reg.snapshot_v1()      # exercises wire_size() with the bad data
    # And the push line-builder path is surrogate-safe too.
    line = cbb._wire_safe(
        'dash snapshot "' + json.dumps(snap, ensure_ascii=False) + '"')
    line.encode("utf-8")          # must not raise


def test_device_safe_maps_symbols_and_keeps_gb2312():
    if _skip():
        return
    ds = cbb._device_safe
    # GB2312 hanzi + ASCII survive unchanged.
    assert ds("中文OK 测试") == "中文OK 测试"
    # Agent-favourite symbols become ASCII (no more .notdef boxes).
    assert ds("done ✓") == "done v"
    assert ds("a → b") == "a -> b"
    assert ds("• item") == "- item"
    assert ds("★ star") == "* star"
    # Emoji and traditional-only hanzi are dropped, not boxed.
    assert "🔥" not in ds("fire🔥")
    assert ds("繁體").startswith("繁")     # 繁 is GB2312, 體 is traditional → dropped
    assert "體" not in ds("繁體")
    # Kept punctuation from the device subset stays.
    assert ds("句号。中点·") == "句号。中点·"


def test_device_safe_fullwidth_punct_to_ascii():
    """The real remaining 乱码: agents write Chinese with fullwidth punctuation
    (，。！？：；（）“”) but the baked SimHei subset has no glyph for the fullwidth
    forms, so they boxed. They must degrade to ASCII, not survive as boxes."""
    if _skip():
        return
    ds = cbb._device_safe
    assert ds("结果：（完成）！") == "结果:(完成)!"
    assert ds("问题？好的；继续") == "问题?好的;继续"
    assert ds("“引用”和‘单引’") == '"引用"和\'单引\''
    # None of the mapped fullwidth forms may survive in the output.
    for box in "！？：；（）“”‘’":
        assert box not in ds(f"x{box}y")


def test_device_safe_uses_real_font_cmap():
    """Root-cause guard: the renderable oracle is the font's actual cmap, so a
    character the baked font lacks can never slip through as a box. If the font
    is present, fullwidth ！(U+FF01) must NOT be in the cmap (that was the bug)."""
    if _skip():
        return
    if cbb._FONT_CMAP is None:
        return  # no font/fontTools in this env — heuristic fallback path
    assert 0xFF01 not in cbb._FONT_CMAP        # fullwidth ! genuinely absent
    assert ord("中") in cbb._FONT_CMAP          # common hanzi present


def test_device_safe_line_stays_valid_json():
    if _skip():
        return
    line = 'dash event "' + json.dumps(
        {"tool": "测试", "text": "done ✓ next → 🔥"}, ensure_ascii=False) + '"'
    safe = cbb._device_safe(line)
    # The JSON payload inside the wire line must still parse after substitution.
    inner = safe[safe.index('"') + 1: safe.rindex('"')]
    obj = json.loads(inner)
    # emoji dropped (its leading space remains), symbols mapped to ASCII.
    assert obj["text"].rstrip() == "done v next ->"
    assert "🔥" not in obj["text"]


if __name__ == "__main__":
    import types
    _mp = types.SimpleNamespace(setattr=lambda o, n, v: setattr(o, n, v))
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            if "monkeypatch" in fn.__code__.co_varnames[:fn.__code__.co_argcount]:
                # crude monkeypatch shim for bare-mode run
                fn(_mp)
            else:
                fn()
            print("ok", name)
    print("ALL PASS")
