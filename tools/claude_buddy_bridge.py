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


# v0.2.0 of the esp-harness framework ships the public persistent-
# session API (gaps G-1, G-3) and the PayloadFollowsReader helper
# (gap G-H1) that this bridge previously rolled by hand. The lazy
# path injection keeps working out-of-the-box when the user installs
# esp-harness from source into a sibling venv. If the import fails
# we fall back to a clear error — the legacy bespoke transport is
# GONE in this revision.
_ESP_HARNESS_SRC = r"D:\Code\esp-harness\tools\esp-harness\src"
if _ESP_HARNESS_SRC not in sys.path:
    sys.path.insert(0, _ESP_HARNESS_SRC)

from esp_harness.client import (  # noqa: E402
    ReplyEvent,
    SessionHandle,
    TransportError,
    open_persistent_session,
)


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
    # v2.3.0: AWAITING state for the device's takeover scene. When the
    # agent is blocking on user input, awaiting_kind is one of:
    #   "continue" - end-of-turn, generic "your turn"
    #   "approve"  - PreToolUse(permission_required); decide via buttons
    #   "pick"     - assistant offered numbered options in last message
    #   "type"     - assistant asked an open-ended question
    #   "clarify"  - assistant flagged ambiguity / asked to clarify
    # Cleared (set None) on next UserPromptSubmit or session drop.
    awaiting_kind: Optional[str] = None
    awaiting_context: list[str] = field(default_factory=list)  # 1-3 lines for AWAITING ctx
    awaiting_since_unix: int = 0
    last_assistant_text: str = ""          # buffered for the classifier on Stop

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
        d = {
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
        # v2.3.0: only emit awaiting_* fields when the session is actually
        # waiting — keeps the snapshot wire-size lean for the common case.
        if self.awaiting_kind is not None:
            d["awaiting_kind"]    = self.awaiting_kind
            d["awaiting_context"] = self.awaiting_context[:3]
            d["awaiting_since"]   = self.awaiting_since_unix
        return d


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

    def set_awaiting(self, agent_kind: str, session_id: str, *,
                      kind: str | None = None,
                      context: list[str] | None = None) -> None:
        """Enter AWAITING for the device's takeover scene.

        ``agent_kind`` is the AGENT (claude-code / codex / other);
        ``kind`` is the AWAITING KIND
        ('continue' / 'approve' / 'pick' / 'type' / 'clarify').
        Renamed positional from `kind` to `agent_kind` so callers can
        pass `kind=` as kwarg without name collision.
        """
        with self._lock:
            k = self._key(agent_kind, session_id)
            sess = self._sessions.get(k)
            if sess is None:
                sess = AgentSession(kind=agent_kind, session_id=session_id)
                self._sessions[k] = sess
            sess.awaiting_kind = kind
            sess.awaiting_context = list(context or [])
            sess.awaiting_since_unix = int(time.time())
            sess.status = "waiting"
            sess.last_active_unix = int(time.time())

    def clear_awaiting(self, agent_kind: str, session_id: str) -> None:
        with self._lock:
            sess = self._sessions.get(self._key(agent_kind, session_id))
            if sess is None:
                return
            sess.awaiting_kind = None
            sess.awaiting_context = []
            sess.awaiting_since_unix = 0

    def set_last_assistant_text(self, agent_kind: str, session_id: str, text: str) -> None:
        with self._lock:
            sess = self._sessions.get(self._key(agent_kind, session_id))
            if sess is None:
                return
            sess.last_assistant_text = text[:4000]

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

        # Wire-size cap: progressive belt-tightening to fit CONSOLE_MAX_LINE.
        # Step 1: shrink entries oldest-first (cheapest, most expendable info).
        # Step 2: keep awaiting_kind/awaiting_since but drop awaiting_context
        #         on agents that aren't the most-recent waiting one (only the
        #         most-recent drives the takeover UI anyway).
        # Step 3: drop cwd field (long paths) from non-waiting agents.
        # Step 4: drop oldest non-waiting agents entirely. Never drop the
        #         most-recent waiting agent — that's the takeover anchor.
        def wire_size() -> int:
            return len(json.dumps(snap, separators=(",", ":")))

        def most_recent_waiting_idx() -> int | None:
            waiting = [(i, a) for i, a in enumerate(snap["agents"])
                       if a.get("awaiting_kind")]
            if not waiting:
                return None
            return max(waiting, key=lambda x: x[1].get("awaiting_since", 0))[0]

        # Step 1
        attempts = 0
        while wire_size() > WIRE_MAX_BYTES and attempts < 64:
            attempts += 1
            biggest = max(snap["agents"], key=lambda a: len(a["entries"]), default=None)
            if biggest is None or not biggest["entries"]:
                break
            biggest["entries"].pop()

        # Step 2
        if wire_size() > WIRE_MAX_BYTES:
            keep_idx = most_recent_waiting_idx()
            for i, a in enumerate(snap["agents"]):
                if i != keep_idx and "awaiting_context" in a:
                    a.pop("awaiting_context", None)

        # Step 3
        if wire_size() > WIRE_MAX_BYTES:
            keep_idx = most_recent_waiting_idx()
            for i, a in enumerate(snap["agents"]):
                if i != keep_idx and a.get("cwd"):
                    a["cwd"] = ""

        # Step 4: drop oldest non-waiting agents until fit.
        while wire_size() > WIRE_MAX_BYTES and len(snap["agents"]) > 1:
            keep_idx = most_recent_waiting_idx()
            droppable = [
                (i, a) for i, a in enumerate(snap["agents"])
                if i != keep_idx and not a.get("awaiting_kind")
            ]
            if not droppable:
                break
            # Oldest = smallest last_active_unix.
            oldest_i, _ = min(droppable, key=lambda x: x[1].get("last_active_unix", 0))
            snap["agents"].pop(oldest_i)
        return snap

    def sweep_stale(self, idle_after_s: int = 300) -> int:
        """Drop sessions idle past `idle_after_s` with no awaiting state.
        Returns the number dropped. Called periodically by the publisher."""
        now = int(time.time())
        dropped = 0
        with self._lock:
            stale = [
                k for k, s in self._sessions.items()
                if not s.awaiting_kind
                and s.status != "waiting"
                and (now - s.last_active_unix) > idle_after_s
            ]
            for k in stale:
                self._sessions.pop(k, None)
                dropped += 1
        return dropped

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
# Transport — now provided by esp_harness.client.open_persistent_session.
# Previously the bridge carried its own _TCPTransport + _SerialTransport
# (~140 LOC); both moved upstream in esp-harness v0.2.0 (gaps G-1, G-3).
# We keep a thin format helper so the existing serve/replay/status/bench
# argparse plumbing (port-kind serial|tcp, port "COM9"|"host:port") still
# uses a single string everywhere.
# ─────────────────────────────────────────────────────────────────────────────


def _resolve_port_arg(port_kind: str, port: str) -> str:
    """Turn (port_kind, port) into the single-string port argument the
    new ``open_persistent_session`` factory consumes.

    The factory auto-detects serial vs TCP by looking for ``host:port``
    shape, so for serial we pass the bare COM name and for TCP we pass
    ``host:port``. The legacy CLI shape with --port-kind is preserved
    so existing scripts / docs keep working.
    """
    if port_kind == "tcp":
        return port
    if port_kind == "serial":
        return port
    raise ValueError(f"unknown port-kind: {port_kind!r}")


# ─────────────────────────────────────────────────────────────────────────────
# Device pusher (v1) — owns the transport, the EVT reader and the reply parser
# ─────────────────────────────────────────────────────────────────────────────

_PERM_RE = re.compile(r"permission\s+id=(\S+)\s+decision=(\w+)")


class DevicePusher:
    """Transport-agnostic pusher with reconnect, buffering, EVT reader.

    Built on :class:`esp_harness.client.SessionHandle` — the bridge
    previously rolled its own transport + reader-loop + payload-follows
    parser. All three moved upstream in esp-harness v0.2.0 (gaps G-1,
    G-3, G-H1, G-H3). The bridge keeps the surface area that's
    consumer-specific: reconnect orchestration, snapshot buffering,
    health bookkeeping, permission round-trip, and dry-run handling.
    """

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
        self._port_arg = _resolve_port_arg(port_kind, port) if not dry_run else ""
        self._session: Optional[SessionHandle] = None
        self._lock = threading.Lock()
        self._timings: list[float] = []
        self._permission_waiters: dict[str, queue.Queue[str]] = {}
        self._buffered_snapshot: Optional[dict] = None
        self._stop = threading.Event()
        self._reconnect_thread: Optional[threading.Thread] = None

    # ── lifecycle ─────────────────────────────────────────────────────
    def open_with_retry(self, *, retry_total_s: float = 30.0, retry_every_s: float = 2.0) -> bool:
        if self.dry_run:
            return True
        deadline = time.monotonic() + retry_total_s
        last_err: Exception | None = None
        while time.monotonic() < deadline:
            try:
                self._open_session()
                self.health.connected = True
                return True
            except TransportError as e:
                last_err = e
                self.health.connected = False
                time.sleep(retry_every_s)
        print(f"[bridge] open failed after {retry_total_s}s: {last_err}", file=sys.stderr)
        return False

    def _open_session(self) -> None:
        """Open a SessionHandle and wire up our event subscribers.

        Called from open_with_retry and from the reconnect path. We
        register handlers for `evt` (permission round-trips) and
        `payload` (HEALTH blob) plus an on_err to log device-side
        errors so they're never silently dropped (gap G-H3).
        """
        session = open_persistent_session(self._port_arg)
        session.on_event(self._on_payload, kinds=frozenset({"payload"}))
        session.on_event(self._on_evt, kinds=frozenset({"evt"}))
        session.on_err(self._on_err)
        self._session = session

    def close(self) -> None:
        self._stop.set()
        if self._session is not None:
            try:
                self._session.close()
            except Exception:
                pass
            self._session = None
        self.health.connected = False

    # ── line push ─────────────────────────────────────────────────────
    def push(self, cmd: str, payload: Optional[dict]) -> dict:
        """Push one ``dash <cmd>`` line. Returns {ok, elapsed_ms, ...}.

        Payload is wrapped in double-quotes so the device tokeniser (post
        G-7 fix) preserves nested JSON quotes intact. The leading `"`
        triggers the "preserve inner quotes" mode in the device parser.
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
            sess = self._session
            if sess is None or not sess.is_open:
                # Attempt fast reconnect for in-flight push
                self._open_session()
                if self.on_reconnect:
                    try:
                        self.on_reconnect()
                    except Exception as e:
                        print(f"[bridge] on_reconnect raised: {e}", file=sys.stderr)
                self.health.connected = True
                sess = self._session
                assert sess is not None
            sess.write_line(line)
            elapsed_ms = (time.monotonic() - started) * 1000
            with self._lock:
                self._timings.append(elapsed_ms)
            return {"ok": True, "elapsed_ms": elapsed_ms}
        except TransportError as e:
            self.health.connected = False
            self._schedule_reconnect()
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

    # ── event handlers (called from SessionHandle reader thread) ──────
    def _on_payload(self, evt: ReplyEvent) -> None:
        """Multi-line OK: payload follows tag=<TAG> bodies arrive here.
        Today only HEALTH is consumed; future payload tags (e.g.
        screenshots streamed via DUMP) can plug in by adding a branch."""
        if evt.tag == "HEALTH":
            try:
                obj = json.loads(evt.blob)
            except json.JSONDecodeError as e:
                print(f"[bridge] bad HEALTH json: {e}", file=sys.stderr)
                return
            self._record_health(obj)

    def _on_evt(self, evt: ReplyEvent) -> None:
        """EVT lines arrive here. We extract permission decisions
        (`permission id=<req_id> decision=<allow|deny|once>`) and dispatch
        to whichever request_permission() call is waiting on that id."""
        m = _PERM_RE.search(evt.text)
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

    def _on_err(self, evt: ReplyEvent) -> None:
        """ERR replies (gap G-H3) — log to stderr so the operator sees
        them. The pre-v0.2 bridge silently dropped these."""
        print(f"[bridge] device ERR: {evt.text}", file=sys.stderr)

    def _schedule_reconnect(self) -> None:
        """Spawn a background reconnect attempt if one isn't already
        running. Keeps the snapshot publisher responsive — push()
        returns immediately, the reconnect happens out-of-line."""
        with self._lock:
            if self._reconnect_thread and self._reconnect_thread.is_alive():
                return
            self._reconnect_thread = threading.Thread(
                target=self._reconnect_loop, daemon=True, name="bridge-reconnect",
            )
            self._reconnect_thread.start()

    def _reconnect_loop(self) -> None:
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline and not self._stop.is_set():
            try:
                if self._session is not None:
                    try:
                        self._session.close()
                    except Exception:
                        pass
                self._open_session()
                self.health.connected = True
                print("[bridge] reconnected", file=sys.stderr)
                if self.on_reconnect:
                    try:
                        self.on_reconnect()
                    except Exception as e:
                        print(f"[bridge] on_reconnect raised: {e}", file=sys.stderr)
                buf = self.take_buffered()
                if buf is not None:
                    self.push("snapshot", buf)
                return
            except TransportError:
                time.sleep(2.0)

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


# ─────────────────────────────────────────────────────────────────────────────
# Awaiting classifier (v2.3.0)
# ─────────────────────────────────────────────────────────────────────────────
#
# Maps the agent's last assistant message + event type onto one of five
# AWAITING kinds. The device renders a different headline + glyph per kind
# so the user knows at a glance what kind of input is needed *before* they
# context-switch into the terminal.
#
# The CC native hook surface only signals two kinds directly:
#   - PreToolUse(permission_required) -> approve
#   - Stop                            -> generic end-of-turn
#
# For Stop we look at the assistant's LAST text and apply heuristics to
# infer pick / type / clarify. Misclassifications degrade to "continue"
# which is always a safe fallback. The classifier is small + cheap to
# extend: add a new pattern in the right block and the next snapshot
# carries the new kind.

import re as _re

_NUMBERED_LINE_RE = _re.compile(
    r"^\s*(?:[\d]+\s*[\.\)]\s+|[\-\*•]\s+|[a-eA-E]\s*[\.\)]\s+)",
    _re.MULTILINE,
)
_CLARIFY_KEYWORDS = (
    "did you mean", "do you mean", "could you clarify", "could you confirm",
    "to clarify", "ambiguous", "unclear", "not sure which", "which one",
    "which of these", "should i assume", "want me to", "shall i",
)


def _has_numbered_options(text: str, min_options: int = 2) -> bool:
    """True iff text contains at least min_options numbered/bulleted lines."""
    if not text:
        return False
    matches = _NUMBERED_LINE_RE.findall(text)
    return len(matches) >= min_options


def _extract_options(text: str, max_n: int = 4) -> list[str]:
    """Pull the option labels (without numbering) from a numbered list."""
    out = []
    for line in text.splitlines():
        m = _re.match(
            r"^\s*(?:\d+\s*[\.\)]|\-|\*|•|[a-eA-E]\s*[\.\)])\s+(.+?)\s*$",
            line,
        )
        if m:
            out.append(m.group(1)[:32])
            if len(out) >= max_n:
                break
    return out


def _short_sentences(text: str, max_chars: int = 80) -> list[str]:
    """Split into two short lines for the AWAITING ctx slot."""
    text = " ".join(text.split())          # collapse whitespace
    text = text.rstrip(".?!:;,")
    if len(text) <= max_chars:
        return [text]
    # Try to split at a natural break point near the middle
    half = max_chars // 2 + 8
    cut = text.rfind(" ", 0, max_chars)
    if cut < 12:
        cut = max_chars
    return [text[:cut], text[cut:].lstrip()[:max_chars - 4] + "…"]


def _ends_with_question(text: str) -> bool:
    s = text.rstrip()
    return s.endswith("?") or s.endswith("？")


def classify_awaiting(
    event_type: str,
    last_assistant_text: str,
    tool_name: str = "",
    tool_input_summary: str = "",
) -> tuple[str, list[str]]:
    """Classify the AWAITING kind + build the device-facing context lines.

    Returns ``(kind, context_lines)`` where ``kind`` is one of
    ``continue / approve / pick / type / clarify`` and ``context_lines``
    is a list of ≤ 3 short strings (each ≤ ~40 chars) the device renders
    under the headline.

    Designed to never raise — bad input just falls through to "continue".
    """
    # ── 1. Permission gate (CC native signal) ──
    if event_type == "pre_tool_use_permission":
        ctx = [f"{tool_name}:" if tool_name else "tool:"]
        if tool_input_summary:
            ctx.append(tool_input_summary[:60])
        return "approve", ctx

    text = (last_assistant_text or "").strip()
    if not text:
        return "continue", ["finished its turn"]

    # ── 2. Numbered options → pick ──
    if _has_numbered_options(text, min_options=2):
        opts = _extract_options(text, max_n=4)
        if opts:
            # Headline above intro line (if any) plus option preview
            # e.g. ["migrate strategy:", "inline · defer · abort"]
            joined = " · ".join(opts[:3])
            # Try to find an intro / lead-in sentence (text before first numbered line)
            lead = text.split("\n", 1)[0]
            first_num = _NUMBERED_LINE_RE.search(text)
            if first_num and first_num.start() > 0:
                lead = text[:first_num.start()].strip().splitlines()
                lead = lead[-1] if lead else ""
                lead = lead.rstrip(":.").strip()
            if lead and len(lead) < 60:
                return "pick", [lead[:48] + ":", joined[:60]]
            return "pick", [f"{len(opts)} options:", joined[:60]]

    # ── 3. Clarify keywords ──
    lowered = text.lower()
    if any(kw in lowered for kw in _CLARIFY_KEYWORDS):
        return "clarify", _short_sentences(text)

    # ── 4. Open-ended question ending with ? ──
    # Look at the last sentence specifically — full message may end in
    # something else like "Done." even though there's a question above.
    sentences = _re.split(r"(?<=[\.\!\?])\s+", text)
    last_sent = sentences[-1] if sentences else ""
    if _ends_with_question(last_sent) and 4 < len(last_sent) < 200:
        return "type", _short_sentences(last_sent)

    # ── 5. Default: continue ──
    # Build a calm "what did you just do" summary from the text.
    return "continue", _short_sentences(text)


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
        # v2.3.0: assistant's last message text, used by the AWAITING
        # classifier on Stop events. hook_dispatch.py is responsible for
        # extracting it from the transcript file when available.
        "last_assistant_text": (
            raw.get("last_assistant_text")
            or raw.get("text")
            or raw.get("last_message")
            or ""
        ),
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
            # User just submitted a new prompt → the agent moves out of
            # any "awaiting you" state. Clear awaiting_* fields so the
            # device leaves the AWAITING takeover and returns to ambient.
            self.registry.upsert(agent, sid, status="running",
                                 cwd=evt["cwd"] or None,
                                 msg=evt["summary"])
            self.registry.clear_awaiting(agent, sid)
            self.publisher.bump()
            return {"continue": True}

        if t == "pre_tool_use":
            # msg owner = UserPromptSubmit. Tool calls become entries[]
            # rows; they do NOT overwrite the user-visible prompt at the
            # top of the agent card. Previously each tool call clobbered
            # the prompt and the user lost the "what did I ask" anchor.
            self.registry.upsert(agent, sid, status="running",
                                 cwd=evt["cwd"] or None,
                                 tool=evt["tool_name"] or "tool",
                                 summary=evt["summary"])
            # v2.3.0: any tool firing means the model is NOT awaiting
            # user input. Clear stale awaiting state from a prior turn.
            self.registry.clear_awaiting(agent, sid)
            if looks_like_permission_required(evt):
                prompt = {
                    "id": f"req_{uuid.uuid4().hex[:8]}",
                    "tool": evt["tool_name"],
                    "hint": (evt["summary"] or evt["tool_name"])[:80],
                    "agent_kind": agent,
                    "session_id": sid,
                }
                self.registry.set_pending(agent, sid, prompt)
                # AWAITING(approve) — the device shows a takeover with
                # the lock glyph + tool name + command preview, plus the
                # BOOT/USER button affordances. Decision still flows
                # through `request_permission()` below; the AWAITING is
                # the visible side of the same wait.
                kind, ctx = classify_awaiting(
                    "pre_tool_use_permission",
                    "",
                    tool_name=evt["tool_name"],
                    tool_input_summary=evt["summary"],
                )
                self.registry.set_awaiting(agent, sid, kind=kind, context=ctx)
                self.publisher.bump()
                decision = self.pusher.request_permission(
                    prompt, timeout=self.permission_timeout_s,
                )
                self.registry.set_pending(agent, sid, None)
                # Permission resolved → leave AWAITING.
                self.registry.clear_awaiting(agent, sid)
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
            # See pre_tool_use note: msg stays the user prompt; tool
            # completion goes to entries[] (already handled via the
            # `tool=` upsert kwarg when summary is present), and tokens
            # accumulate.
            self.registry.upsert(agent, sid, status="running",
                                 tool=evt["tool_name"] or "tool",
                                 summary=evt["summary"] or "ok",
                                 tokens=evt["tokens"])
            self.publisher.bump()
            return {"continue": True}

        if t == "stop":
            # v2.3.0: Stop = "ball in user's court". Status moves to
            # `waiting` (not `idle`), and the classifier picks the
            # AWAITING kind from the assistant's last message so the
            # device's takeover scene shows the right headline + glyph.
            #
            # The session is NOT dropped from the registry on Stop —
            # the user might come back to it. A long-idle sweeper
            # (running in DevicePusher's keepalive loop) drops sessions
            # that stay in waiting for > 30 minutes with no activity.
            kind, ctx = classify_awaiting(
                "stop",
                evt.get("last_assistant_text", ""),
            )
            self.registry.upsert(agent, sid, status="waiting")
            self.registry.set_awaiting(agent, sid, kind=kind, context=ctx)
            self.publisher.bump()
            return {"continue": True}

        if t == "assistant_event":
            # Assistant text doesn't replace the prompt either — model
            # response becomes its own entries[] row. Keeps the card's
            # top line stable as "what the user asked". Also stash the
            # full text on the session so a later Stop event's
            # classifier can read it.
            self.registry.upsert(agent, sid, status="running",
                                 tool="assistant",
                                 summary=evt["summary"])
            text = (evt.get("last_assistant_text") or "")
            if text:
                self.registry.set_last_assistant_text(agent, sid, text)
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
