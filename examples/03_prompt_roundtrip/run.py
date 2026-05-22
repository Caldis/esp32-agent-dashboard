#!/usr/bin/env python3
"""03_prompt_roundtrip — send a `dash prompt`, wait for the decision.

This is the most interesting protocol round-trip: the host asks "should I
let the agent run `rm -rf /tmp/foo`?", the device renders the prompt
scene, and a human (or, against the mock, the auto-decide timer) presses
BOOT (approve once) or USER (deny). The decision arrives as an unsolicited
`EVT: permission id=<id> decision=<once|deny>` line.

Against the mock, the decision fires after `--decision-delay-ms` ms
(default 500). Against a real board, it fires when you press a button or
the 60 s timeout elapses.

Run:

    # terminal A
    python tools/mock_device_v1.py --port 9876 -v --decision-delay-ms 800

    # terminal B
    python examples/03_prompt_roundtrip/run.py

Exit code 0 if the decision matches what we requested, 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import uuid


def send_command(sock: socket.socket, verb: str, payload: dict | None = None) -> None:
    if payload is None:
        line = f"dash {verb}\n"
    else:
        body = json.dumps(payload, separators=(",", ":"))
        line = f'dash {verb} "{body}"\n'
    sock.sendall(line.encode("utf-8"))


def read_until(
    sock: socket.socket,
    predicate,
    *,
    timeout_s: float = 10.0,
) -> str | None:
    """Read line-by-line until predicate(line) is True, or timeout."""
    deadline = time.time() + timeout_s
    buf = b""
    while time.time() < deadline:
        sock.settimeout(max(0.05, deadline - time.time()))
        try:
            chunk = sock.recv(4096)
        except (socket.timeout, TimeoutError):
            continue
        if not chunk:
            return None
        buf += chunk
        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            text = line.decode("utf-8", errors="replace")
            print(f"  <-- {text}")
            if predicate(text):
                return text
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9876)
    ap.add_argument("--tool", default="Bash",
                    help="tool name the prompt is asking about")
    ap.add_argument("--hint", default="rm -rf /tmp/foo",
                    help="exact command preview shown on the device")
    ap.add_argument("--decision-timeout", type=float, default=10.0,
                    help="seconds to wait for the EVT permission line")
    args = ap.parse_args()

    prompt_id = f"req_{uuid.uuid4().hex[:8]}"

    with socket.create_connection((args.host, args.port), timeout=5.0) as sock:
        print(f"connected to {args.host}:{args.port}\n")

        # 1) Send the prompt request. The device flips to the prompt scene
        #    and starts its 60 s timeout countdown.
        prompt = {
            "id": prompt_id,
            "tool": args.tool,
            "hint": args.hint,
            "agent_kind": "claude-code",
            "session_id": "cc_demo",
        }
        print(f"step 1: send prompt id={prompt_id} tool={args.tool!r}")
        print(f"        hint={args.hint!r}")
        send_command(sock, "prompt", prompt)

        # 2) The OK: line confirms the device parked on the prompt scene.
        ok = read_until(sock, lambda l: l.startswith("OK:") or l.startswith("ERR:"),
                        timeout_s=2.0)
        if ok is None or not ok.startswith("OK:"):
            print(f"FAIL: no OK for prompt (got {ok!r})", file=sys.stderr)
            return 1

        # 3) Now block for the decision. Against the mock this fires within
        #    ~500 ms; against a real board, until the human presses BOOT or
        #    USER, or the device times out (60 s) and falls through to
        #    `deny`.
        print(f"\nstep 2: waiting up to {args.decision_timeout}s for "
              "EVT: permission ...")
        evt = read_until(
            sock,
            lambda l: l.startswith(f"EVT: permission id={prompt_id}"),
            timeout_s=args.decision_timeout,
        )
        if evt is None:
            print("FAIL: timed out waiting for decision", file=sys.stderr)
            return 1

        # 4) Parse the decision out of the EVT line.
        #    EVT: permission id=<id> decision=<once|deny> [session_id=<id>]
        decision = None
        for part in evt.split():
            if part.startswith("decision="):
                decision = part.split("=", 1)[1]
                break
        if decision not in ("once", "deny"):
            print(f"FAIL: unparseable decision in {evt!r}", file=sys.stderr)
            return 1

        print(f"\nstep 3: decision = {decision!r}")
        print("(in the real bridge, this is where the PreToolUse hook unblocks)")

        # 5) Park back on idle. In the real bridge, the next snapshot
        #    would do this implicitly when the agent moves past the
        #    blocked tool.
        send_command(sock, "idle")
        read_until(sock, lambda l: l.startswith("OK:"), timeout_s=2.0)

    print(f"\nOK — prompt round-trip complete (decision={decision}).")
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
