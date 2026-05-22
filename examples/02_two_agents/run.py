#!/usr/bin/env python3
"""02_two_agents — two concurrent agents, live-toggling between them.

Demonstrates the v1 `(kind, session_id)` slot model:

  * a claude-code agent (`cc_main`) running
  * a codex agent (`cx_aux`) running alongside it
  * snapshots are pushed at ~2 Hz; every other snapshot one of the two
    flips between `running` and `waiting`, so on a real device the
    sessions scene visibly shimmers between the two agents

Run against the bundled TCP mock:

    # terminal A
    python tools/mock_device_v1.py --port 9876 -v

    # terminal B
    python examples/02_two_agents/run.py --duration 6

Exit code 0 on a clean walk, 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time


def send_command(sock: socket.socket, verb: str, payload: dict | None = None) -> None:
    if payload is None:
        line = f"dash {verb}\n"
    else:
        body = json.dumps(payload, separators=(",", ":"))
        line = f'dash {verb} "{body}"\n'
    sock.sendall(line.encode("utf-8"))


def drain_replies(sock: socket.socket, timeout_s: float = 0.2) -> list[str]:
    """Read whatever the device has queued up, then return."""
    sock.settimeout(timeout_s)
    buf = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
    except (socket.timeout, TimeoutError):
        pass
    lines = [l for l in buf.decode("utf-8", errors="replace").split("\n") if l]
    return lines


def build_snapshot(tick: int) -> dict:
    """Build a snapshot where the two agents toggle status every tick.

    The device uses `(kind, session_id)` as the slot key — so even though
    the order of `agents[]` shifts between ticks, the device keeps each
    agent pinned to its own row. session_id is the load-bearing field.
    """
    cc_running = (tick % 2 == 0)
    cx_running = not cc_running
    now = int(time.time())
    cc = {
        "kind": "claude-code",
        "session_id": "cc_main",
        "status": "running" if cc_running else "waiting",
        "cwd": "D:\\Code\\firmware",
        "msg": f"editing main.c (tick={tick})",
        "entries": [
            {"t": "10:42", "tool": "Bash", "summary": "git diff"},
            {"t": "10:41", "tool": "Edit", "summary": "main.c (+8 -2)"},
            {"t": "10:39", "tool": "Read", "summary": "main.c (120 lines)"},
        ],
        "tokens": 84500 + tick * 100,
        "tokens_today": 21200 + tick * 100,
        "last_active_unix": now if cc_running else now - 3,
        "prompt": None,
    }
    cx = {
        "kind": "codex",
        "session_id": "cx_aux",
        "status": "running" if cx_running else "waiting",
        "cwd": "D:\\Code\\notes",
        "msg": f"summarising README (tick={tick})",
        "entries": [
            {"t": "10:30", "tool": "Grep", "summary": "login (42 hits)"},
        ],
        "tokens": 12300 + tick * 40,
        "tokens_today": 12300 + tick * 40,
        "last_active_unix": now if cx_running else now - 3,
        "prompt": None,
    }
    # Alternate the order in `agents[]` to prove the device doesn't rely
    # on positional indexing.
    agents = [cc, cx] if tick % 2 == 0 else [cx, cc]
    running = sum(1 for a in agents if a["status"] == "running")
    waiting = sum(1 for a in agents if a["status"] == "waiting")
    return {
        "agents": agents,
        "totals": {
            "total": 2, "running": running, "waiting": waiting,
            "tokens": cc["tokens"] + cx["tokens"],
            "tokens_today": cc["tokens_today"] + cx["tokens_today"],
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9876)
    ap.add_argument("--duration", type=float, default=4.0,
                    help="seconds to keep pushing snapshots (default 4)")
    ap.add_argument("--interval", type=float, default=0.5,
                    help="seconds between snapshots (default 0.5)")
    args = ap.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5.0) as sock:
        print(f"connected to {args.host}:{args.port}")
        print(f"pushing snapshots every {args.interval}s for {args.duration}s\n")

        # Park on idle first so the device starts from a clean slate.
        send_command(sock, "idle")
        drain_replies(sock)

        deadline = time.time() + args.duration
        tick = 0
        while time.time() < deadline:
            snap = build_snapshot(tick)
            running = snap["totals"]["running"]
            waiting = snap["totals"]["waiting"]
            print(f"tick {tick:>2} | running={running} waiting={waiting} "
                  f"| order={[a['kind'] for a in snap['agents']]}")
            send_command(sock, "snapshot", snap)
            replies = drain_replies(sock, timeout_s=args.interval)
            for r in replies:
                if r.startswith("ERR"):
                    print(f"  device error: {r}", file=sys.stderr)
                    return 1
            tick += 1

        # Drop the agents — bridge would do this when both Stop hooks fire.
        print("\nfinal: park back on idle (both agents stopped)")
        send_command(sock, "idle")
        drain_replies(sock, timeout_s=0.5)

    print(f"\nOK — pushed {tick} snapshots across 2 agents.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ConnectionRefusedError, OSError) as e:
        print(f"connection failed: {e}", file=sys.stderr)
        print("hint: start the mock first — `python tools/mock_device_v1.py --port 9876 -v`",
              file=sys.stderr)
        sys.exit(2)
    except Exception as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
