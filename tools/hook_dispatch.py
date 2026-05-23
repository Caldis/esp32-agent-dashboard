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


def _passthrough(reason: str = "") -> int:
    """Always-safe fallback response if the bridge is unreachable."""
    out: dict = {"continue": True}
    if reason:
        out["systemMessage"] = reason
    print(json.dumps(out))
    return 0


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

    try:
        # Short connect timeout so a dead bridge doesn't stall CC.
        with socket.create_connection((DEFAULT_HOST, DEFAULT_PORT),
                                      timeout=1.0) as sock:
            sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
            sock.settimeout(timeout)
            line = sock.makefile("r", encoding="utf-8").readline()
            print(line.strip() or json.dumps({"continue": True}))
            return 0
    except (ConnectionRefusedError, socket.timeout, OSError) as e:
        return _passthrough(f"claude_buddy_bridge offline: {e}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
