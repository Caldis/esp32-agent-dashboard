#!/usr/bin/env python3
"""
mock_device.py — TCP stand-in for the ESP32 dashboard device.

Speaks the same `dash <verb> <json>` wire protocol the firmware speaks
over USB-Serial, but over TCP. Lets the bridge be tested without
COM9 access (CI, parallel dev, regression).

Usage:
    python docs/mock_device.py --port 9876
    # then run the bridge with --port-kind tcp --port 127.0.0.1:9876

Tokeniser mirrors components/aurora-harness/src/console_protocol.c
exactly (whitespace-split + double-quote runs) — anything that talks
to mock_device.py correctly will talk to the real device correctly.
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time
from dataclasses import dataclass, field


@dataclass
class DeviceState:
    current_scene: str = "idle"
    last_snapshot: dict | None = None
    pending_prompt: dict | None = None
    cumulative_tokens: int = 0
    today_tokens: int = 0
    sparkline: list[int] = field(default_factory=list)
    snapshots_received: int = 0
    prompts_received: int = 0
    events_received: int = 0


VERBS = {"snapshot", "prompt", "event", "tokens", "idle"}


class MockDevice:
    def __init__(self, *, decision_delay_ms: int, auto_deny: bool, verbose: bool):
        self.state = DeviceState()
        self.decision_delay_ms = decision_delay_ms
        self.auto_deny = auto_deny
        self.verbose = verbose
        self._lock = threading.Lock()

    def _log(self, *a):
        if self.verbose:
            print("[mock]", *a, file=sys.stderr, flush=True)

    def handle_line(self, line: str, send) -> None:
        line = line.strip()
        if not line:
            return
        argv = self._tokenise(line)
        if not argv:
            return
        if argv[0] != "dash":
            send(f"ERR: unknown command: {argv[0]}\n")
            return
        if len(argv) < 2:
            send("ERR: usage: dash <verb> [json]\n")
            return
        verb = argv[1]
        if verb not in VERBS:
            send(f"ERR: dash: unknown verb '{verb}'\n")
            return
        payload = None
        if len(argv) >= 3:
            try:
                payload = json.loads(argv[2])
            except json.JSONDecodeError as e:
                send(f"ERR: dash {verb}: malformed JSON ({e})\n")
                return
        with self._lock:
            if verb == "idle":
                self.state.current_scene = "idle"
                send('OK: {"scene":"idle"}\n')
            elif verb == "snapshot":
                self.state.last_snapshot = payload
                self.state.snapshots_received += 1
                if payload.get("prompt"):
                    self.state.current_scene = "prompt"
                    self.state.pending_prompt = payload["prompt"]
                    self._schedule_decision(send, payload["prompt"]["id"])
                elif payload.get("total", 0) > 0:
                    self.state.current_scene = "sessions"
                else:
                    self.state.current_scene = "idle"
                send(f'OK: {{"scene":"{self.state.current_scene}",'
                     f'"total":{payload.get("total", 0)}}}\n')
            elif verb == "prompt":
                self.state.current_scene = "prompt"
                self.state.pending_prompt = payload
                self.state.prompts_received += 1
                self._schedule_decision(send, payload["id"])
                send(f'OK: {{"scene":"prompt","id":"{payload["id"]}"}}\n')
            elif verb == "event":
                self.state.events_received += 1
                send('OK: {"appended":true}\n')
            elif verb == "tokens":
                self.state.cumulative_tokens = payload.get("cumulative", 0)
                self.state.today_tokens = payload.get("today", 0)
                if "latest_sample" in payload:
                    self.state.sparkline.append(payload["latest_sample"])
                    self.state.sparkline = self.state.sparkline[-64:]
                send('OK: {"updated":true}\n')

    @staticmethod
    def _tokenise(line: str) -> list[str]:
        argv: list[str] = []
        cur: list[str] = []
        in_quote = False
        for ch in line:
            if not in_quote and ch.isspace():
                if cur:
                    argv.append("".join(cur))
                    cur = []
                continue
            if ch == '"':
                in_quote = not in_quote
                continue
            cur.append(ch)
        if cur:
            argv.append("".join(cur))
        return argv

    def _schedule_decision(self, send, prompt_id: str) -> None:
        decision = "deny" if self.auto_deny else "once"
        delay = self.decision_delay_ms

        def fire():
            time.sleep(delay / 1000.0)
            send(f"EVT: permission id={prompt_id} decision={decision}\n")
            with self._lock:
                self.state.current_scene = "idle"
                self.state.pending_prompt = None

        threading.Thread(target=fire, daemon=True).start()


def serve(host: str, port: int, mock: MockDevice) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"[mock] listening on {host}:{port}", file=sys.stderr, flush=True)
    while True:
        conn, addr = srv.accept()
        print(f"[mock] connection from {addr}", file=sys.stderr, flush=True)
        try:
            buf = b""

            def send(s: str) -> None:
                conn.sendall(s.encode("utf-8"))

            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    one, buf = buf.split(b"\n", 1)
                    line = one.decode("utf-8", errors="replace")
                    mock.handle_line(line, send)
        except ConnectionResetError:
            pass
        finally:
            conn.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9876)
    ap.add_argument("--decision-delay-ms", type=int, default=500)
    ap.add_argument("--auto-deny", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    mock = MockDevice(decision_delay_ms=args.decision_delay_ms,
                      auto_deny=args.auto_deny, verbose=args.verbose)
    try:
        serve(args.host, args.port, mock)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
