"""test_bridge_reconnect_sync.py — instant disconnect detection + full
resync on reconnect:

  - SessionHandle.on_close fires when the transport dies underneath the
    reader (USB yank), NOT on a deliberate close()
  - DevicePusher reacts to session loss by marking unhealthy and kicking
    the reconnect loop immediately
  - SnapshotPublisher.force_push makes the next tick push the current
    snapshot even when nothing changed (reconnect/reboot resync)

Run: python tools/test_bridge_reconnect_sync.py   (or via pytest)
"""

from __future__ import annotations

import sys
import time
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import claude_buddy_bridge as cbb
    from esp_harness import client as eh_client
except Exception as exc:  # esp_harness not importable → skip cleanly
    print(f"[SKIP] cannot import bridge/esp_harness ({exc})")
    cbb = None  # type: ignore
    eh_client = None  # type: ignore


def _skip() -> bool:
    if cbb is None:
        print("[SKIP] bridge import unavailable")
        return True
    return False


class _DyingTransport:
    """read_chunk raises after `alive_reads` calls — simulates USB yank."""

    def __init__(self, alive_reads: int = 2) -> None:
        self.alive_reads = alive_reads
        self.closed = False

    def open(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True

    def read_chunk(self, timeout: float) -> bytes:
        if self.closed:
            raise eh_client.TransportError("closed")
        if self.alive_reads > 0:
            self.alive_reads -= 1
            time.sleep(0.01)
            return b""
        raise eh_client.TransportError("port vanished (simulated unplug)")

    def write_line(self, line: str) -> None:
        if self.closed:
            raise eh_client.TransportError("closed")


def test_on_close_fires_on_transport_loss():
    if _skip():
        return
    sess = eh_client.SessionHandle(_DyingTransport(alive_reads=2))
    lost = threading.Event()
    sess.on_close(lambda: lost.set())
    sess.open()
    assert lost.wait(timeout=3.0), "on_close did not fire on transport loss"
    assert not sess.is_open


def test_on_close_silent_on_deliberate_close():
    if _skip():
        return
    sess = eh_client.SessionHandle(_DyingTransport(alive_reads=10_000))
    lost = threading.Event()
    sess.on_close(lambda: lost.set())
    sess.open()
    time.sleep(0.05)
    sess.close()
    assert not lost.wait(timeout=0.5), "on_close fired on deliberate close()"


def test_session_lost_kicks_reconnect():
    if _skip():
        return
    health = cbb.DeviceHealth()
    pusher = cbb.DevicePusher(
        port_kind="serial", port="COM_NONEXISTENT", dry_run=True,
        health=health, on_reconnect=None,
    )
    kicked = []
    pusher._schedule_reconnect = lambda: kicked.append(1)  # type: ignore
    health.connected = True
    pusher._on_session_lost()
    assert kicked, "_on_session_lost did not schedule a reconnect"
    assert health.connected is False


class _RecordingPusher:
    def __init__(self) -> None:
        self.pushed: list[str] = []

    def push(self, cmd, payload):
        self.pushed.append(cmd)
        return {"ok": True}

    def buffer_snapshot(self, snap):
        pass


def test_force_push_resends_unchanged_snapshot():
    if _skip():
        return
    reg = cbb.SessionRegistry()
    rec = _RecordingPusher()
    pub = cbb.SnapshotPublisher(
        registry=reg, pusher=rec, throttle_ms=1, keepalive_ms=3_600_000,
    )
    # First tick: initial snapshot goes out (last_snap_json starts empty).
    pub._wake.set()
    pub._tick()
    assert rec.pushed == ["snapshot"]

    # Nothing changed + keepalive far away → tick must NOT push again.
    pub._wake.set()
    time.sleep(0.01)
    pub._tick()
    assert rec.pushed == ["snapshot"], "tick pushed without change/keepalive"

    # force_push (reconnect path) → next tick pushes even though the
    # registry content is identical.
    pub.force_push()
    time.sleep(0.01)
    pub._tick()
    assert rec.pushed == ["snapshot", "snapshot"], "force_push did not resync"


def main() -> int:
    tests = [
        test_on_close_fires_on_transport_loss,
        test_on_close_silent_on_deliberate_close,
        test_session_lost_kicks_reconnect,
        test_force_push_resends_unchanged_snapshot,
    ]
    failures = 0
    for t in tests:
        try:
            t()
        except Exception as exc:  # pragma: no cover
            failures += 1
            print(f"[FAIL] {t.__name__}: {exc}")
        else:
            print(f"[PASS] {t.__name__}")
    if failures:
        print(f"{failures}/{len(tests)} failed")
        return 1
    print(f"all {len(tests)} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
