"""claude_buddy_bridge.py — host-side daemon that bridges Claude Code CLI
and Codex CLI hook events to the ESP32 agent dashboard.

Design (Agent H scope):
- A long-running daemon that listens on a localhost TCP socket (Windows has
  flaky UDS support — TCP is portable across the two CLIs we wrap).
- Three input sources funnel into the same event queue:
    1. ``--stdin``           — read newline-delimited JSON from stdin (test harness)
    2. ``hook_dispatch.py``  — Claude Code hook commands forward stdin to the
       socket, then optionally read a JSON response line and pipe it back to
       Claude.
    3. ``codex_wrapper.py``  — shells out to ``codex`` and tees its JSON event
       stream into the socket. (Codex 0.x has no native hook system, so we
       wrap.)
- A ``SessionRegistry`` aggregates state per (agent, pid/session_id).
- A throttled writer emits ``dash snapshot`` to the device at most every
  ``--snapshot-min-interval-ms`` (default 250ms) and at least every
  ``--keepalive-ms`` (default 10000ms).
- For ``PreToolUse`` events that trip a permission prompt, the daemon pushes
  ``dash prompt`` and blocks (with timeout) on the matching device EVT before
  returning the decision JSON to ``hook_dispatch.py``.

Run modes:
    python claude_buddy_bridge.py serve [--port COM9] [--dry-run] [--listen 127.0.0.1:7321]
    python claude_buddy_bridge.py send  --type pre_tool_use < event.json
    python claude_buddy_bridge.py replay sample_session.jsonl --dry-run

This file deliberately has zero third-party dependencies except ``pyserial``
(via ``esp_harness.core.console_session``), and only when ``--dry-run`` is
not set.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import socket
import socketserver
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field, asdict
from datetime import date
from typing import Any, Optional


# ─────────────────────────────────────────────────────────────────────────────
# Configuration knobs
# ─────────────────────────────────────────────────────────────────────────────

DEFAULT_LISTEN_HOST = "127.0.0.1"
DEFAULT_LISTEN_PORT = 7321
DEFAULT_SERIAL_PORT = "COM9"
DEFAULT_SNAPSHOT_MIN_INTERVAL_MS = 250
DEFAULT_KEEPALIVE_MS = 10_000
DEFAULT_PERMISSION_TIMEOUT_S = 30.0

ESP_HARNESS_PY = r"D:\Code\esp-harness\tools\esp-harness\.venv\Scripts\python.exe"


# ─────────────────────────────────────────────────────────────────────────────
# Session registry
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class Session:
    key: str                         # agent + session id
    agent: str                       # "claude-code" | "codex"
    session_id: str
    status: str = "running"          # running | waiting | idle
    last_msg: str = ""
    pending_prompt: Optional[dict] = None  # {id, tool, hint}
    last_update_ts: float = field(default_factory=time.time)

    def to_entry_str(self) -> str:
        """How this session shows up in the snapshot ``entries`` array."""
        prefix = "C" if self.agent == "claude-code" else "X"
        short_id = self.session_id[:6] if self.session_id else "?"
        return f"{prefix}:{short_id} {self.status}"


class SessionRegistry:
    """Thread-safe in-memory store of all live agent sessions."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._sessions: dict[str, Session] = {}
        self._tokens_cumulative = 0
        self._tokens_today = 0
        self._tokens_latest_sample = 0
        self._tokens_today_date = date.today().isoformat()
        self._last_msg = ""

    # ── tokens ────────────────────────────────────────────────────────
    def add_tokens(self, n: int) -> None:
        if n <= 0:
            return
        with self._lock:
            today = date.today().isoformat()
            if today != self._tokens_today_date:
                self._tokens_today_date = today
                self._tokens_today = 0
            self._tokens_cumulative += n
            self._tokens_today += n
            self._tokens_latest_sample = n

    # ── sessions ──────────────────────────────────────────────────────
    def upsert(self, agent: str, session_id: str, **fields) -> Session:
        key = f"{agent}:{session_id}"
        with self._lock:
            sess = self._sessions.get(key)
            if sess is None:
                sess = Session(key=key, agent=agent, session_id=session_id)
                self._sessions[key] = sess
            for k, v in fields.items():
                if hasattr(sess, k) and v is not None:
                    setattr(sess, k, v)
            sess.last_update_ts = time.time()
            if fields.get("last_msg"):
                self._last_msg = fields["last_msg"]
            return sess

    def drop(self, agent: str, session_id: str) -> None:
        key = f"{agent}:{session_id}"
        with self._lock:
            self._sessions.pop(key, None)

    def set_pending(self, agent: str, session_id: str, prompt: dict | None) -> None:
        key = f"{agent}:{session_id}"
        with self._lock:
            sess = self._sessions.get(key)
            if sess is None:
                sess = Session(key=key, agent=agent, session_id=session_id)
                self._sessions[key] = sess
            sess.pending_prompt = prompt
            sess.status = "waiting" if prompt else "running"
            sess.last_update_ts = time.time()

    # ── snapshot ──────────────────────────────────────────────────────
    def snapshot(self) -> dict:
        with self._lock:
            running = sum(1 for s in self._sessions.values() if s.status == "running")
            waiting = sum(1 for s in self._sessions.values() if s.status == "waiting")
            total = len(self._sessions)
            entries = [s.to_entry_str() for s in self._sessions.values()][:8]
            pending = None
            for s in self._sessions.values():
                if s.pending_prompt:
                    pending = s.pending_prompt
                    break
            return {
                "total": total,
                "running": running,
                "waiting": waiting,
                "msg": (self._last_msg or "")[:64],
                "entries": entries,
                "tokens": self._tokens_cumulative,
                "tokens_today": self._tokens_today,
                "prompt": pending,
            }

    def tokens_payload(self) -> dict:
        with self._lock:
            return {
                "cumulative": self._tokens_cumulative,
                "today": self._tokens_today,
                "latest_sample": self._tokens_latest_sample,
            }


# ─────────────────────────────────────────────────────────────────────────────
# Device push
# ─────────────────────────────────────────────────────────────────────────────

class DevicePusher:
    """Wraps device communication. In dry-run mode just prints would-be commands."""

    def __init__(self, *, port: str, dry_run: bool, esp_harness_py: str) -> None:
        self.port = port
        self.dry_run = dry_run
        self.esp_harness_py = esp_harness_py
        self._lock = threading.Lock()
        self._serial_session = None  # lazy persistent pyserial connection
        self._timings: list[float] = []
        self._permission_waiters: dict[str, queue.Queue[str]] = {}
        self._reader_thread: Optional[threading.Thread] = None

    # ── one-shot snapshot/tokens push (uses esp-harness subprocess) ────
    def push(self, cmd: str, payload: dict) -> dict:
        line = f"dash {cmd} {json.dumps(payload, separators=(',', ':'))}"
        if self.dry_run:
            print(f"[DRY] {line}", flush=True)
            return {"ok": True, "dry_run": True}
        started = time.monotonic()
        try:
            proc = subprocess.run(
                [
                    self.esp_harness_py,
                    "-m", "esp_harness", "console",
                    "--cmd", line,
                    "--port", self.port,
                    "--json",
                ],
                capture_output=True,
                timeout=3,
                # Don't pass text=True: on Windows the subprocess pipe may emit
                # non-UTF-8 bytes (Chinese device descriptions in GBK from
                # pyserial errors) and crash the reader thread. Decode by hand
                # with errors="replace".
            )
            elapsed_ms = (time.monotonic() - started) * 1000
            with self._lock:
                self._timings.append(elapsed_ms)
            ok = proc.returncode == 0
            stdout = (proc.stdout or b"").decode("utf-8", errors="replace")
            return {"ok": ok, "elapsed_ms": elapsed_ms, "stdout": stdout[-200:]}
        except subprocess.TimeoutExpired:
            return {"ok": False, "error": "timeout"}
        except FileNotFoundError as e:
            return {"ok": False, "error": f"esp-harness not found: {e}"}

    def timing_stats(self) -> dict:
        with self._lock:
            ts = list(self._timings)
        if not ts:
            return {"count": 0}
        ts_sorted = sorted(ts)
        n = len(ts_sorted)
        median = ts_sorted[n // 2]
        p95 = ts_sorted[int(n * 0.95)] if n >= 20 else ts_sorted[-1]
        jitter = max(ts_sorted) - min(ts_sorted)
        return {
            "count": n,
            "median_ms": round(median, 1),
            "min_ms": round(min(ts_sorted), 1),
            "max_ms": round(max(ts_sorted), 1),
            "p95_ms": round(p95, 1),
            "jitter_ms": round(jitter, 1),
        }

    # ── permission round-trip via persistent pyserial session ──────────
    def _ensure_serial(self):
        """Lazy-open the persistent pyserial connection used for permission
        prompts (we need to read EVT lines back, not just fire-and-forget)."""
        if self.dry_run:
            return None
        if self._serial_session is not None:
            return self._serial_session
        # Add esp-harness src to sys.path
        esp_harness_src = r"D:\Code\esp-harness\tools\esp-harness\src"
        if esp_harness_src not in sys.path:
            sys.path.insert(0, esp_harness_src)
        try:
            from esp_harness.core.console_session import ConsoleSession  # type: ignore
        except Exception as e:
            print(f"[bridge] cannot import ConsoleSession: {e}", file=sys.stderr)
            return None
        sess = ConsoleSession(self.port).__enter__()
        self._serial_session = sess
        # Start background EVT reader
        self._reader_thread = threading.Thread(
            target=self._evt_reader_loop, daemon=True, name="evt-reader"
        )
        self._reader_thread.start()
        return sess

    _PERM_RE = re.compile(r"permission\s+id=(\S+)\s+decision=(\w+)")

    def _evt_reader_loop(self) -> None:
        """Background reader that pulls EVT lines from the serial session and
        dispatches ``permission`` events to the waiter queue keyed on id."""
        sess = self._serial_session
        if sess is None:
            return
        try:
            while True:
                # Use the low-level iter_lines through a 1s deadline window.
                deadline = time.monotonic() + 1.0
                for ln in sess._iter_lines(deadline):
                    if ln == "\0":
                        continue
                    if not ln.startswith("EVT:"):
                        continue
                    body = ln[4:].strip()
                    m = self._PERM_RE.search(body)
                    if not m:
                        continue
                    req_id, decision = m.group(1), m.group(2)
                    with self._lock:
                        q = self._permission_waiters.get(req_id)
                    if q is not None:
                        q.put(decision)
        except Exception as e:
            print(f"[bridge] evt reader died: {e}", file=sys.stderr)

    def request_permission(self, prompt: dict, timeout: float) -> str | None:
        """Push ``dash prompt`` and wait for a matching device EVT.

        Returns the decision string (``once`` / ``deny`` / ...) or None on timeout.
        """
        req_id = prompt.get("id") or f"req_{uuid.uuid4().hex[:8]}"
        prompt = dict(prompt)
        prompt["id"] = req_id

        if self.dry_run:
            # In dry-run mode, simulate a "deny" decision after a tick.
            print(f"[DRY] dash prompt {json.dumps(prompt, separators=(',', ':'))}",
                  flush=True)
            return "deny"

        q: queue.Queue[str] = queue.Queue(maxsize=1)
        with self._lock:
            self._permission_waiters[req_id] = q

        sess = self._ensure_serial()
        if sess is None:
            with self._lock:
                self._permission_waiters.pop(req_id, None)
            return None

        # Send dash prompt over the persistent session (no wait for OK timing —
        # the EVT is what we care about).
        try:
            cmd_line = f"dash prompt {json.dumps(prompt, separators=(',', ':'))}"
            sess._ser.write((cmd_line + "\n").encode("utf-8"))  # type: ignore[attr-defined]
            sess._ser.flush()  # type: ignore[attr-defined]
        except Exception as e:
            print(f"[bridge] prompt push failed: {e}", file=sys.stderr)
            with self._lock:
                self._permission_waiters.pop(req_id, None)
            return None

        try:
            decision = q.get(timeout=timeout)
            return decision
        except queue.Empty:
            return None
        finally:
            with self._lock:
                self._permission_waiters.pop(req_id, None)


# ─────────────────────────────────────────────────────────────────────────────
# Snapshot publisher (throttled, with keepalive)
# ─────────────────────────────────────────────────────────────────────────────

class SnapshotPublisher(threading.Thread):
    """Coalescing publisher: on a `bump()`, send at most once per ``min_interval``.

    Also fires a keepalive snapshot every ``keepalive`` seconds.
    """

    def __init__(
        self,
        *,
        registry: SessionRegistry,
        pusher: DevicePusher,
        min_interval_ms: int,
        keepalive_ms: int,
    ) -> None:
        super().__init__(daemon=True, name="snapshot-publisher")
        self.registry = registry
        self.pusher = pusher
        self.min_interval = min_interval_ms / 1000.0
        self.keepalive = keepalive_ms / 1000.0
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._last_push_ts = 0.0
        self._last_snapshot_json = ""

    def bump(self) -> None:
        self._wake.set()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()

    def run(self) -> None:
        while not self._stop.is_set():
            now = time.monotonic()
            # next deadline = max(throttle-wait, keepalive-wait)
            throttle_wait = max(0.0, self.min_interval - (now - self._last_push_ts))
            keepalive_wait = max(0.0, self.keepalive - (now - self._last_push_ts))
            wait = min(keepalive_wait, throttle_wait) if self._wake.is_set() else keepalive_wait
            self._wake.wait(timeout=max(0.05, wait))
            if self._stop.is_set():
                break
            now = time.monotonic()
            since = now - self._last_push_ts
            if self._wake.is_set() and since < self.min_interval:
                continue  # still throttled, loop will recompute
            snap = self.registry.snapshot()
            snap_json = json.dumps(snap, sort_keys=True)
            changed = snap_json != self._last_snapshot_json
            is_keepalive = since >= self.keepalive
            if not changed and not is_keepalive:
                self._wake.clear()
                continue
            res = self.pusher.push("snapshot", snap)
            self._last_push_ts = time.monotonic()
            self._last_snapshot_json = snap_json
            self._wake.clear()
            if not res.get("ok") and not res.get("dry_run"):
                print(f"[bridge] push failed: {res}", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# Event normalization
# ─────────────────────────────────────────────────────────────────────────────

DANGEROUS_BASH_RE = re.compile(
    r"\b(rm\s+-rf|mkfs|dd\s+if=|:(){:|:&};:|shutdown|reboot|format\s+[a-z]:)",
    re.IGNORECASE,
)


def looks_like_permission_required(event: dict) -> bool:
    """Heuristic: does this PreToolUse event probably need a human click?

    Claude Code itself decides whether to show a permission prompt based on
    its own allow/deny config; the hook gets called regardless. The bridge
    asks the device when the event looks risky AND the user has opted in
    via ``permission_required: true`` in the hook input (Claude Code can
    inject this), OR when the bash command matches a danger regex.
    """
    if event.get("permission_required") is True:
        return True
    tool = event.get("tool_name", "")
    if tool == "Bash":
        cmd = (event.get("tool_input", {}) or {}).get("command", "")
        if DANGEROUS_BASH_RE.search(cmd):
            return True
    return False


def normalize_event(raw: dict) -> dict:
    """Map Claude Code / Codex hook payloads into the bridge's internal schema.

    Internal schema:
        {
          "type": "pre_tool_use" | "post_tool_use" | "user_prompt_submit" |
                  "stop" | "assistant_event" | "tokens" | "raw",
          "agent": "claude-code" | "codex",
          "session_id": "...",
          "tool_name": "...",
          "tool_input": {...},
          "summary": "short human str",
          "tokens": int,
          "permission_required": bool,
        }
    """
    out: dict[str, Any] = {
        "type": raw.get("type", "raw"),
        "agent": raw.get("agent", "claude-code"),
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
    }
    # Best-effort summary string
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
# Bridge core: per-event handling
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
        """Process one event. Returns a response dict (used for hook stdout)."""
        evt = normalize_event(raw)
        t = evt["type"]
        agent = evt["agent"]
        sid = evt["session_id"]

        if evt["tokens"]:
            self.registry.add_tokens(evt["tokens"])

        if t == "user_prompt_submit":
            self.registry.upsert(agent, sid, status="running",
                                 last_msg=evt["summary"])
            self.publisher.bump()
            return {"continue": True}

        if t == "pre_tool_use":
            self.registry.upsert(agent, sid, status="running",
                                 last_msg=evt["summary"])
            if looks_like_permission_required(evt):
                # Build the prompt payload and ask the device.
                hint = evt["summary"] or evt["tool_name"]
                prompt = {
                    "id": f"req_{uuid.uuid4().hex[:8]}",
                    "tool": evt["tool_name"],
                    "hint": hint[:80],
                }
                self.registry.set_pending(agent, sid, prompt)
                self.publisher.bump()
                decision = self.pusher.request_permission(
                    prompt, timeout=self.permission_timeout_s
                )
                self.registry.set_pending(agent, sid, None)
                self.publisher.bump()
                # Map device decision into Claude Code hook response shape.
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
                # Timeout / unknown: defer to Claude Code's default.
                return {"continue": True,
                        "systemMessage": f"dashboard timeout (no decision): {decision}"}
            self.publisher.bump()
            return {"continue": True}

        if t == "post_tool_use":
            self.registry.upsert(agent, sid, status="running",
                                 last_msg=evt["summary"] or "tool ok")
            self.publisher.bump()
            return {"continue": True}

        if t == "stop":
            # Mark idle, then drop after a beat so a brief flash shows.
            self.registry.upsert(agent, sid, status="idle",
                                 last_msg=evt["summary"] or "(idle)")
            self.publisher.bump()
            # Also push a dedicated 'idle' command if device cares.
            self.pusher.push("idle", {})
            # Schedule a delayed drop so the entry leaves the list.
            def _drop():
                time.sleep(2.0)
                self.registry.drop(agent, sid)
                self.publisher.bump()
            threading.Thread(target=_drop, daemon=True).start()
            return {"continue": True}

        if t == "assistant_event":
            self.registry.upsert(agent, sid, status="running",
                                 last_msg=evt["summary"])
            self.publisher.bump()
            return {"continue": True}

        if t == "tokens":
            # already added above
            self.pusher.push("tokens", self.registry.tokens_payload())
            return {"continue": True}

        # Unknown: just log and pass through.
        return {"continue": True}


# ─────────────────────────────────────────────────────────────────────────────
# TCP server
# ─────────────────────────────────────────────────────────────────────────────

class _Handler(socketserver.StreamRequestHandler):
    bridge: Bridge = None  # type: ignore[assignment]

    def handle(self) -> None:
        # One request per connection: read one JSON line, write one JSON line.
        try:
            line = self.rfile.readline()
            if not line:
                return
            try:
                raw = json.loads(line.decode("utf-8"))
            except json.JSONDecodeError as e:
                self.wfile.write(
                    (json.dumps({"continue": True, "error": f"bad json: {e}"}) + "\n")
                    .encode("utf-8")
                )
                return
            resp = self.bridge.handle(raw)
            self.wfile.write((json.dumps(resp) + "\n").encode("utf-8"))
        except Exception as e:
            try:
                self.wfile.write(
                    (json.dumps({"continue": True, "error": str(e)}) + "\n")
                    .encode("utf-8")
                )
            except Exception:
                pass


class _ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


# ─────────────────────────────────────────────────────────────────────────────
# Modes: serve / send / replay
# ─────────────────────────────────────────────────────────────────────────────

def _build_bridge(args) -> tuple[Bridge, DevicePusher, SnapshotPublisher, SessionRegistry]:
    registry = SessionRegistry()
    pusher = DevicePusher(
        port=args.serial_port,
        dry_run=args.dry_run,
        esp_harness_py=args.esp_harness_py,
    )
    publisher = SnapshotPublisher(
        registry=registry,
        pusher=pusher,
        min_interval_ms=args.snapshot_min_interval_ms,
        keepalive_ms=args.keepalive_ms,
    )
    bridge = Bridge(
        registry=registry,
        pusher=pusher,
        publisher=publisher,
        permission_timeout_s=args.permission_timeout_s,
    )
    return bridge, pusher, publisher, registry


def cmd_serve(args) -> int:
    bridge, pusher, publisher, registry = _build_bridge(args)
    publisher.start()

    host, port_s = args.listen.split(":")
    port = int(port_s)

    _Handler.bridge = bridge
    server = _ThreadedTCPServer((host, port), _Handler)
    print(f"[bridge] serving on {host}:{port} | dry_run={args.dry_run} | "
          f"serial={args.serial_port}", flush=True)

    # Also accept events on stdin if requested (useful for `cat events | bridge`).
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
        stats = pusher.timing_stats()
        print(f"[bridge] push timing: {stats}", flush=True)
    return 0


def cmd_send(args) -> int:
    """Connect to a running bridge and forward one event."""
    raw = sys.stdin.read().strip()
    if not raw:
        print('{"continue": true, "error": "empty stdin"}')
        return 0
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        # The hook gave us not-JSON — wrap it as a raw text event.
        payload = {"type": "raw", "text": raw}

    # Hook-specific type tagging.
    if args.type and "type" not in payload:
        payload["type"] = args.type
    if args.agent and "agent" not in payload:
        payload["agent"] = args.agent

    host, port_s = args.listen.split(":")
    port = int(port_s)
    try:
        with socket.create_connection((host, port), timeout=args.timeout) as sock:
            sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
            sock.settimeout(args.timeout)
            data = sock.makefile("r", encoding="utf-8").readline()
            print(data.strip() or '{"continue": true}')
    except (ConnectionRefusedError, socket.timeout, OSError) as e:
        # Bridge isn't running — never block the hook host.
        print(json.dumps({"continue": True, "error": f"bridge offline: {e}"}))
    return 0


def cmd_replay(args) -> int:
    """Process a JSONL file in-process (no socket). For offline testing."""
    bridge, pusher, publisher, registry = _build_bridge(args)
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
                print(f"[replay] {i:02d} <- {raw.get('type', '?'):20s} "
                      f"-> {json.dumps(resp)}")
                if args.pace_ms:
                    time.sleep(args.pace_ms / 1000.0)
        # Drain final pending snapshot
        time.sleep(0.4)
    finally:
        publisher.stop()
        stats = pusher.timing_stats()
        print(f"[replay] push timing: {stats}")
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# argparse plumbing
# ─────────────────────────────────────────────────────────────────────────────

def _add_common(p):
    p.add_argument("--serial-port", default=DEFAULT_SERIAL_PORT,
                   help=f"COM port (default {DEFAULT_SERIAL_PORT})")
    p.add_argument("--dry-run", action="store_true",
                   help="don't actually push to the device, just print")
    p.add_argument("--esp-harness-py", default=ESP_HARNESS_PY,
                   help="path to the esp-harness venv python")
    p.add_argument("--snapshot-min-interval-ms", type=int,
                   default=DEFAULT_SNAPSHOT_MIN_INTERVAL_MS)
    p.add_argument("--keepalive-ms", type=int, default=DEFAULT_KEEPALIVE_MS)
    p.add_argument("--permission-timeout-s", type=float,
                   default=DEFAULT_PERMISSION_TIMEOUT_S)
    p.add_argument("--listen", default=f"{DEFAULT_LISTEN_HOST}:{DEFAULT_LISTEN_PORT}",
                   help="HOST:PORT for the local TCP server / client")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="claude_buddy_bridge",
        description="Host bridge: Claude Code + Codex hook events → ESP32 dashboard.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_serve = sub.add_parser("serve", help="run the long-lived daemon")
    _add_common(p_serve)
    p_serve.add_argument("--stdin", action="store_true",
                         help="also accept JSONL events on stdin")

    p_send = sub.add_parser("send", help="forward one event to a running bridge")
    p_send.add_argument("--listen", default=f"{DEFAULT_LISTEN_HOST}:{DEFAULT_LISTEN_PORT}")
    p_send.add_argument("--type", default=None,
                        help="override event type (pre_tool_use, post_tool_use, ...)")
    p_send.add_argument("--agent", default=None,
                        help="override agent name (claude-code|codex)")
    p_send.add_argument("--timeout", type=float, default=5.0)

    p_replay = sub.add_parser("replay", help="process a JSONL file in-process")
    _add_common(p_replay)
    p_replay.add_argument("file", help="JSONL file of events to replay")
    p_replay.add_argument("--pace-ms", type=int, default=80,
                          help="sleep between events to exercise throttling")

    args = parser.parse_args(argv)
    if args.cmd == "serve":
        return cmd_serve(args)
    if args.cmd == "send":
        return cmd_send(args)
    if args.cmd == "replay":
        return cmd_replay(args)
    parser.error(f"unknown cmd {args.cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
