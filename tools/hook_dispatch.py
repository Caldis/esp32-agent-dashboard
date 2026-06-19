"""hook_dispatch.py — tiny stdin→bridge forwarder for Claude Code hooks.

Claude Code's hook config points at this script with one positional arg
(the event type, e.g. ``pre_tool_use``). We read the hook payload from
stdin, forward it to the long-running ``claude_buddy_bridge serve``
daemon over loopback TCP, and pipe the daemon's JSON response back to
stdout (which Claude Code reads).

If the bridge isn't running we MUST NOT block Claude Code — we print
``{"continue": true}`` and exit 0.

Why a separate script: ``hooks.command`` is invoked per event, so we want
this hook to be small, dependency-free, and fast (under a few ms in the
common case).
"""

from __future__ import annotations

import json
import os
import re
import socket
import sys

DEFAULT_HOST = os.environ.get("CLAUDE_BUDDY_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("CLAUDE_BUDDY_PORT", "7321"))
# v1: 5s for non-PreToolUse events (never block CC for longer); the bridge
# itself replies within ~1ms in the common case.
DEFAULT_TIMEOUT = float(os.environ.get("CLAUDE_BUDDY_TIMEOUT", "5.0"))
# v1: PreToolUse permission round-trip can take up to 60 s (user has to
# physically tap a button on the device, default permission-timeout-s).
PROMPT_TIMEOUT = float(os.environ.get("CLAUDE_BUDDY_PROMPT_TIMEOUT", "60.0"))

# v2.7.0 circuit breaker: N consecutive timeouts in W seconds → skip for M seconds.
CB_THRESHOLD = int(os.environ.get("CLAUDE_BUDDY_CB_THRESHOLD", "3"))
CB_WINDOW_S = float(os.environ.get("CLAUDE_BUDDY_CB_WINDOW", "30.0"))
CB_COOLDOWN_S = float(os.environ.get("CLAUDE_BUDDY_CB_COOLDOWN", "60.0"))
_CB_STATE_FILE = os.path.join(
    os.environ.get("TEMP", os.environ.get("TMPDIR", "/tmp")),
    "claude_buddy_cb.json",
)


_DBG_FLAG = os.path.join(
    os.environ.get("TEMP", os.environ.get("TMPDIR", "/tmp")),
    "hook_dispatch_debug.on",
)
_DBG_LOG = os.path.join(
    os.environ.get("TEMP", os.environ.get("TMPDIR", "/tmp")),
    "hook_dispatch_debug.log",
)


def _dlog(msg: str) -> None:
    """Append a diagnostic line iff the sentinel file exists. Off by default
    (no overhead beyond one stat), so safe to ship — create the .on file to
    trace dropped hook events end-to-end, delete it to stop."""
    try:
        if not os.path.exists(_DBG_FLAG):
            return
        import time as _t
        with open(_DBG_LOG, "a", encoding="utf-8") as f:
            f.write(f"{_t.time():.3f} pid={os.getpid()} {msg}\n")
    except OSError:
        pass


def _passthrough(reason: str = "") -> int:
    """Always-safe fallback response if the bridge is unreachable."""
    out: dict = {"continue": True}
    if reason:
        out["systemMessage"] = reason
    print(json.dumps(out))
    return 0


import time as _time

def _cb_load() -> dict:
    try:
        with open(_CB_STATE_FILE, "r") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError, ValueError):
        return {"timestamps": [], "open_until": 0.0}


def _cb_save(state: dict) -> None:
    try:
        with open(_CB_STATE_FILE, "w") as f:
            json.dump(state, f)
    except OSError:
        pass


def _cb_is_open() -> bool:
    """Return True if circuit is open (should short-circuit)."""
    state = _cb_load()
    return _time.time() < state.get("open_until", 0.0)


def _cb_record_timeout() -> None:
    """Record a timeout and trip the breaker if threshold reached."""
    now = _time.time()
    state = _cb_load()
    cutoff = now - CB_WINDOW_S
    ts = [t for t in state.get("timestamps", []) if t > cutoff]
    ts.append(now)
    if len(ts) >= CB_THRESHOLD:
        state["open_until"] = now + CB_COOLDOWN_S
        ts = []
    state["timestamps"] = ts
    _cb_save(state)


def _cb_record_success() -> None:
    """Clear timeout history on a successful round-trip."""
    state = _cb_load()
    if state.get("timestamps") or state.get("open_until", 0.0) > 0:
        _cb_save({"timestamps": [], "open_until": 0.0})


# ── dash-state extraction (v2.4.0 contract) ──────────────────────────
#
# When CC fires `Stop`, we read the transcript file and extract the
# `<dash-state>` block from the last assistant message. The block lets
# the agent supply a marquee `summary` + 2-4 `options` for the device's
# AWAITING takeover. See docs/DASH_STATE_CONTRACT.md.

_DASH_STATE_RE = re.compile(
    r"<dash-state>\s*(?P<body>.*?)\s*</dash-state>\s*$",
    re.DOTALL,
)


def _extract_dash_state(text: str) -> dict | None:
    """Return {summary, options} or None if no block / parse fails."""
    if not text:
        return None
    m = _DASH_STATE_RE.search(text)
    if m is None:
        return None
    body = m.group("body")
    summary = ""
    options: list[str] = []
    in_options = False
    for raw in body.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        if not in_options:
            # Look for `summary: ...` (case-insensitive on the key)
            if line.lower().startswith("summary:"):
                summary = line.split(":", 1)[1].strip()
                continue
            if line.lower().startswith("options:"):
                in_options = True
                continue
            # Tolerate freeform lines before `options:` only as summary
            # continuation if summary not yet set.
            if not summary:
                summary = line.strip()
        else:
            # Strip bullet markers `-` `*` `•`
            stripped = line.lstrip()
            if stripped.startswith(("-", "*", "•")):
                opt = stripped[1:].strip()
                if opt:
                    options.append(opt[:64])  # cap to 64; bridge re-caps to 32
                    if len(options) >= 4:
                        break
    if not summary and not options:
        return None
    return {
        "summary": summary[:240],
        "options": options[:4],
    }


def _read_last_assistant_text(transcript_path: str) -> str:
    """Best-effort: pull the most recent assistant message text from
    a CC transcript JSONL. Falls back to empty string on any error."""
    if not transcript_path or not os.path.exists(transcript_path):
        return ""
    try:
        # Read backwards is cleaner but expensive on huge files; small
        # CC transcripts fit in memory.
        with open(transcript_path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return ""
    for ln in reversed(lines):
        ln = ln.strip()
        if not ln:
            continue
        try:
            rec = json.loads(ln)
        except json.JSONDecodeError:
            continue
        role = rec.get("role") or rec.get("type")
        if role != "assistant":
            continue
        content = rec.get("content") or rec.get("text") or ""
        if isinstance(content, list):
            parts = []
            for block in content:
                if isinstance(block, dict):
                    t = block.get("text") or block.get("content") or ""
                    if isinstance(t, str):
                        parts.append(t)
            content = "\n".join(parts)
        if isinstance(content, str) and content:
            return content
    return ""


def main(argv: list[str]) -> int:
    event_type = argv[1] if len(argv) > 1 else "raw"
    agent = argv[2] if len(argv) > 2 else "claude-code"

    raw = sys.stdin.read().strip()
    try:
        payload = json.loads(raw) if raw else {}
    except json.JSONDecodeError:
        payload = {"text": raw}

    payload.setdefault("type", event_type)
    payload.setdefault("agent", agent)

    # Tag with pid + cwd for session disambiguation when Claude Code doesn't
    # provide session_id (older versions / non-standard runners).
    payload.setdefault("pid", os.getpid())
    payload.setdefault("cwd", os.getcwd())

    # v2.4.0: enrich Stop events with the assistant's last text + any
    # <dash-state> block they appended. The bridge classifier uses the
    # full text; the AWAITING takeover uses summary + options if present.
    if event_type == "stop":
        transcript_path = payload.get("transcript_path") or ""
        last_text = _read_last_assistant_text(transcript_path)
        if last_text and "last_assistant_text" not in payload:
            payload["last_assistant_text"] = last_text
        ds = _extract_dash_state(last_text)
        if ds:
            payload["dash_state"] = ds

    timeout = PROMPT_TIMEOUT if event_type == "pre_tool_use" else DEFAULT_TIMEOUT

    sid = str(payload.get("session_id", ""))[:10]
    if _cb_is_open():
        _dlog(f"{event_type} {sid} CB_OPEN -> drop")
        return _passthrough("circuit breaker open — bridge skipped")

    try:
        with socket.create_connection((DEFAULT_HOST, DEFAULT_PORT),
                                      timeout=1.0) as sock:
            sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
            sock.settimeout(timeout)
            line = sock.makefile("r", encoding="utf-8").readline()
            _cb_record_success()
            _dlog(f"{event_type} {sid} -> {DEFAULT_HOST}:{DEFAULT_PORT} "
                  f"resp={'EMPTY' if not line.strip() else line.strip()[:40]}")
            print(line.strip() or json.dumps({"continue": True}))
            return 0
    except socket.timeout:
        _cb_record_timeout()
        _dlog(f"{event_type} {sid} TIMEOUT({timeout}s) -> drop")
        return _passthrough("claude_buddy_bridge timeout")
    except (ConnectionRefusedError, OSError) as e:
        _dlog(f"{event_type} {sid} CONN_ERR {e} -> drop")
        return _passthrough(f"claude_buddy_bridge offline: {e}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
