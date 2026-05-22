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
