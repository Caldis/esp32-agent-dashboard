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

from awaiting_classifier import classify_awaiting
from bridge_runtime import import_esp_harness_client

try:
    import tomllib  # py311+
except ImportError:  # pragma: no cover
    tomllib = None  # type: ignore


# v0.2.0 of the esp-harness framework ships the public persistent-session
# API (gaps G-1, G-3) and the PayloadFollowsReader helper (gap G-H1) that
# this bridge previously rolled by hand. Runtime discovery lives in
# bridge_runtime.py so local development paths do not leak into this script's
# Interface.
_esp_harness_client = import_esp_harness_client()
ReplyEvent = _esp_harness_client.ReplyEvent
SessionHandle = _esp_harness_client.SessionHandle
TransportError = _esp_harness_client.TransportError
open_persistent_session = _esp_harness_client.open_persistent_session


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

# CLAUDE_BUDDY_DEBUG=1 → log every event received + every line pushed to the
# device, so dropped/coalesced events can be traced end-to-end.
_DEBUG = bool(os.environ.get("CLAUDE_BUDDY_DEBUG"))
_rx_seq = 0

# Claude Code fires NO hook on user-interrupt (ESC) — known limitation
# (anthropics/claude-code#9516). It also can miss Stop when it stalls mid-turn
# (#29881). So if a session is "running" but goes silent (no tool in flight) for
# this long, treat the turn as over → "your turn". Tools running (PreToolUse with
# no PostToolUse yet) are exempt so a long build/test is never misread as idle.
IDLE_TURN_S = float(os.environ.get("CLAUDE_BUDDY_IDLE_TURN_S", "60"))

CONFIG_PATH = Path.home() / ".claude-buddy" / "config.toml"
# Device console caps a line at 1023 bytes. The wire line is `dash snapshot "<json>"`
# (~16 bytes of wrapper around the JSON), so keep the JSON itself under ~1000 to
# stay safely below 1023. Raised from 900 → 1000 so fewer agents get trimmed.
WIRE_MAX_BYTES = 1000
# Max entries[] per agent on the wire. The device renders only ~2 per agent, so
# 12 (the in-memory cap) just bloats the snapshot and forces agent-dropping trims.
WIRE_ENTRIES_PER_AGENT = 6

# Serial open watchdog. pyserial's ser.open() can block INDEFINITELY on Windows
# when the USB-Serial-JTAG endpoint wedges (device USB stack hung) — it doesn't
# raise, it just never returns. Without a bound, that hung open takes down every
# thread that touches the port (TCP handlers, publisher, health poller) and the
# whole bridge deadlocks on one bad handle. We run every open in a watchdog
# thread and give up after this long, so the bridge stays alive and keeps
# retrying instead of freezing. Raise via CLAUDE_BUDDY_OPEN_TIMEOUT_S if needed.
SERIAL_OPEN_TIMEOUT_S = float(os.environ.get("CLAUDE_BUDDY_OPEN_TIMEOUT_S", "6.0"))


def _wire_safe(s: str) -> str:
    """Strip lone surrogates so the string can always be UTF-8 encoded onto the
    wire. Event text mis-decoded upstream (e.g. a CJK payload read under a
    non-UTF-8 stdin locale) can carry unpaired surrogates that make
    ``.encode("utf-8")`` raise — which, on the snapshot publisher thread, means
    it never pushes and the device freezes on stale data. Replace them with '?'
    (round-trips clean text untouched)."""
    return s.encode("utf-8", "replace").decode("utf-8")


# The device font (main/zh.ttf) is a GB2312 + ASCII + limited-punctuation subset
# of SimHei — see tools/make_cjk_font.py. Anything outside that set renders as a
# ".notdef" box ("方块字乱码"): agent-favourite symbols (✓ → • ★ …), emoji, and
# traditional-only hanzi. SimHei has no emoji glyphs, so expanding the subset
# can't fix it. Instead we substitute at the host: map common symbols to ASCII
# equivalents that keep their meaning, and drop the truly unrenderable rest.
_DEVICE_PUNCT = set(
    "　、。·ˉ…—～‖‘’“”〔〕〈〉《》「」『』【】（）！？：；，．±×÷°"
)
_SYMBOL_MAP = {
    "✓": "v", "✔": "v", "☑": "v", "√": "v", "✅": "v",
    "✗": "x", "✘": "x", "✕": "x", "❌": "x", "❎": "x",
    "→": "->", "←": "<-", "↑": "^", "↓": "v", "⇒": "=>", "⟶": "->", "➜": "->",
    "•": "-", "‣": "-", "●": "-", "◦": "-", "▪": "-", "▹": ">", "▸": ">", "▶": ">",
    "★": "*", "☆": "*", "⭐": "*", "✦": "*", "✸": "*",
    "⚠": "!", "❗": "!", "‼": "!!", "ℹ": "i", "✨": "*",
    "‑": "-", "‒": "-", "–": "-", "―": "-",   # dash variants → hyphen
    " ": " ",                              # nbsp → space
}


def _device_safe(s: str) -> str:
    """Rewrite *s* so every character is renderable by the device font. ASCII and
    GB2312 hanzi pass through; known symbols become ASCII; everything else
    (emoji, traditional-only hanzi, exotic punctuation) is dropped. Prevents the
    ".notdef" boxes the user saw for agent-emitted ✓/→/•/emoji."""
    out: list[str] = []
    for ch in s:
        if ord(ch) < 0x80 or ch in _DEVICE_PUNCT:
            out.append(ch)
            continue
        mapped = _SYMBOL_MAP.get(ch)
        if mapped is not None:
            out.append(mapped)
            continue
        try:
            ch.encode("gb2312")        # in the device's hanzi set → keep
            out.append(ch)
        except UnicodeEncodeError:
            pass                        # unrenderable → drop rather than show a box
    return "".join(out)

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

_KNOWN_KINDS = frozenset({
    "claude-code", "codex", "cursor", "aider", "windsurf",
    "copilot", "qwen-code", "other",
})
_KIND_PREFIX = {
    "claude-code": "C", "codex": "X", "cursor": "U",
    "aider": "A", "windsurf": "W", "copilot": "P",
    "qwen-code": "Q", "other": "o",
}


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
    # True between a PreToolUse and its PostToolUse — i.e. a tool is actively
    # running (possibly for minutes). Used by the idle-turn sweep so a long tool
    # is NOT mistaken for an interrupted/idle agent.
    tool_in_flight: bool = False
    # Path to the CC transcript. CC keeps writing it WHILE the model thinks (even
    # with no tool calls), so its mtime lets the idle-turn sweep tell "long
    # thinking" (file still growing → keep running) from "ESC/idle" (file
    # static → flip to your turn). Without this, a long think with no tool calls
    # tripped the sweep and the device wrongly showed "your turn" mid-think.
    transcript_path: str = ""
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
    # v2.4.0: dash-state contract. When the agent appends a <dash-state>
    # block at the end of its message, hook_dispatch extracts it and the
    # bridge stores it here. The device's AWAITING takeover renders:
    #   - awaiting_summary as a marquee (LV_LABEL_LONG_SCROLL)
    #   - awaiting_options as a numbered list (1-4)
    # See docs/DASH_STATE_CONTRACT.md.
    awaiting_summary: str = ""
    awaiting_options: list[str] = field(default_factory=list)

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
            # Cap entries on the wire: the device shows only ~2 per agent
            # (scene_sessions) plus a small global aggregate (scene_dashboard),
            # but a single busy agent with 12 entries alone approaches the
            # CONSOLE_MAX_LINE cap — pushing the snapshot over WIRE_MAX_BYTES and
            # making wire-trim drop whole AGENTS (the "events dropped / UI out of
            # sync" symptom). Send the most-recent few; the device never needs more.
            "entries": [e.as_dict() for e in self.entries[:WIRE_ENTRIES_PER_AGENT]],
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
            # v2.4.0: include summary + options when set (dash-state contract).
            if self.awaiting_summary:
                d["awaiting_summary"] = self.awaiting_summary[:200]
            if self.awaiting_options:
                d["awaiting_options"] = [o[:32] for o in self.awaiting_options[:4]]
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
                      context: list[str] | None = None,
                      summary: str | None = None,
                      options: list[str] | None = None) -> None:
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
            # v2.4.0: dash-state summary + options. Empty values clear
            # the field so an option-less turn replaces a previous
            # option-ful turn cleanly.
            if summary is not None:
                sess.awaiting_summary = summary
            if options is not None:
                sess.awaiting_options = list(options)

    def clear_awaiting(self, agent_kind: str, session_id: str) -> None:
        with self._lock:
            sess = self._sessions.get(self._key(agent_kind, session_id))
            if sess is None:
                return
            sess.awaiting_kind = None
            sess.awaiting_context = []
            sess.awaiting_since_unix = 0
            sess.awaiting_summary = ""
            sess.awaiting_options = []

    def set_last_assistant_text(self, agent_kind: str, session_id: str, text: str) -> None:
        with self._lock:
            sess = self._sessions.get(self._key(agent_kind, session_id))
            if sess is None:
                return
            sess.last_assistant_text = text[:4000]

    def set_tool_in_flight(self, agent_kind: str, session_id: str, val: bool) -> None:
        with self._lock:
            sess = self._sessions.get(self._key(agent_kind, session_id))
            if sess is not None:
                sess.tool_in_flight = val

    def set_transcript_path(self, agent_kind: str, session_id: str, path: str) -> None:
        with self._lock:
            sess = self._sessions.get(self._key(agent_kind, session_id))
            if sess is not None and path:
                sess.transcript_path = path

    def drop_session(self, agent_kind: str, session_id: str) -> bool:
        """Remove a session (e.g. on SessionEnd). Returns True if one existed."""
        with self._lock:
            return self._sessions.pop(self._key(agent_kind, session_id), None) is not None

    def sweep_idle_turns(self, timeout_s: float) -> int:
        """Flip 'running' sessions that have gone silent (no tool in flight) for
        > timeout_s into AWAITING_CONTINUE ('your turn'). Covers user-interrupt
        (ESC) and mid-turn stalls, neither of which fires a Stop hook. Returns
        the number flipped (so the caller can push a fresh snapshot)."""
        if timeout_s <= 0:
            return 0          # sweep disabled
        now = int(time.time())
        flipped = 0
        with self._lock:
            for sess in self._sessions.values():
                if (sess.status == "running"
                        and not sess.tool_in_flight
                        and sess.awaiting_kind is None
                        and (now - sess.last_active_unix) > timeout_s):
                    # Still thinking? CC keeps writing the transcript while the
                    # model reasons (even with no tool calls). If it was touched
                    # within the window, the agent is working — do NOT flip.
                    tp = sess.transcript_path
                    if tp:
                        try:
                            if (now - os.path.getmtime(tp)) <= timeout_s:
                                continue
                        except OSError:
                            pass
                    sess.status = "waiting"
                    sess.awaiting_kind = "continue"
                    sess.awaiting_context = ["(idle — interrupted or stalled)"]
                    sess.awaiting_since_unix = now
                    flipped += 1
        return flipped

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
            # Byte length of the UTF-8 wire form (ensure_ascii=False, matching the
            # actual push) — CJK is multi-byte, so count encoded bytes vs the cap.
            # errors="replace": a lone surrogate anywhere in the data (e.g. a
            # mis-decoded event string) must never crash the publisher tick — it
            # would loop forever logging and never push, freezing the device.
            return len(json.dumps(snap, separators=(",", ":"),
                                  ensure_ascii=False).encode("utf-8", "replace"))

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

        # Step 5 (hard guarantee): Step 4 never drops a waiting agent, so >1
        # waiting agent could still leave the snapshot over the cap → the line
        # exceeds CONSOLE_MAX_LINE and the DEVICE rejects it ("line too long")
        # and freezes. As a last resort drop oldest agents regardless of status,
        # always keeping the most-recent waiting one (the takeover anchor) and
        # never going below a single agent, so the line always fits.
        while wire_size() > WIRE_MAX_BYTES and len(snap["agents"]) > 1:
            keep_idx = most_recent_waiting_idx()
            droppable = [(i, a) for i, a in enumerate(snap["agents"]) if i != keep_idx]
            if not droppable:
                break
            oldest_i, _ = min(droppable, key=lambda x: x[1].get("last_active_unix", 0))
            snap["agents"].pop(oldest_i)
        # Final safety: a lone agent still over cap (huge awaiting_summary etc.)
        # — shrink its variable-length text fields so the line can never exceed.
        if wire_size() > WIRE_MAX_BYTES and snap["agents"]:
            a = snap["agents"][0]
            for fld in ("awaiting_summary", "msg", "cwd"):
                while wire_size() > WIRE_MAX_BYTES and a.get(fld):
                    a[fld] = a[fld][:-16] if len(a[fld]) > 16 else ""
            a.pop("awaiting_options", None) if wire_size() > WIRE_MAX_BYTES else None
            a.pop("entries", None) if wire_size() > WIRE_MAX_BYTES else None

        # Totals were computed over the FULL session set above, but wire-trimming
        # may have dropped agents to fit CONSOLE_MAX_LINE. Recompute from the
        # agents the snapshot actually carries so it is self-consistent — else
        # the device shows e.g. waiting=2 while only one waiting agent is listed
        # (a status="waiting" agent without awaiting_kind is droppable in Step 4
        # yet was counted in totals.waiting).
        fa = snap["agents"]
        snap["totals"] = {
            "total":   len(fa),
            "running": sum(1 for a in fa if a.get("status") == "running"),
            "waiting": sum(1 for a in fa if a.get("status") == "waiting"),
            "tokens":       sum(a.get("tokens", 0) for a in fa),
            "tokens_today": sum(a.get("tokens_today", 0) for a in fa),
        }
        return snap

    def sweep_stale(self, idle_after_s: int = 300,
                    awaiting_idle_after_s: int = 900) -> int:
        """Drop dead sessions. A session with NO activity for `idle_after_s`
        (running/idle) — likely crashed/killed — is removed. An awaiting
        ('your turn') session is kept longer (`awaiting_idle_after_s`) so the
        user has time to attend to it, but is still dropped once clearly
        abandoned — otherwise a CC session that ended WITHOUT a SessionEnd hook
        (kill/crash) would, after the idle-turn flip, sit on the device as
        "your turn" forever and such corpses would accumulate. Reappears on the
        next prompt. Returns the number dropped."""
        now = int(time.time())
        dropped = 0
        with self._lock:
            stale = [
                k for k, s in self._sessions.items()
                if (now - s.last_active_unix) >
                   (awaiting_idle_after_s if s.awaiting_kind else idle_after_s)
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
_REPLY_RE = re.compile(r"reply\s+id=(\S+)\s+choice=(\d+)")


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
        mirror: Optional[str] = None,
    ) -> None:
        self.port_kind = port_kind
        self.port = port
        self.dry_run = dry_run
        self.health = health
        self.on_reconnect = on_reconnect
        # Optional one-way tap (HOST:PORT): every line written to the real
        # device is also copied here so a passive observer (the web serve.py)
        # can mirror exactly what the hardware screen receives. Best-effort —
        # never blocks or fails a real push.
        self._mirror_addr = mirror
        self._mirror_sock: Optional[socket.socket] = None
        self._port_arg = _resolve_port_arg(port_kind, port) if not dry_run else ""
        self._session: Optional[SessionHandle] = None
        self._lock = threading.Lock()
        self._timings: list[float] = []
        self._permission_waiters: dict[str, queue.Queue[str]] = {}
        self._reply_waiters: dict[str, queue.Queue[int]] = {}
        self._buffered_snapshot: Optional[dict] = None
        self._stop = threading.Event()
        self._reconnect_thread: Optional[threading.Thread] = None
        # Serial-open watchdog bookkeeping. Only ONE open attempt runs at a
        # time (single-flight); a hung open thread is abandoned but tracked so
        # we never stack a second blocking open on the same wedged port.
        self._open_thread: Optional[threading.Thread] = None
        self._open_lock = threading.Lock()

    def _mirror_write(self, line: str) -> None:
        """Best-effort tee of one line to the mirror tap. Never raises/blocks."""
        if not self._mirror_addr:
            return
        try:
            if self._mirror_sock is None:
                host, port_s = self._mirror_addr.rsplit(":", 1)
                self._mirror_sock = socket.create_connection((host, int(port_s)), timeout=0.5)
            self._mirror_sock.sendall((line + "\n").encode("utf-8"))
        except OSError:
            try:
                if self._mirror_sock:
                    self._mirror_sock.close()
            except OSError:
                pass
            self._mirror_sock = None   # reconnect on next push

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

    def _wire_session(self, session: SessionHandle) -> None:
        """Register our event subscribers on a freshly-opened session."""
        session.on_event(self._on_payload, kinds=frozenset({"payload"}))
        session.on_event(self._on_evt, kinds=frozenset({"evt"}))
        session.on_err(self._on_err)

    def _open_session(self) -> None:
        """Open a SessionHandle under a WATCHDOG and wire up subscribers.

        ``open_persistent_session`` → ``ser.open()`` can block forever on a
        wedged USB-Serial-JTAG port (it doesn't raise). We run it in a
        daemon thread and bound the wait to SERIAL_OPEN_TIMEOUT_S so a bad
        port can never freeze the caller (open_with_retry / reconnect loop).
        Single-flight: if a prior open is still hung we don't start another —
        we just report "still opening" so the retry loop backs off, and when
        the port finally unwedges that one thread adopts the session.

        Raises TransportError on timeout or open failure (same contract as
        before), so every existing ``except TransportError`` keeps working.
        """
        with self._open_lock:
            if self._open_thread is not None and self._open_thread.is_alive():
                raise TransportError("serial open still in progress (port wedged?)")
            result: dict = {}

            def _worker() -> None:
                try:
                    session = open_persistent_session(self._port_arg)
                    self._wire_session(session)
                except Exception as e:  # noqa: BLE001
                    result["error"] = e
                    return
                # Adopt only if nobody else already has a session and we're not
                # shutting down; otherwise this is a late completion from an
                # abandoned attempt — close it so we don't leak a port handle.
                if self._session is None and not self._stop.is_set():
                    self._session = session
                else:
                    try:
                        session.close()
                    except Exception:
                        pass

            t = threading.Thread(target=_worker, daemon=True, name="bridge-serial-open")
            self._open_thread = t
            t.start()

        t.join(SERIAL_OPEN_TIMEOUT_S)
        if t.is_alive():
            # Still blocked — abandon it (tracked via _open_thread so we won't
            # start a second) and let the caller retry later.
            raise TransportError(
                f"serial open timed out after {SERIAL_OPEN_TIMEOUT_S}s (port wedged?)")
        if "error" in result:
            raise TransportError(str(result["error"]))
        if self._session is None:
            raise TransportError("serial open produced no session")

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
            # ensure_ascii=False: send CJK as raw UTF-8, not \uXXXX escapes — the
            # device's tiny_json doesn't decode \u, and UTF-8 (3B/char) is shorter
            # on the wire than the escape (6B/char), easing the 1023B line cap.
            line = f'dash {cmd} "{json.dumps(payload, separators=(",", ":"), ensure_ascii=False)}"'
        # Single chokepoint for every device write:
        #  1. _device_safe — swap chars the device font can't render (✓ → • emoji
        #     traditional hanzi) for ASCII or drop them, so nothing shows as a box.
        #  2. _wire_safe   — guarantee UTF-8-encodable so no downstream .encode()
        #     (transport, mirror) can crash on a stray surrogate.
        # Structural JSON is ASCII and passes through untouched; only string
        # values change, so the line stays valid JSON.
        line = _wire_safe(_device_safe(line))
        if _DEBUG:
            flag = " OVERSIZE!" if len(line) > 1023 else ""
            print(f"[dbg] tx {cmd} len={len(line)}{flag}", file=sys.stderr, flush=True)
        self._mirror_write(line)
        if self.dry_run:
            print(f"[DRY] {line}", flush=True)
            return {"ok": True, "dry_run": True}
        started = time.monotonic()
        try:
            sess = self._session
            if sess is None or not sess.is_open:
                # NOT connected — never open the port INLINE here. ser.open()
                # can block for seconds (or forever on a wedged port), and this
                # push() runs on TCP-handler / publisher / health-poller threads
                # that must stay responsive. Hand opening to the background
                # reconnect loop (watchdog-bounded) and fail fast; the publisher
                # buffers the snapshot and the health poller retries in 5s.
                self.health.connected = False
                self._schedule_reconnect()
                return {"ok": False, "error": "not connected (reconnecting)"}
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
        """EVT lines arrive here. Extract permission decisions and reply
        choices, dispatch to the appropriate waiter queue."""
        m = _PERM_RE.search(evt.text)
        if m:
            req_id, decision = m.group(1), m.group(2)
            with self._lock:
                q = self._permission_waiters.get(req_id)
            if q is not None:
                try:
                    q.put_nowait(decision)
                except queue.Full:
                    pass
            return

        m = _REPLY_RE.search(evt.text)
        if m:
            req_id, choice = m.group(1), int(m.group(2))
            with self._lock:
                q = self._reply_waiters.get(req_id)
            if q is not None:
                try:
                    q.put_nowait(choice)
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
        """Keep trying to reopen the port until we succeed or the bridge stops.

        Persistent by design: a device unplugged for minutes must still be
        picked up the moment it comes back — a fixed deadline would give up and
        leave the screen dark until the next restart. The watchdog-bounded
        _open_session + the 2s backoff keep this from busy-spinning, and the
        single-flight guard means a wedged open never stacks up.
        """
        attempts = 0
        while not self._stop.is_set():
            attempts += 1
            try:
                if self._session is not None:
                    try:
                        self._session.close()
                    except Exception:
                        pass
                    # Must null out BEFORE opening: the watchdog worker only
                    # adopts a fresh session when _session is None.
                    self._session = None
                self._open_session()
                self.health.connected = True
                print(f"[bridge] reconnected (after {attempts} attempt(s))",
                      file=sys.stderr)
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
                self._stop.wait(2.0)

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

    # ── quick reply ───────────────────────────────────────────────────
    def push_reply_prompt(self, req_id: str, options: list[str],
                          *, timeout: float = 120.0) -> None:
        prompt = {
            "id": req_id,
            "mode": "reply",
            "tool": options[0][:32],
            "hint": options[1][:32] if len(options) > 1 else "",
        }
        q: queue.Queue[int] = queue.Queue(maxsize=1)
        with self._lock:
            self._reply_waiters[req_id] = q

        def _wait_and_copy():
            try:
                res = self.push("prompt", prompt)
                if not res.get("ok"):
                    return
                try:
                    choice = q.get(timeout=timeout)
                except queue.Empty:
                    return
                if choice < 0:
                    return
                text = options[choice] if choice < len(options) else ""
                if text:
                    try:
                        subprocess.run(
                            ["powershell", "-NoProfile", "-Command",
                             f"Set-Clipboard -Value '{text}'"],
                            check=True, timeout=3.0,
                            capture_output=True,
                        )
                        print(f"[bridge] reply copied to clipboard: {text[:40]}",
                              file=sys.stderr)
                    except Exception as e:
                        print(f"[bridge] clipboard write failed: {e}",
                              file=sys.stderr)
            finally:
                with self._lock:
                    self._reply_waiters.pop(req_id, None)

        threading.Thread(target=_wait_and_copy, daemon=True,
                         name=f"reply-{req_id}").start()

    def cancel_pending_replies(self) -> None:
        with self._lock:
            for q in self._reply_waiters.values():
                try:
                    q.put_nowait(-1)
                except queue.Full:
                    pass
            self._reply_waiters.clear()

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
        # When paused, no auto snapshots are pushed (health keepalive keeps
        # flowing). Used by the web "screen test driver" so a hand-pushed
        # `dash snapshot` UI combo stays on the device screen instead of being
        # overwritten by the live registry on the next keepalive/bump.
        self._paused = False

    @property
    def push_count(self) -> int:
        return self._push_count

    @property
    def paused(self) -> bool:
        return self._paused

    def pause(self) -> None:
        self._paused = True

    def resume(self) -> None:
        self._paused = False
        self._wake.set()

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
        if self._paused:
            # Idle while paused: push nothing, but keep the thread alive and
            # responsive to resume()/stop(). The registry still updates from
            # live hooks, so the latest state is pushed once we resume.
            self._wake.wait(timeout=0.2)
            self._wake.clear()
            return
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
        # Flip silent/interrupted turns → "your turn" (CC fires no hook on ESC),
        # and drop dead/abandoned sessions so corpses don't pile up. Both mutate
        # the registry, so the snapshot below reflects them and gets pushed.
        self.registry.sweep_idle_turns(IDLE_TURN_S)
        self.registry.sweep_stale()
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
    if agent not in _KNOWN_KINDS:
        agent = "other"
    out: dict[str, Any] = {
        "type": raw.get("type", "raw"),
        "agent": agent,
        # No pid fallback: a hook runs as a fresh process per event, so
        # falling back to its pid invented a NEW phantom session every time an
        # event arrived without a session_id (e.g. an occasional session-less
        # Stop) — and that phantom, stuck in awaiting='continue', kept the
        # device frozen on "your turn" forever. Unattributable events (empty
        # below) are dropped in Bridge._handle_inner instead.
        "session_id": (raw.get("session_id") or raw.get("transcript_path", "") or ""),
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
        # v2.4.0: dash-state block extracted by hook_dispatch.py.
        # Shape: {"summary": "...", "options": ["...", ...]} or None.
        "dash_state": raw.get("dash_state") or None,
        "transcript_path": raw.get("transcript_path", ""),
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
        gate_permissions: bool = False,
    ) -> None:
        self.registry = registry
        self.pusher = pusher
        self.publisher = publisher
        self.permission_timeout_s = permission_timeout_s
        # When False (default = "observe"), the dashboard does NOT gate tool
        # permissions — it just shows activity and lets Claude Code's own
        # permission prompt (in its terminal) handle approval. When True
        # ("gate"), a permission-required tool blocks here until the device/
        # browser answers (the approve/deny-via-device interaction feature).
        # Observe avoids stalling the agent ~60s on every dangerous-looking
        # command when nobody is watching the device buttons.
        self.gate_permissions = gate_permissions

    def handle(self, raw: dict) -> dict:
        if _DEBUG:
            global _rx_seq
            _rx_seq += 1
            t = raw.get("type")
            if t not in ("__dash__", "__pause__"):
                print(f"[dbg] rx#{_rx_seq} {t} "
                      f"{raw.get('agent','?')}:{str(raw.get('session_id',''))[:8]} "
                      f"tool={raw.get('tool_name','')}", file=sys.stderr, flush=True)
        try:
            return self._handle_inner(raw)
        except Exception as e:  # NEVER crash — bridge would take CC down with it
            print(f"[bridge] handle() crashed (caught): {e}", file=sys.stderr)
            return {"continue": True, "error": str(e)}

    def _handle_inner(self, raw: dict) -> dict:
        # Control messages from the web "screen test driver" (not hook events).
        # __dash__: push one raw `dash <cmd> [json]` line straight to the
        # connected device (real ESP32 on COM9 or the mock) so the UI can be
        # exercised with arbitrary state combinations. __pause__: freeze/unfreeze
        # the auto snapshot publisher so a hand-pushed combo stays on screen.
        ctl = raw.get("type")
        if ctl == "__dash__":
            res = self.pusher.push(raw.get("cmd", ""), raw.get("payload"))
            return {"ok": bool(res.get("ok") or res.get("dry_run")),
                    "sent": raw.get("cmd", ""), "push": res}
        if ctl == "__pause__":
            if raw.get("on"):
                self.publisher.pause()
            else:
                self.publisher.resume()
            return {"ok": True, "paused": self.publisher.paused}
        if ctl == "__ping__":
            # Liveness probe used by the single-instance guard: a starting bridge
            # asks "is one already running here?" An unambiguous reply (role +
            # pid) tells the newcomer to bow out.
            return {"ok": True, "role": "claude_buddy_bridge", "pid": os.getpid()}

        evt = normalize_event(raw)
        t = evt["type"]
        agent = evt["agent"]
        sid = evt["session_id"]

        if not sid:
            # Unattributable event (no session_id / transcript_path) — e.g. an
            # occasional session-less Stop. Ignore it: creating a session for it
            # would be a phantom that never clears and freezes the device on
            # "your turn". Real per-session events always carry a session_id.
            return {"continue": True}

        # Remember the transcript path so the idle-turn sweep can tell active
        # thinking (transcript still being written) from real idle/ESC. No-op
        # until the session exists (created by the first event below).
        if evt.get("transcript_path"):
            self.registry.set_transcript_path(agent, sid, evt["transcript_path"])

        if t == "session_start":
            # Session appeared → show it on the device immediately (idle, ball
            # in the user's court until the first prompt) instead of waiting for
            # the first tool call.
            self.registry.upsert(agent, sid, status="waiting",
                                 cwd=evt["cwd"] or None)
            self.registry.set_awaiting(agent, sid, kind="continue",
                                       context=["session started"])
            self.publisher.bump()
            return {"continue": True}

        if t == "session_end":
            # Session ended → remove it now rather than letting it linger until
            # the 300s stale-sweep.
            self.registry.drop_session(agent, sid)
            self.pusher.cancel_pending_replies()
            self.publisher.bump()
            return {"continue": True}

        if t == "user_prompt_submit":
            # User just submitted a new prompt → the agent moves out of
            # any "awaiting you" state. Clear awaiting_* fields so the
            # device leaves the AWAITING takeover and returns to ambient.
            self.registry.upsert(agent, sid, status="running",
                                 cwd=evt["cwd"] or None,
                                 msg=evt["summary"])
            self.registry.clear_awaiting(agent, sid)
            self.registry.set_tool_in_flight(agent, sid, False)
            self.pusher.cancel_pending_replies()
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
            # A tool is now in flight (may run for minutes) — exempt from the
            # idle-turn sweep until PostToolUse clears it.
            self.registry.set_tool_in_flight(agent, sid, True)
            if looks_like_permission_required(evt) and self.gate_permissions:
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
            tool_name = evt["tool_name"] or "tool"
            summary = evt["summary"] or "ok"
            self.registry.upsert(agent, sid, status="running",
                                 tool=tool_name,
                                 summary=summary,
                                 tokens=evt["tokens"])
            self.registry.set_tool_in_flight(agent, sid, False)
            self.publisher.bump()
            # v2.7.0: top-slide-down banner on device
            hint = summary[:40] if summary != "ok" else ""
            self.pusher.push("push", {"tool": tool_name, "hint": hint})
            return {"continue": True}

        if t == "stop":
            # v2.3.0: Stop = "ball in user's court". Status moves to
            # `waiting` (not `idle`), and the classifier picks the
            # AWAITING kind from the assistant's last message so the
            # device's takeover scene shows the right headline + glyph.
            # v2.4.0: if the agent included a <dash-state> block, the
            # summary + options enrich the takeover (marquee + numbered
            # option list). dash_state.summary overrides the classifier's
            # default context lines so the user sees the agent's own
            # framing rather than the heuristic guess.
            kind, ctx = classify_awaiting(
                "stop",
                evt.get("last_assistant_text", ""),
            )
            ds = evt.get("dash_state") or {}
            summary = (ds.get("summary") or "")[:240]
            options = [o[:32] for o in (ds.get("options") or [])][:4]
            # If summary is present, use it as the single ctx line for
            # backwards-compat (devices that don't parse summary still
            # see something useful). The device's scene_awaiting will
            # prefer summary when present.
            if summary:
                ctx = [summary[:48]]
            # Options (if any) are now DISPLAY-ONLY: the device shows them as a
            # numbered list ("pick") but does NOT round-trip a choice back. The
            # old 2-option path pushed an interactive reply-prompt + wrote the
            # picked text to the clipboard — that was a device-side synchronous
            # interaction, which we've stopped. The device is a one-way status
            # mirror; the user acts in their own terminal, not on the device.
            if len(options) >= 2:
                kind = "pick"
            # tokens for this turn come from the transcript usage (hook_dispatch
            # fills evt["tokens"] on Stop) → accumulates into tokens_today.
            self.registry.upsert(agent, sid, status="waiting", tokens=evt["tokens"])
            self.registry.set_tool_in_flight(agent, sid, False)
            self.registry.set_awaiting(
                agent, sid,
                kind=kind, context=ctx,
                summary=summary, options=options,
            )
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
    mirror: Optional[str] = None
    gate_permissions: bool = False

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
        mirror=getattr(args, "mirror", None),
        gate_permissions=bool(getattr(args, "gate_permissions", False)),
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
        mirror=settings.mirror,
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
        gate_permissions=settings.gate_permissions,
    )
    return bridge, pusher, publisher, registry, health, setup_state


# ─────────────────────────────────────────────────────────────────────────────
# Subcommand: serve
# ─────────────────────────────────────────────────────────────────────────────

def bridge_already_running(listen: str, *, timeout: float = 1.0) -> bool:
    """Single-instance guard: return True if a live bridge already answers on
    *listen*. Used so an auto-started duplicate (hook_dispatch spawns one per
    "bridge offline" detection) bows out instead of fighting over the port and
    the serial handle. Cheap __ping__ round-trip; any error → assume none."""
    try:
        host, port_s = listen.split(":")
        with socket.create_connection((host, int(port_s)), timeout=timeout) as sock:
            sock.sendall(b'{"type":"__ping__"}\n')
            sock.settimeout(timeout)
            line = sock.makefile("r", encoding="utf-8").readline()
        obj = json.loads(line or "{}")
        return obj.get("role") == "claude_buddy_bridge"
    except (OSError, ValueError, json.JSONDecodeError):
        return False


def cmd_serve(args) -> int:
    settings = build_settings(args)

    # Single-instance guard. hook_dispatch auto-starts a bridge whenever it
    # can't reach one; several concurrent hooks (or a manual run on top of an
    # auto-started one) would otherwise race for COM9. If a live bridge already
    # owns this listen address, step aside cleanly.
    if bridge_already_running(settings.listen):
        print(f"[bridge] another instance already serving {settings.listen}; "
              f"exiting", flush=True)
        return 0

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
    p.add_argument("--mirror", default=None, metavar="HOST:PORT",
                   help="also copy every pushed line to this TCP tap (web mirror)")
    p.add_argument("--gate-permissions", action="store_true",
                   help="block tool calls until approved via device/browser "
                        "(default: observe — let Claude Code's own prompt gate, "
                        "so the agent never stalls waiting on the dashboard)")


def main(argv: list[str] | None = None) -> int:
    # Events (and `send`/`--stdin` JSONL) are UTF-8. On a non-UTF-8 locale
    # (Chinese Windows cp936) the default stdin/stdout decode mangles CJK into
    # lone surrogates, which then crash snapshot serialization downstream. Pin
    # UTF-8 so payloads survive verbatim. Guarded for stub streams (tests).
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

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
                        help="override agent (claude-code|codex|cursor|aider|windsurf|copilot|qwen-code)")
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
