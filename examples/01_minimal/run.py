#!/usr/bin/env python3
"""01_minimal — the smallest possible v1 protocol consumer.

Walks the device through three states:

    idle   →   sessions (one agent)   →   idle

Reads `OK:` lines back so you can see exactly what the device acknowledges.

Run against the bundled TCP mock:

    # terminal A
    python tools/mock_device_v1.py --port 9876 -v

    # terminal B
    python examples/01_minimal/run.py

Exit code 0 on a clean walk, 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time


# Line framing: the device speaks one JSON-bearing command per line.
# `dash <verb> "<json>"` — the *json* MUST be a single quoted token so the
# device-side tokeniser (G-7) keeps the inner quotes intact.
def send_command(sock: socket.socket, verb: str, payload: dict | None = None) -> None:
    if payload is None:
        line = f"dash {verb}\n"
    else:
        # Encode the JSON, then wrap in `"..."`. Inner `"` survive because
        # the device's quote-leading tokeniser scans for `"`-followed-by-ws.
        body = json.dumps(payload, separators=(",", ":"))
        line = f'dash {verb} "{body}"\n'
    sock.sendall(line.encode("utf-8"))
    print(f"  --> {line.rstrip()}")


def read_one_reply(sock: socket.socket, timeout_s: float = 2.0) -> str:
    sock.settimeout(timeout_s)
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    line, _, _ = buf.partition(b"\n")
    text = line.decode("utf-8", errors="replace")
    print(f"  <-- {text}")
    return text


def expect_ok(reply: str) -> None:
    if not reply.startswith("OK:"):
        raise RuntimeError(f"expected OK, got: {reply!r}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9876)
    args = ap.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5.0) as sock:
        print(f"connected to {args.host}:{args.port}\n")

        # 1) Park the device on idle. This is what you should do at startup
        #    so an old screen doesn't linger if the bridge restarts.
        print("step 1: park on idle scene")
        send_command(sock, "idle")
        expect_ok(read_one_reply(sock))
        time.sleep(0.2)

        # 2) Push a single-agent snapshot. The device picks the `sessions`
        #    scene because `agents[]` is non-empty.
        #
        #    Field cheat-sheet (see PROTOCOL.md for the full grammar):
        #      kind          colour palette key (claude-code | codex | other)
        #      session_id    stable per-agent identity; (kind, session_id) is
        #                    the slot the device uses to keep agents distinct
        #      status        running | waiting | idle — drives the status pip
        #      entries[]     the last few transcript lines (newest first)
        #      tokens        cumulative for this session
        #      tokens_today  cumulative across today (across all sessions of
        #                    this kind on this host)
        #      prompt        if non-null, device jumps to the prompt scene
        print("\nstep 2: snapshot with a single running claude-code agent")
        snapshot = {
            "agents": [
                {
                    "kind": "claude-code",
                    "session_id": "cc_demo",
                    "status": "running",
                    "cwd": "D:\\Code\\hello-world",
                    "msg": "writing a tiny example",
                    "entries": [
                        {"t": "10:42", "tool": "Edit", "summary": "main.py (+5 -0)"},
                        {"t": "10:41", "tool": "Read", "summary": "main.py (42 lines)"},
                    ],
                    "tokens": 1234,
                    "tokens_today": 1234,
                    "last_active_unix": int(time.time()),
                    "prompt": None,
                }
            ],
            "totals": {
                "total": 1, "running": 1, "waiting": 0,
                "tokens": 1234, "tokens_today": 1234,
            },
        }
        send_command(sock, "snapshot", snapshot)
        expect_ok(read_one_reply(sock))
        time.sleep(0.5)

        # 3) Park the device back on idle. In the real bridge this is
        #    what `Stop` hooks ultimately trigger when there are no
        #    running agents left.
        print("\nstep 3: park back on idle")
        send_command(sock, "idle")
        expect_ok(read_one_reply(sock))

    print("\nOK — minimal round-trip complete.")
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
