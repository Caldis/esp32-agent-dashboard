"""claude_buddy_bridge.py — host-side daemon bridging Claude Code & Codex
to the ESP32 agent dashboard (protocol v1).

v1 changes over v0:
- Multi-agent snapshots (CC + Codex side-by-side in one ``agents[]``).
- ``dash config`` / ``dash time`` pushed once on connect (re-pushed on reboot).
- ``dash health`` polled every 5 s for the connection-health indicator.
- TCP transport (for ``mock_device.py``) in addition to USB-Serial.
- ``~/.claude-buddy/config.toml`` with CLI override.
- ``status`` subcommand (one-shot health JSON dump).
- ``bench`` subcommand (1000-event replay benchmark).
- Reconnect / recovery (open-retry, push buffering, reboot-detect).
- Hardened snapshot publisher (catches all exceptions, never crashes host).

Run modes:
    python claude_buddy_bridge.py serve [...]
    python claude_buddy_bridge.py send  --type pre_tool_use < event.json
    python claude_buddy_bridge.py replay sample_dual.jsonl [--dry-run]
    python claude_buddy_bridge.py status [--port-kind tcp --port ...]
    python claude_buddy_bridge.py bench  [--events 1000] [--dry-run]
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import socket
import socketserver
import statistics
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path
from typing import Any, Optional

try:
    import tomllib  # py311+
except ImportError:  # pragma: no cover
    tomllib = None  # type: ignore


# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────

DEFAULT_LISTEN_HOST = "127.0.0.1"
DEFAULT_LISTEN_PORT = 7321
DEFAULT_SERIAL_PORT = "COM9"
DEFAULT_THROTTLE_MS = 250
DEFAULT_KEEPALIVE_MS = 10_000
DEFAULT_PERMISSION_TIMEOUT_S = 60.0
DEFAULT_HEALTH_POLL_S = 5.0
DEFAULT_DEVICE_NAME = "Clawd"
DEFAULT_THEME = "noir"
DEFAULT_OWNER = os.environ.get("USER") or os.environ.get("USERNAME") or "user"

CONFIG_PATH = Path.home() / ".claude-buddy" / "config.toml"
WIRE_MAX_BYTES = 900  # leave 124 bytes headroom under CONSOLE_MAX_LINE = 1024

ESP_HARNESS_PY = r"D:\Code\esp-harness\tools\esp-harness\.venv\Scripts\python.exe"


SAMPLE_CONFIG = """\
# ~/.claude-buddy/config.toml — sample
# CLI flags override every key below.

throttle_ms = 250
keepalive_ms = 10000
permission_timeout_s = 60

device_name = "Clawd"
owner = "Felix"
theme = "noir"

port_kind = "serial"
serial_port = "COM9"
tcp_port = "127.0.0.1:9876"

listen = "127.0.0.1:7321"
health_poll_s = 5
"""


# ─────────────────────────────────────────────────────────────────────────────
# Config file
# ─────────────────────────────────────────────────────────────────────────────

def load_config_file(path: Path = CONFIG_PATH, *, create_if_missing: bool = True) -> dict:
    """Load TOML at *path*. Create a sample if missing. Never raises."""
    try:
        if not path.exists():
            if create_if_missing:
                try:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(SAMPLE_CONFIG, encoding="utf-8")
                    print(f"[bridge] wrote sample config to {path}", file=sys.stderr)
                except OSError as e:
                    print(f"[bridge] could not write sample config: {e}", file=sys.stderr)
            return {}
        if tomllib is None:
            return {}
        with path.open("rb") as f:
            return tomllib.load(f)
    except Exception as e:
        print(f"[bridge] config load failed ({path}): {e}", file=sys.stderr)
        return {}


# ─────────────────────────────────────────────────────────────────────────────
# Session registry (v1 multi-agent)
# ─────────────────────────────────────────────────────────────────────────────

_KIND_PREFIX = {"claude-code": "C", "codex": "X", "other": "o"}


@dataclass
class AgentEntry:
    t: str       # "HH:MM"
    tool: str
    summary: str

    def as_dict(self) -> dict:
        return {"t": self.t, "tool": self.tool, "summary": self.summary[:80]}


@dataclass
class AgentSession:
    kind: str                              # claude-code | codex | other
    session_id: str
    status: str = "running"                # running | waiting | idle
    cwd: str = ""
    msg: str = ""
    entries: list[AgentEntry] = field(default_factory=list)
    tokens: int = 0
    tokens_today: int = 0
    tokens_today_date: str = field(default_factory=lambda: date.today().isoformat())
    last_active_unix: int = field(default_factory=lambda: int(time.time()))
    pending_prompt: Optional[dict] = None  # {id, tool, hint, agent_kind, session_id}

    def add_entry(self, tool: str, summary: str) -> None:
        ts = time.strftime("%H:%M")
        self.entries.insert(0, AgentEntry(t=ts, tool=tool, summary=summary or tool))
        # Cap at 12; final wire-truncation handled in snapshot builder.
        if len(self.entries) > 12:
            self.entries = self.entries[:12]

    def add_tokens(self, n: int) -> None:
        if n <= 0:
            return
        today = date.today().isoformat()
        if today != self.tokens_today_date:
            self.tokens_today_date = today
            self.tokens_today = 0
        self.tokens += n
        self.tokens_today += n
        self.last_active_unix = int(time.time())

    def as_dict(self) -> dict:
        return {
            "kind": self.kind,
            "session_id": self.session_id,
            "status": self.status,
            "cwd": self.cwd,
            "msg": self.msg[:80],
            "entries": [e.as_dict() for e in self.entries],
            "tokens": self.tokens,
            "tokens_today": self.tokens_today,
            "last_active_unix": self.last_active_unix,
            "prompt": self.pending_prompt,
        }


class SessionRegistry:
    """Thread-safe in-memory store of all live agent sessions (v1 shape)."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._sessions: dict[str, AgentSession] = {}

    @staticmethod
    def _key(kind: str, session_id: str) -> str:
        return f"{kind}:{session_id}"

    def upsert(
        self,
        kind: str,
        session_id: str,
        *,
        status: Optional[str] = None,
        cwd: Optional[str] = None,
        msg: Optional[str] = None,
        tool: Optional[str] = None,
        summary: Optional[str] = None,
        tokens: int = 0,
    ) -> AgentSession:
        with self._lock:
            k = self._key(kind, session_id)
            sess = self._sessions.get(k)
            if sess is None:
                sess = AgentSession(kind=kind, session_id=session_id)
                self._sessions[k] = sess
            if status is not None:
                sess.status = status
            if cwd is not None:
                sess.cwd = cwd
            if msg is not None:
                sess.msg = msg
            if tool and summary is not None:
                sess.add_entry(tool, summary)
            if tokens:
                sess.add_tokens(tokens)
            sess.last_active_unix = int(time.time())
            return sess

    def drop(self, kind: str, session_id: str) -> None:
        with self._lock:
            self._sessions.pop(self._key(kind, session_id), None)

    def set_pending(self, kind: str, session_id: str, prompt: dict | None) -> None:
        with self._lock:
            k = self._key(kind, session_id)
            sess = self._sessions.get(k)
            if sess is None:
                sess = AgentSession(kind=kind, session_id=session_id)
                self._sessions[k] = sess
            sess.pending_prompt = prompt
            sess.status = "waiting" if prompt else "running"
            sess.last_active_unix = int(time.time())

    def known_kinds(self) -> set[str]:
        with self._lock:
            return {s.kind for s in self._sessions.values()}

    def snapshot_v1(self) -> dict:
        """Build the v1 ``agents[]`` snapshot. Falls back to flat v0 shape if
        only one session is known (still legal per PROTOCOL.md). Truncates
        entries oldest-first to stay under wire-size cap."""
        with self._lock:
            agents = [s.as_dict() for s in self._sessions.values()]

        total = len(agents)
        running = sum(1 for a in agents if a["status"] == "running")
        waiting = sum(1 for a in agents if a["status"] == "waiting")
        tokens = sum(a["tokens"] for a in agents)
        tokens_today = sum(a["tokens_today"] for a in agents)

        # Choose shape. Single agent → still emit v1 (agents[]) — the spec
        # prefers it from v1 bridges; v0 flat is reserved for backward-compat.
        snap = {
            "agents": agents,
            "totals": {
                "total": total,
                "running": running,
                "waiting": waiting,
                "tokens": tokens,
                "tokens_today": tokens_today,
            },
        }

        # Wire-size cap: shrink entries oldest-first until under cap.
        attempts = 0
        while len(json.dumps(snap, separators=(",", ":"))) > WIRE_MAX_BYTES and attempts < 64:
            attempts += 1
            # find the agent with the most entries and pop the oldest
            biggest = max(snap["agents"], key=lambda a: len(a["entries"]), default=None)
            if biggest is None or not biggest["entries"]:
                break
            biggest["entries"].pop()  # last item = oldest (insert-at-0 order)
        return snap

    def snapshot_v0_flat(self) -> dict:
        """Backwards-compat flat shape for tests/dry-run only — not used live."""
        v1 = self.snapshot_v1()
        first = v1["agents"][0] if v1["agents"] else {}
        return {
            "total": v1["totals"]["total"],
            "running": v1["totals"]["running"],
            "waiting": v1["totals"]["waiting"],
            "msg": first.get("msg", ""),
            "entries": [
                f"{_KIND_PREFIX.get(a['kind'], '?')}:{a['session_id'][:6]} {a['status']}"
                for a in v1["agents"]
            ][:8],
            "tokens": v1["totals"]["tokens"],
            "tokens_today": v1["totals"]["tokens_today"],
            "prompt": next(
                (a["prompt"] for a in v1["agents"] if a["prompt"]),
                None,
            ),
        }

    def tokens_payload_for(self, kind: str, session_id: str) -> dict:
        with self._lock:
            sess = self._sessions.get(self._key(kind, session_id))
            if sess is None:
                return {"cumulative": 0, "today": 0, "latest_sample": 0}
            return {
                "agent_kind": sess.kind,
                "session_id": sess.session_id,
                "cumulative": sess.tokens,
                "today": sess.tokens_today,
                "latest_sample": 0,
            }


# ─────────────────────────────────────────────────────────────────────────────
# Device health
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class DeviceHealth:
    last_seen_ts: float = 0.0
    connected: bool = False
    payload: dict = field(default_factory=dict)
    last_uptime_s: int = 0

    def as_dict(self) -> dict:
        return {
            "connected": self.connected,
            "last_seen_ts": self.last_seen_ts,
            "stale_s": round(time.time() - self.last_seen_ts, 1) if self.last_seen_ts else None,
            "device": self.payload,
        }


# ─────────────────────────────────────────────────────────────────────────────
# Transport: TCP (mock_device.py) and Serial (esp-harness ConsoleSession)
# ─────────────────────────────────────────────────────────────────────────────

class TransportError(Exception):
    pass


class _TCPTransport:
    """Newline-framed TCP transport. One persistent socket, line-oriented."""

    def __init__(self, addr: str) -> None:
        host, port_s = addr.split(":")
        self._host = host
        self._port = int(port_s)
        self._sock: Optional[socket.socket] = None
        self._buf = b""
        self._lock = threading.Lock()

    def open(self) -> None:
        with self._lock:
            if self._sock is not None:
                return
            try:
                self._sock = socket.create_connection((self._host, self._port), timeout=3.0)
            except OSError as e:
                raise TransportError(f"tcp connect {self._host}:{self._port} failed: {e}") from e
            self._sock.settimeout(0.5)
            self._buf = b""

    def close(self) -> None:
        with self._lock:
            if self._sock:
                try:
                    self._sock.close()
                except OSError:
                    pass
            self._sock = None
            self._buf = b""

    def is_open(self) -> bool:
        return self._sock is not None

    def write_line(self, line: str) -> None:
        with self._lock:
            if self._sock is None:
                raise TransportError("transport not open")
            try:
                self._sock.sendall((line.rstrip("\n") + "\n").encode("utf-8"))
            except OSError as e:
                self._sock = None
                raise TransportError(f"write failed: {e}") from e

    def read_lines(self, deadline: float) -> list[str]:
        """Read available lines until *deadline* (monotonic seconds)."""
        out: list[str] = []
        while time.monotonic() < deadline:
            sock = self._sock
            if sock is None:
                raise TransportError("transport not open")
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                if out:
                    return out
                continue
            except OSError as e:
                self._sock = None
                raise TransportError(f"read failed: {e}") from e
            if not chunk:
                self._sock = None
                raise TransportError("EOF from peer")
            self._buf += chunk
            while b"\n" in self._buf:
                one, self._buf = self._buf.split(b"\n", 1)
                out.append(one.decode("utf-8", errors="replace").rstrip("\r"))
            if out:
                return out
        return out


class _SerialTransport:
    """Persistent USB-Serial transport via esp-harness ConsoleSession."""

    def __init__(self, port: str) -> None:
        self._port = port
        self._sess = None
        self._lock = threading.Lock()
        # Lazy path injection
        esp_src = r"D:\Code\esp-harness\tools\esp-harness\src"
        if esp_src not in sys.path:
            sys.path.insert(0, esp_src)

    def open(self) -> None:
        with self._lock:
            if self._sess is not None:
                return
            try:
                from esp_harness.core.console_session import ConsoleSession  # type: ignore
            except Exception as e:
                raise TransportError(f"cannot import ConsoleSession: {e}") from e
            try:
                self._sess = ConsoleSession(self._port).__enter__()
            except Exception as e:
                raise TransportError(f"serial open {self._port} failed: {e}") from e

    def close(self) -> None:
        with self._lock:
            if self._sess is not None:
                try:
                    self._sess.__exit__(None, None, None)
                except Exception:
                    pass
            self._sess = None

    def is_open(self) -> bool:
        return self._sess is not None

    def write_line(self, line: str) -> None:
        with self._lock:
            if self._sess is None:
                raise TransportError("transport not open")
            try:
                self._sess._ser.write((line.rstrip("\n") + "\n").encode("utf-8"))
                self._sess._ser.flush()
            except Exception as e:
                self._sess = None
                raise TransportError(f"serial write failed: {e}") from e

    def read_lines(self, deadline: float) -> list[str]:
        sess = self._sess
        if sess is None:
            raise TransportError("transport not open")
        try:
            return [ln for ln in sess._iter_lines(deadline) if ln and ln != "\0"]
        except Exception as e:
            self._sess = None
            raise TransportError(f"serial read failed: {e}") from e


def make_transport(port_kind: str, port: str):
    if port_kind == "tcp":
        return _TCPTransport(port)
    if port_kind == "serial":
        return _SerialTransport(port)
    raise ValueError(f"unknown port-kind: {port_kind!r}")


# ─────────────────────────────────────────────────────────────────────────────
# Device pusher (v1) — owns the transport, the EVT reader and the reply parser
# ─────────────────────────────────────────────────────────────────────────────

_PERM_RE = re.compile(r"permission\s+id=(\S+)\s+decision=(\w+)")


class DevicePusher:
    """v1 transport-agnostic pusher with reconnect, buffering, EVT reader."""

    def __init__(
        self,
        *,
        port_kind: str,
        port: str,
        dry_run: bool,
        health: DeviceHealth,
        on_reconnect: Optional[callable] = None,
    ) -> None:
        self.port_kind = port_kind
        self.port = port
        self.dry_run = dry_run
        self.health = health
        self.on_reconnect = on_reconnect
        self._transport = None if dry_run else make_transport(port_kind, port)
        self._lock = threading.Lock()
        self._timings: list[float] = []
        self._permission_waiters: dict[str, queue.Queue[str]] = {}
        self._buffered_snapshot: Optional[dict] = None
        self._reader_thread: Optional[threading.Thread] = None
        self._stop_reader = threading.Event()
        # Pending payload-follows parser state
        self._await_tag: Optional[str] = None
        self._await_bytes: int = 0
        self._await_buf: list[str] = []

    # ── lifecycle ─────────────────────────────────────────────────────
    def open_with_retry(self, *, retry_total_s: float = 30.0, retry_every_s: float = 2.0) -> bool:
        if self.dry_run:
            return True
        deadline = time.monotonic() + retry_total_s
        last_err = None
        while time.monotonic() < deadline:
            try:
                self._transport.open()
                self.health.connected = True
                self._start_reader()
                return True
            except TransportError as e:
                last_err = e
                self.health.connected = False
                time.sleep(retry_every_s)
        print(f"[bridge] open failed after {retry_total_s}s: {last_err}", file=sys.stderr)
        return False

    def close(self) -> None:
        self._stop_reader.set()
        if self._transport is not None:
            self._transport.close()
        self.health.connected = False

    def _start_reader(self) -> None:
        if self._reader_thread and self._reader_thread.is_alive():
            return
        self._stop_reader.clear()
        self._reader_thread = threading.Thread(
            target=self._evt_reader_loop, daemon=True, name="evt-reader"
        )
        self._reader_thread.start()

    # ── line push ─────────────────────────────────────────────────────
    def push(self, cmd: str, payload: Optional[dict]) -> dict:
        """Push one ``dash <cmd>`` line. Returns {ok, elapsed_ms, ...}.

        Payload is wrapped in double-quotes so the device tokeniser (post G-7
        fix) preserves nested JSON quotes intact. The leading `"` triggers the
        "preserve inner quotes" mode in the device parser.
        """
        if payload is None:
            line = f"dash {cmd}"
        else:
            line = f'dash {cmd} "{json.dumps(payload, separators=(",", ":"))}"'
        if self.dry_run:
            print(f"[DRY] {line}", flush=True)
            return {"ok": True, "dry_run": True}
        started = time.monotonic()
        try:
            if not self._transport.is_open():
                # Attempt fast reconnect for in-flight push
                self._transport.open()
                if self.on_reconnect:
                    try:
                        self.on_reconnect()
                    except Exception as e:
                        print(f"[bridge] on_reconnect raised: {e}", file=sys.stderr)
                self._start_reader()
                self.health.connected = True
            self._transport.write_line(line)
            elapsed_ms = (time.monotonic() - started) * 1000
            with self._lock:
                self._timings.append(elapsed_ms)
            return {"ok": True, "elapsed_ms": elapsed_ms}
        except TransportError as e:
            self.health.connected = False
            return {"ok": False, "error": str(e)}
        except Exception as e:
            self.health.connected = False
            return {"ok": False, "error": f"unexpected: {e}"}

    def buffer_snapshot(self, snap: dict) -> None:
        with self._lock:
            self._buffered_snapshot = snap

    def take_buffered(self) -> Optional[dict]:
        with self._lock:
            snap = self._buffered_snapshot
            self._buffered_snapshot = None
            return snap

    def is_connected(self) -> bool:
        return bool(self.health.connected)

    # ── EVT reader (background) ───────────────────────────────────────
    def _evt_reader_loop(self) -> None:
        try:
            while not self._stop_reader.is_set():
                try:
                    lines = self._transport.read_lines(time.monotonic() + 1.0)
                except TransportError as e:
                    if self._stop_reader.is_set():
                        return  # clean shutdown
                    self.health.connected = False
                    print(f"[bridge] reader transport lost: {e}", file=sys.stderr)
                    self._try_reconnect()
                    continue
                for ln in lines:
                    self._process_line(ln)
        except Exception as e:  # NEVER let the reader die unhandled
            if not self._stop_reader.is_set():
                print(f"[bridge] evt reader crashed (caught): {e}", file=sys.stderr)

    def _try_reconnect(self) -> None:
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline and not self._stop_reader.is_set():
            try:
                self._transport.open()
                self.health.connected = True
                print("[bridge] reconnected", file=sys.stderr)
                if self.on_reconnect:
                    try:
                        self.on_reconnect()
                    except Exception as e:
                        print(f"[bridge] on_reconnect raised: {e}", file=sys.stderr)
                # flush buffered snapshot if present
                buf = self.take_buffered()
                if buf is not None:
                    self.push("snapshot", buf)
                return
            except TransportError:
                time.sleep(2.0)

    def _process_line(self, ln: str) -> None:
        if not ln:
            return
        # Payload-follows multi-line replies (HEALTH_BEGIN ... HEALTH_END)
        if self._await_tag:
            # Skip the framing line `<TAG>_BEGIN fmt=... bytes=...`
            if ln.startswith(f"{self._await_tag}_BEGIN"):
                return
            if ln == f"{self._await_tag}_END":
                blob = "\n".join(self._await_buf)
                self._await_buf.clear()
                tag = self._await_tag
                self._await_tag = None
                self._handle_tagged_blob(tag, blob)
                return
            self._await_buf.append(ln)
            return
        if ln.startswith("OK:"):
            body = ln[3:].strip()
            # Detect "payload follows tag=NAME"
            m = re.search(r"payload follows tag=(\w+)", body)
            if m:
                self._await_tag = m.group(1)
                self._await_buf.clear()
                return
            # Plain OK: possibly inline JSON
            self._maybe_consume_ok_inline(body)
            return
        if ln.startswith("EVT:"):
            self._handle_evt(ln[4:].strip())
            return
        # Otherwise ignore (could be ERR, log line, etc.)

    def _maybe_consume_ok_inline(self, body: str) -> None:
        # health used to come inline in early v1 firmware; tolerate.
        if not body.startswith("{"):
            return
        try:
            obj = json.loads(body)
        except json.JSONDecodeError:
            return
        if "uptime_s" in obj:
            self._record_health(obj)

    def _handle_tagged_blob(self, tag: str, blob: str) -> None:
        if tag == "HEALTH":
            try:
                obj = json.loads(blob)
            except json.JSONDecodeError as e:
                print(f"[bridge] bad HEALTH json: {e}", file=sys.stderr)
                return
            self._record_health(obj)

    def _record_health(self, obj: dict) -> None:
        prev_uptime = self.health.last_uptime_s
        new_uptime = int(obj.get("uptime_s") or 0)
        self.health.payload = obj
        self.health.last_seen_ts = time.time()
        self.health.connected = True
        self.health.last_uptime_s = new_uptime
        # Reboot detection: uptime went backwards
        if prev_uptime and new_uptime < prev_uptime and self.on_reconnect:
            print(f"[bridge] device reboot detected ({prev_uptime}s → {new_uptime}s)",
                  file=sys.stderr)
            try:
                self.on_reconnect()
            except Exception as e:
                print(f"[bridge] on_reconnect (reboot) raised: {e}", file=sys.stderr)

    def _handle_evt(self, body: str) -> None:
        m = _PERM_RE.search(body)
        if not m:
            return
        req_id, decision = m.group(1), m.group(2)
        with self._lock:
            q = self._permission_waiters.get(req_id)
        if q is not None:
            try:
                q.put_nowait(decision)
            except queue.Full:
                pass

    # ── permission round-trip ─────────────────────────────────────────
    def request_permission(self, prompt: dict, *, timeout: float) -> str | None:
        req_id = prompt.get("id") or f"req_{uuid.uuid4().hex[:8]}"
        prompt = dict(prompt)
        prompt["id"] = req_id

        if self.dry_run:
            print(f"[DRY] dash prompt {json.dumps(prompt, separators=(',', ':'))}",
                  flush=True)
            return "deny"

        q: queue.Queue[str] = queue.Queue(maxsize=1)
        with self._lock:
            self._permission_waiters[req_id] = q
        try:
            res = self.push("prompt", prompt)
            if not res.get("ok"):
                return None
            try:
                return q.get(timeout=timeout)
            except queue.Empty:
                return None
        finally:
            with self._lock:
                self._permission_waiters.pop(req_id, None)

    # ── stats ─────────────────────────────────────────────────────────
    def timing_stats(self) -> dict:
        with self._lock:
            ts = list(self._timings)
        if not ts:
            return {"count": 0}
        ts_sorted = sorted(ts)
        n = len(ts_sorted)
        return {
            "count": n,
            "median_ms": round(statistics.median(ts_sorted), 2),
            "p95_ms": round(ts_sorted[max(0, int(n * 0.95) - 1)], 2),
            "min_ms": round(ts_sorted[0], 2),
            "max_ms": round(ts_sorted[-1], 2),
        }


# ─────────────────────────────────────────────────────────────────────────────
# Snapshot publisher (throttled, keepalive, exception-safe)
# ─────────────────────────────────────────────────────────────────────────────

class SnapshotPublisher(threading.Thread):
    def __init__(
        self,
        *,
        registry: SessionRegistry,
        pusher: DevicePusher,
        throttle_ms: int,
        keepalive_ms: int,
    ) -> None:
        super().__init__(daemon=True, name="snapshot-publisher")
        self.registry = registry
        self.pusher = pusher
        self.min_interval = throttle_ms / 1000.0
        self.keepalive = keepalive_ms / 1000.0
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._last_push_ts = 0.0
        self._last_snap_json = ""
        self._push_count = 0

    @property
    def push_count(self) -> int:
        return self._push_count

    def bump(self) -> None:
        self._wake.set()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()

    def run(self) -> None:
        while not self._stop.is_set():
            try:
                self._tick()
            except Exception as e:  # NEVER crash — would kill the CC host session
                print(f"[bridge] publisher tick crashed (caught): {e}", file=sys.stderr)
                time.sleep(0.5)

    def _tick(self) -> None:
        now = time.monotonic()
        throttle_wait = max(0.0, self.min_interval - (now - self._last_push_ts))
        keepalive_wait = max(0.0, self.keepalive - (now - self._last_push_ts))
        wait = min(keepalive_wait, throttle_wait) if self._wake.is_set() else keepalive_wait
        self._wake.wait(timeout=max(0.05, wait))
        if self._stop.is_set():
            return
        now = time.monotonic()
        since = now - self._last_push_ts
        if self._wake.is_set() and since < self.min_interval:
            return
        snap = self.registry.snapshot_v1()
        snap_json = json.dumps(snap, sort_keys=True)
        changed = snap_json != self._last_snap_json
        is_keepalive = since >= self.keepalive
        if not changed and not is_keepalive:
            self._wake.clear()
            return
        res = self.pusher.push("snapshot", snap)
        if res.get("ok") or res.get("dry_run"):
            self._last_push_ts = time.monotonic()
            self._last_snap_json = snap_json
            self._push_count += 1
            self._wake.clear()
        else:
            # Buffer for retry post-reconnect; do not advance _last_push_ts so
            # we keep trying.
            self.pusher.buffer_snapshot(snap)
            print(f"[bridge] push failed: {res}", file=sys.stderr)
            self._wake.clear()
            time.sleep(0.5)


# ─────────────────────────────────────────────────────────────────────────────
# Health poller (background, exception-safe)
# ─────────────────────────────────────────────────────────────────────────────

class HealthPoller(threading.Thread):
    def __init__(self, *, pusher: DevicePusher, period_s: float) -> None:
        super().__init__(daemon=True, name="health-poller")
        self.pusher = pusher
        self.period = period_s
        self._stop = threading.Event()

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        while not self._stop.is_set():
            try:
                self.pusher.push("health", None)
            except Exception as e:
                print(f"[bridge] health poll crashed (caught): {e}", file=sys.stderr)
            self._stop.wait(self.period)


# ─────────────────────────────────────────────────────────────────────────────
# Event normalization
# ─────────────────────────────────────────────────────────────────────────────

DANGEROUS_BASH_RE = re.compile(
    r"\b(rm\s+-rf|mkfs|dd\s+if=|:(){:|:&};:|shutdown|reboot|format\s+[a-z]:)",
    re.IGNORECASE,
)


def looks_like_permission_required(event: dict) -> bool:
    if event.get("permission_required") is True:
        return True
    if event.get("tool_name") == "Bash":
        cmd = (event.get("tool_input", {}) or {}).get("command", "")
        if DANGEROUS_BASH_RE.search(cmd):
            return True
    return False


def normalize_event(raw: dict) -> dict:
    agent = raw.get("agent") or raw.get("agent_kind") or "claude-code"
    if agent not in ("claude-code", "codex", "other"):
        agent = "other"
    out: dict[str, Any] = {
        "type": raw.get("type", "raw"),
        "agent": agent,
        "session_id": (
            raw.get("session_id")
            or raw.get("transcript_path", "")
            or str(raw.get("pid") or os.getpid())
        ),
        "tool_name": raw.get("tool_name", ""),
        "tool_input": raw.get("tool_input") or {},
        "summary": raw.get("summary") or "",
        "tokens": int(raw.get("tokens") or 0),
        "permission_required": bool(raw.get("permission_required")),
        "cwd": raw.get("cwd", ""),
    }
    if not out["summary"]:
        if out["type"] == "pre_tool_use":
            ti = out["tool_input"]
            if out["tool_name"] == "Bash":
                out["summary"] = "$ " + (ti.get("command", "") or "")[:60]
            elif out["tool_name"] in ("Read", "Edit", "Write"):
                out["summary"] = f"{out['tool_name']} {ti.get('file_path', '')[-60:]}"
            else:
                out["summary"] = f"{out['tool_name']}"
        elif out["type"] == "user_prompt_submit":
            out["summary"] = "> " + (raw.get("prompt", "") or "")[:60]
        elif out["type"] == "stop":
            out["summary"] = "(stop)"
        elif out["type"] == "assistant_event":
            out["summary"] = (raw.get("text", "") or "")[:60]
    return out


# ─────────────────────────────────────────────────────────────────────────────
# Bridge (per-event handling)
# ─────────────────────────────────────────────────────────────────────────────

class Bridge:
    def __init__(
        self,
        *,
        registry: SessionRegistry,
        pusher: DevicePusher,
        publisher: SnapshotPublisher,
        permission_timeout_s: float,
    ) -> None:
        self.registry = registry
        self.pusher = pusher
        self.publisher = publisher
        self.permission_timeout_s = permission_timeout_s

    def handle(self, raw: dict) -> dict:
        try:
            return self._handle_inner(raw)
        except Exception as e:  # NEVER crash — bridge would take CC down with it
            print(f"[bridge] handle() crashed (caught): {e}", file=sys.stderr)
            return {"continue": True, "error": str(e)}

    def _handle_inner(self, raw: dict) -> dict:
        evt = normalize_event(raw)
        t = evt["type"]
        agent = evt["agent"]
        sid = evt["session_id"]

        if t == "user_prompt_submit":
            self.registry.upsert(agent, sid, status="running",
                                 cwd=evt["cwd"] or None,
                                 msg=evt["summary"])
            self.publisher.bump()
            return {"continue": True}

        if t == "pre_tool_use":
            self.registry.upsert(agent, sid, status="running",
                                 cwd=evt["cwd"] or None,
                                 msg=evt["summary"],
                                 tool=evt["tool_name"] or "tool",
                                 summary=evt["summary"])
            if looks_like_permission_required(evt):
                prompt = {
                    "id": f"req_{uuid.uuid4().hex[:8]}",
                    "tool": evt["tool_name"],
                    "hint": (evt["summary"] or evt["tool_name"])[:80],
                    "agent_kind": agent,
                    "session_id": sid,
                }
                self.registry.set_pending(agent, sid, prompt)
                self.publisher.bump()
                decision = self.pusher.request_permission(
                    prompt, timeout=self.permission_timeout_s,
                )
                self.registry.set_pending(agent, sid, None)
                self.publisher.bump()
                if decision in ("once", "allow"):
                    return {"hookSpecificOutput": {
                        "hookEventName": "PreToolUse",
                        "permissionDecision": "allow",
                    }}
                if decision == "deny":
                    return {"hookSpecificOutput": {
                        "hookEventName": "PreToolUse",
                        "permissionDecision": "deny",
                        "permissionDecisionReason": "denied via dashboard",
                    }}
                return {"continue": True,
                        "systemMessage": f"dashboard timeout (no decision)"}
            self.publisher.bump()
            return {"continue": True}

        if t == "post_tool_use":
            self.registry.upsert(agent, sid, status="running",
                                 msg=evt["summary"] or "tool ok",
                                 tokens=evt["tokens"])
            self.publisher.bump()
            return {"continue": True}

        if t == "stop":
            self.registry.upsert(agent, sid, status="idle",
                                 msg=evt["summary"] or "(idle)")
            self.publisher.bump()

            def _drop():
                try:
                    time.sleep(2.0)
                    self.registry.drop(agent, sid)
                    self.publisher.bump()
                except Exception as e:
                    print(f"[bridge] _drop crashed (caught): {e}", file=sys.stderr)
            threading.Thread(target=_drop, daemon=True).start()
            return {"continue": True}

        if t == "assistant_event":
            self.registry.upsert(agent, sid, status="running",
                                 msg=evt["summary"])
            self.publisher.bump()
            return {"continue": True}

        if t == "tokens":
            self.registry.upsert(agent, sid, tokens=evt["tokens"])
            payload = self.registry.tokens_payload_for(agent, sid)
            self.pusher.push("tokens", payload)
            return {"continue": True}

        return {"continue": True}


# ─────────────────────────────────────────────────────────────────────────────
# TCP server for hook_dispatch.py clients
# ─────────────────────────────────────────────────────────────────────────────

class _Handler(socketserver.StreamRequestHandler):
    bridge: Bridge = None  # type: ignore[assignment]
    timeout = 65.0  # >60s permission-timeout headroom

    def handle(self) -> None:
        try:
            line = self.rfile.readline()
            if not line:
                return
            try:
                raw = json.loads(line.decode("utf-8"))
            except json.JSONDecodeError as e:
                self._reply({"continue": True, "error": f"bad json: {e}"})
                return
            resp = self.bridge.handle(raw)
            self._reply(resp)
        except Exception as e:
            self._reply({"continue": True, "error": str(e)})

    def _reply(self, obj: dict) -> None:
        try:
            self.wfile.write((json.dumps(obj) + "\n").encode("utf-8"))
        except Exception:
            pass


class _ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


# ─────────────────────────────────────────────────────────────────────────────
# Settings (CLI ⊕ TOML)
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class Settings:
    throttle_ms: int
    keepalive_ms: int
    permission_timeout_s: float
    device_name: str
    owner: str
    theme: str
    port_kind: str
    port: str
    listen: str
    health_poll_s: float
    dry_run: bool
    esp_harness_py: str

    def as_redacted_dict(self) -> dict:
        return {
            "throttle_ms": self.throttle_ms,
            "keepalive_ms": self.keepalive_ms,
            "permission_timeout_s": self.permission_timeout_s,
            "device_name": self.device_name,
            "owner": self.owner,
            "theme": self.theme,
            "port_kind": self.port_kind,
            "port": self.port,
            "listen": self.listen,
            "health_poll_s": self.health_poll_s,
            "dry_run": self.dry_run,
        }


def _resolve_setting(cli_value, file_value, default):
    """CLI overrides file, file overrides default; sentinel ``None`` means
    'CLI not specified'."""
    if cli_value is not None:
        return cli_value
    if file_value is not None:
        return file_value
    return default


def build_settings(args) -> Settings:
    cfg = load_config_file()
    pk = _resolve_setting(getattr(args, "port_kind", None), cfg.get("port_kind"), "serial")
    if pk == "tcp":
        port_default = cfg.get("tcp_port") or "127.0.0.1:9876"
    else:
        port_default = cfg.get("serial_port") or DEFAULT_SERIAL_PORT
    port = _resolve_setting(getattr(args, "port", None), port_default, port_default)

    return Settings(
        throttle_ms=int(_resolve_setting(
            getattr(args, "throttle_ms", None), cfg.get("throttle_ms"), DEFAULT_THROTTLE_MS)),
        keepalive_ms=int(_resolve_setting(
            getattr(args, "keepalive_ms", None), cfg.get("keepalive_ms"), DEFAULT_KEEPALIVE_MS)),
        permission_timeout_s=float(_resolve_setting(
            getattr(args, "permission_timeout_s", None),
            cfg.get("permission_timeout_s"), DEFAULT_PERMISSION_TIMEOUT_S)),
        device_name=str(_resolve_setting(
            getattr(args, "device_name", None), cfg.get("device_name"), DEFAULT_DEVICE_NAME)),
        owner=str(_resolve_setting(
            getattr(args, "owner", None), cfg.get("owner"), DEFAULT_OWNER)),
        theme=str(_resolve_setting(
            getattr(args, "theme", None), cfg.get("theme"), DEFAULT_THEME)),
        port_kind=pk,
        port=port,
        listen=str(_resolve_setting(
            getattr(args, "listen", None), cfg.get("listen"),
            f"{DEFAULT_LISTEN_HOST}:{DEFAULT_LISTEN_PORT}")),
        health_poll_s=float(_resolve_setting(
            getattr(args, "health_poll_s", None),
            cfg.get("health_poll_s"), DEFAULT_HEALTH_POLL_S)),
        dry_run=bool(getattr(args, "dry_run", False)),
        esp_harness_py=getattr(args, "esp_harness_py", ESP_HARNESS_PY),
    )


# ─────────────────────────────────────────────────────────────────────────────
# Build helpers
# ─────────────────────────────────────────────────────────────────────────────

def _local_tz_offset_seconds() -> int:
    return -time.timezone if (time.daylight == 0) else -time.altzone


def push_initial_config_and_time(pusher: DevicePusher, settings: Settings,
                                 *, force: bool = False,
                                 state: dict | None = None) -> None:
    """Send `dash config` and `dash time` once unless the values changed
    or *force* is set (e.g. on device reboot)."""
    if state is None:
        state = {}
    cfg = {
        "device_name": settings.device_name,
        "owner": settings.owner,
        "theme": settings.theme,
    }
    time_payload = {
        "epoch_unix": int(time.time()),
        "tz_offset_seconds": _local_tz_offset_seconds(),
    }
    if force or state.get("config") != cfg:
        pusher.push("config", cfg)
        state["config"] = cfg
    if force or "time" not in state or abs(time_payload["epoch_unix"] - state.get("time_pushed_at", 0)) > 60:
        pusher.push("time", time_payload)
        state["time_pushed_at"] = time_payload["epoch_unix"]


def _build_stack(settings: Settings):
    registry = SessionRegistry()
    health = DeviceHealth()
    setup_state: dict = {}

    def _on_reconnect():
        # Re-push config + time after reconnect / reboot
        push_initial_config_and_time(pusher, settings, force=True, state=setup_state)

    pusher = DevicePusher(
        port_kind=settings.port_kind,
        port=settings.port,
        dry_run=settings.dry_run,
        health=health,
        on_reconnect=_on_reconnect,
    )
    publisher = SnapshotPublisher(
        registry=registry,
        pusher=pusher,
        throttle_ms=settings.throttle_ms,
        keepalive_ms=settings.keepalive_ms,
    )
    bridge = Bridge(
        registry=registry,
        pusher=pusher,
        publisher=publisher,
        permission_timeout_s=settings.permission_timeout_s,
    )
    return bridge, pusher, publisher, registry, health, setup_state


# ─────────────────────────────────────────────────────────────────────────────
# Subcommand: serve
# ─────────────────────────────────────────────────────────────────────────────

def cmd_serve(args) -> int:
    settings = build_settings(args)
    bridge, pusher, publisher, registry, health, setup_state = _build_stack(settings)

    if not settings.dry_run:
        ok = pusher.open_with_retry(retry_total_s=30.0, retry_every_s=2.0)
        if not ok:
            print("[bridge] could not open transport after 30s; continuing in degraded mode",
                  file=sys.stderr)
        else:
            push_initial_config_and_time(pusher, settings, state=setup_state)

    publisher.start()
    health_poller = HealthPoller(pusher=pusher, period_s=settings.health_poll_s)
    if not settings.dry_run:
        health_poller.start()

    host, port_s = settings.listen.split(":")
    port = int(port_s)
    _Handler.bridge = bridge
    server = _ThreadedTCPServer((host, port), _Handler)

    print(
        f"[bridge] v1 serving listen={host}:{port} | dry_run={settings.dry_run} | "
        f"transport={settings.port_kind}:{settings.port} | "
        f"throttle={settings.throttle_ms}ms keepalive={settings.keepalive_ms}ms "
        f"perm_timeout={settings.permission_timeout_s}s",
        flush=True,
    )

    if args.stdin:
        def _stdin_loop():
            for line in sys.stdin:
                line = line.strip()
                if not line:
                    continue
                try:
                    raw = json.loads(line)
                except json.JSONDecodeError as e:
                    print(f"[bridge] stdin bad json: {e}", file=sys.stderr)
                    continue
                resp = bridge.handle(raw)
                print(json.dumps(resp), flush=True)
        threading.Thread(target=_stdin_loop, daemon=True, name="stdin-loop").start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] stopping...", flush=True)
    finally:
        publisher.stop()
        health_poller.stop()
        pusher.close()
        stats = pusher.timing_stats()
        print(f"[bridge] push timing: {stats}", flush=True)
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# Subcommand: send (client)
# ─────────────────────────────────────────────────────────────────────────────

def cmd_send(args) -> int:
    raw = sys.stdin.read().strip()
    if not raw:
        print('{"continue": true, "error": "empty stdin"}')
        return 0
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        payload = {"type": "raw", "text": raw}
    if args.type and "type" not in payload:
        payload["type"] = args.type
    if args.agent and "agent" not in payload:
        payload["agent"] = args.agent

    host, port_s = args.listen.split(":")
    try:
        with socket.create_connection((host, int(port_s)), timeout=args.timeout) as sock:
            sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
            sock.settimeout(args.timeout)
            data = sock.makefile("r", encoding="utf-8").readline()
            print(data.strip() or '{"continue": true}')
    except (ConnectionRefusedError, socket.timeout, OSError) as e:
        print(json.dumps({"continue": True, "error": f"bridge offline: {e}"}))
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# Subcommand: replay (offline JSONL → bridge in-process)
# ─────────────────────────────────────────────────────────────────────────────

def cmd_replay(args) -> int:
    settings = build_settings(args)
    bridge, pusher, publisher, registry, health, setup_state = _build_stack(settings)

    if not settings.dry_run:
        pusher.open_with_retry(retry_total_s=10.0, retry_every_s=1.0)
        push_initial_config_and_time(pusher, settings, state=setup_state)

    publisher.start()
    try:
        with open(args.file, "r", encoding="utf-8") as f:
            for i, line in enumerate(f, 1):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    raw = json.loads(line)
                except json.JSONDecodeError as e:
                    print(f"[replay] line {i} bad json: {e}", file=sys.stderr)
                    continue
                resp = bridge.handle(raw)
                if not args.quiet:
                    print(f"[replay] {i:02d} <- {raw.get('type', '?'):20s} -> {json.dumps(resp)}")
                if args.pace_ms:
                    time.sleep(args.pace_ms / 1000.0)
        time.sleep(0.4)
    finally:
        publisher.stop()
        pusher.close()
        stats = pusher.timing_stats()
        print(f"[replay] push timing: {stats} | snapshot pushes: {publisher.push_count}")
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# Subcommand: status (one-shot health JSON)
# ─────────────────────────────────────────────────────────────────────────────

def cmd_status(args) -> int:
    settings = build_settings(args)
    health = DeviceHealth()

    if settings.dry_run:
        print(json.dumps({"ok": False, "error": "status requires a live transport"}))
        return 1

    pusher = DevicePusher(
        port_kind=settings.port_kind,
        port=settings.port,
        dry_run=False,
        health=health,
        on_reconnect=None,
    )
    try:
        if not pusher.open_with_retry(retry_total_s=args.timeout, retry_every_s=1.0):
            print(json.dumps({"ok": False, "error": "transport open failed"}))
            return 1
        res = pusher.push("health", None)
        if not res.get("ok"):
            print(json.dumps({"ok": False, "error": f"push failed: {res}"}))
            return 1
        # Wait for HEALTH_END blob via the reader.
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if health.last_seen_ts:
                break
            time.sleep(0.1)
        if not health.last_seen_ts:
            print(json.dumps({
                "ok": False,
                "error": "no HEALTH reply within timeout",
                "settings": settings.as_redacted_dict(),
            }))
            return 2
        print(json.dumps({
            "ok": True,
            "settings": settings.as_redacted_dict(),
            "health": health.as_dict(),
        }, indent=2 if args.pretty else None))
        return 0
    finally:
        pusher.close()


# ─────────────────────────────────────────────────────────────────────────────
# Subcommand: bench (1000-event replay → throttled snapshot count)
# ─────────────────────────────────────────────────────────────────────────────

def _synth_events(n: int) -> list[dict]:
    """Generate *n* synthetic events alternating CC + Codex."""
    out = []
    for i in range(n):
        is_cc = (i % 2) == 0
        agent = "claude-code" if is_cc else "codex"
        sid = "cc_bench01" if is_cc else "cx_bench02"
        tool = ["Bash", "Read", "Edit", "Grep", "Write"][i % 5]
        if i % 7 == 0:
            out.append({
                "type": "user_prompt_submit", "agent": agent, "session_id": sid,
                "prompt": f"task #{i}",
            })
        elif i % 11 == 0:
            out.append({
                "type": "stop", "agent": agent, "session_id": sid,
            })
        elif i % 3 == 0:
            out.append({
                "type": "post_tool_use", "agent": agent, "session_id": sid,
                "tool_name": tool, "tokens": 20,
                "summary": f"{tool} #{i}",
            })
        else:
            out.append({
                "type": "pre_tool_use", "agent": agent, "session_id": sid,
                "tool_name": tool, "tool_input": {"command": f"echo {i}"},
                "summary": f"{tool} #{i}",
            })
    return out


def cmd_bench(args) -> int:
    args.dry_run = True  # bench is always dry-run by design
    settings = build_settings(args)
    bridge, pusher, publisher, registry, health, setup_state = _build_stack(settings)
    publisher.start()

    events = _synth_events(args.events)
    per_event_us: list[float] = []
    pace_s = args.pace_ms / 1000.0 if args.pace_ms > 0 else 0.0
    started = time.monotonic()
    for evt in events:
        t0 = time.perf_counter()
        bridge.handle(evt)
        per_event_us.append((time.perf_counter() - t0) * 1e6)
        if pace_s:
            time.sleep(pace_s)
    # Let the publisher drain at least one throttle window
    time.sleep(max(0.3, settings.throttle_ms / 1000.0 * 1.5))
    publisher.stop()
    publisher.join(timeout=1.0)
    elapsed = time.monotonic() - started
    pushed = publisher.push_count
    p50 = statistics.median(per_event_us)
    p95 = sorted(per_event_us)[int(len(per_event_us) * 0.95) - 1] if per_event_us else 0
    expected_pushes = max(1, int(elapsed * 1000 / settings.throttle_ms))
    out = {
        "events": args.events,
        "elapsed_s": round(elapsed, 3),
        "snapshot_pushes": pushed,
        "expected_pushes_from_throttle": expected_pushes,
        "median_event_us": round(p50, 1),
        "p95_event_us": round(p95, 1),
        "events_per_s": round(args.events / elapsed, 1) if elapsed > 0 else None,
        "throttle_ms": settings.throttle_ms,
        "keepalive_ms": settings.keepalive_ms,
        "pace_ms": args.pace_ms,
    }
    print(json.dumps(out, indent=2))
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# argparse plumbing
# ─────────────────────────────────────────────────────────────────────────────

def _add_v1_flags(p):
    p.add_argument("--throttle-ms", type=int, default=None,
                   help=f"min ms between snapshot pushes (default {DEFAULT_THROTTLE_MS})")
    p.add_argument("--keepalive-ms", type=int, default=None,
                   help=f"max ms between snapshot pushes (default {DEFAULT_KEEPALIVE_MS})")
    p.add_argument("--permission-timeout-s", type=float, default=None,
                   help=f"prompt-EVT wait (default {DEFAULT_PERMISSION_TIMEOUT_S})")
    p.add_argument("--device-name", default=None,
                   help=f"name pushed via dash config (default {DEFAULT_DEVICE_NAME!r})")
    p.add_argument("--owner", default=None,
                   help="owner name pushed via dash config (default $USER)")
    p.add_argument("--theme", default=None, choices=[None, "noir", "lab", "mono"],
                   help=f"theme pushed via dash config (default {DEFAULT_THEME!r})")
    p.add_argument("--port-kind", default=None, choices=[None, "serial", "tcp"],
                   help="transport kind (default serial)")
    p.add_argument("--port", default=None,
                   help="COM port (serial) or HOST:PORT (tcp)")
    p.add_argument("--listen", default=None,
                   help=f"HOST:PORT for hook_dispatch clients (default {DEFAULT_LISTEN_HOST}:{DEFAULT_LISTEN_PORT})")
    p.add_argument("--health-poll-s", type=float, default=None,
                   help=f"dash health poll period (default {DEFAULT_HEALTH_POLL_S})")
    p.add_argument("--dry-run", action="store_true",
                   help="don't push to device, print would-be commands")
    p.add_argument("--esp-harness-py", default=ESP_HARNESS_PY,
                   help="(serial only) esp-harness venv python")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="claude_buddy_bridge",
        description="Host bridge v1: Claude Code + Codex hook events → ESP32 dashboard.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_serve = sub.add_parser("serve", help="run the long-lived daemon")
    _add_v1_flags(p_serve)
    p_serve.add_argument("--stdin", action="store_true",
                         help="also accept JSONL events on stdin")

    p_send = sub.add_parser("send", help="forward one event to a running bridge")
    p_send.add_argument("--listen", default=f"{DEFAULT_LISTEN_HOST}:{DEFAULT_LISTEN_PORT}")
    p_send.add_argument("--type", default=None,
                        help="override event type (pre_tool_use, ...)")
    p_send.add_argument("--agent", default=None,
                        help="override agent (claude-code|codex)")
    p_send.add_argument("--timeout", type=float, default=5.0)

    p_replay = sub.add_parser("replay", help="process a JSONL file in-process")
    _add_v1_flags(p_replay)
    p_replay.add_argument("file", help="JSONL file of events")
    p_replay.add_argument("--pace-ms", type=int, default=80,
                          help="sleep between events to exercise throttling")
    p_replay.add_argument("--quiet", action="store_true",
                          help="suppress per-event log lines")

    p_status = sub.add_parser("status", help="one-shot health JSON dump")
    _add_v1_flags(p_status)
    p_status.add_argument("--timeout", type=float, default=5.0)
    p_status.add_argument("--pretty", action="store_true")

    p_bench = sub.add_parser("bench", help="benchmark a synthetic event stream")
    _add_v1_flags(p_bench)
    p_bench.add_argument("--events", type=int, default=1000)
    p_bench.add_argument("--pace-ms", type=int, default=0,
                         help="sleep between events (0 = full-speed); use >0 to simulate realistic arrival")

    args = parser.parse_args(argv)
    if args.cmd == "serve":
        return cmd_serve(args)
    if args.cmd == "send":
        return cmd_send(args)
    if args.cmd == "replay":
        return cmd_replay(args)
    if args.cmd == "status":
        return cmd_status(args)
    if args.cmd == "bench":
        return cmd_bench(args)
    parser.error(f"unknown cmd {args.cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
