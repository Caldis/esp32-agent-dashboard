"""codex_wrapper.py — wrap a ``codex exec`` invocation and tee its stream
to the bridge daemon.

As of Codex CLI 0.x there's no first-party hook system equivalent to
Claude Code's, but ``codex exec`` already emits structured JSONL when
invoked with ``--output-format json`` (or whatever the current flag is —
check ``codex exec --help``). This wrapper:

1. Spawns the real codex process with the same args you'd pass.
2. Reads its stdout line-by-line; tries to parse each line as JSON.
3. Forwards a normalized event to the bridge for each line that looks
   like a tool event.
4. Re-emits the original stdout to the caller's stdout (so codex behaves
   transparently when invoked from a script).

Usage:
    python codex_wrapper.py -- codex exec "explain this code"
    python codex_wrapper.py --bridge 127.0.0.1:7321 -- codex exec ...

If codex emits non-JSON or a format we don't recognize, we still pass
the output through; only the bridge events are best-effort.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import uuid

DEFAULT_BRIDGE = os.environ.get("CLAUDE_BUDDY_BRIDGE", "127.0.0.1:7321")


def _send(addr: str, payload: dict) -> None:
    host, port_s = addr.split(":")
    try:
        with socket.create_connection((host, int(port_s)), timeout=1.0) as sock:
            sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
            # Ignore response.
    except OSError:
        pass


def _classify(obj: dict, session_id: str) -> dict | None:
    """Map a codex-stream object into the bridge's normalized schema.

    Codex emits things like:
        {"type": "agent_message", "message": "..."}
        {"type": "tool_call", "name": "shell", "args": {"command": "..."}}
        {"type": "tool_result", "name": "shell", "ok": true}
        {"type": "token_count", "input": 12, "output": 34}
        {"type": "task_complete"}
    The exact names can drift; this function is intentionally lenient.
    """
    t = obj.get("type", "")
    if t in ("agent_message", "message", "assistant"):
        return {
            "type": "assistant_event",
            "agent": "codex",
            "session_id": session_id,
            "text": obj.get("message") or obj.get("content") or "",
            "summary": (obj.get("message") or obj.get("content") or "")[:60],
        }
    if t in ("tool_call", "tool_use"):
        return {
            "type": "pre_tool_use",
            "agent": "codex",
            "session_id": session_id,
            "tool_name": obj.get("name") or obj.get("tool") or "shell",
            "tool_input": obj.get("args") or obj.get("input") or {},
        }
    if t in ("tool_result", "tool_output"):
        return {
            "type": "post_tool_use",
            "agent": "codex",
            "session_id": session_id,
            "tool_name": obj.get("name") or obj.get("tool") or "shell",
            "summary": f"{obj.get('name', 'tool')} {'ok' if obj.get('ok') else 'err'}",
        }
    if t in ("token_count", "usage"):
        n = int(obj.get("input", 0)) + int(obj.get("output", 0)) + int(obj.get("tokens", 0))
        return {
            "type": "tokens",
            "agent": "codex",
            "session_id": session_id,
            "tokens": n,
        }
    if t in ("task_complete", "stop", "done"):
        return {
            "type": "stop",
            "agent": "codex",
            "session_id": session_id,
        }
    return None


def main() -> int:
    parser = argparse.ArgumentParser(prog="codex_wrapper")
    parser.add_argument("--bridge", default=DEFAULT_BRIDGE,
                        help="bridge HOST:PORT (default 127.0.0.1:7321)")
    parser.add_argument("--session-id", default=None,
                        help="override session id (default: random uuid)")
    parser.add_argument("rest", nargs=argparse.REMAINDER,
                        help="-- followed by the codex command to run")
    args = parser.parse_args()

    cmd = args.rest
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        parser.error("missing codex command. usage: codex_wrapper.py -- codex exec ...")

    session_id = args.session_id or f"cx_{uuid.uuid4().hex[:8]}"

    # Notify session start.
    _send(args.bridge, {
        "type": "user_prompt_submit",
        "agent": "codex",
        "session_id": session_id,
        "summary": " ".join(cmd)[:60],
    })

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=1,
        text=True,
    )

    def _consume(stream, out_stream):
        for line in stream:
            out_stream.write(line)
            out_stream.flush()
            line = line.rstrip("\r\n")
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            evt = _classify(obj, session_id)
            if evt is not None:
                _send(args.bridge, evt)

    t_out = threading.Thread(target=_consume, args=(proc.stdout, sys.stdout),
                             daemon=True)
    t_err = threading.Thread(target=_consume, args=(proc.stderr, sys.stderr),
                             daemon=True)
    t_out.start(); t_err.start()
    rc = proc.wait()
    t_out.join(timeout=1); t_err.join(timeout=1)

    _send(args.bridge, {
        "type": "stop",
        "agent": "codex",
        "session_id": session_id,
        "summary": f"exit {rc}",
    })
    return rc


if __name__ == "__main__":
    sys.exit(main())
