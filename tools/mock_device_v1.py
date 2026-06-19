#!/usr/bin/env python3
"""mock_device_v1.py — TCP stand-in for the ESP32 dashboard speaking the v1
wire protocol.

Extends `docs/mock_device.py` with the verbs `config`, `time`, `health`
(plus tagged HEALTH_BEGIN/HEALTH_END reply). For local E2E of the v1 host
bridge (`claude_buddy_bridge.py`) without COM9 access.

Usage:
    python tools/mock_device_v1.py --port 9876 -v
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
    device_name: str = "MockDev"
    owner: str = "anon"
    theme: str = "noir"
    current_scene: str = "idle"
    epoch_unix: int = 0
    tz_offset_seconds: int = 0
    last_snapshot: dict | None = None
    pending_prompt: dict | None = None
    snapshots_received: int = 0
    prompts_received: int = 0
    events_received: int = 0
    decisions_sent: int = 0
    boot_unix: float = field(default_factory=time.time)
    agent_count: int = 0
    connection_age_started: float = field(default_factory=time.time)


# NOTE: "push" is the top-banner verb the bridge emits on every post_tool_use
# (see claude_buddy_bridge.Bridge: pusher.push("push", {...})). It was missing
# here, so the bridge logged `device ERR: dash: unknown verb 'push'` on every
# tool call. It's fire-and-forget (no state, no EVT) — just ACK it.
VERBS = {"snapshot", "prompt", "event", "tokens", "idle", "config", "time",
         "health", "push"}


class MockDeviceV1:
    def __init__(self, *, decision_delay_ms: int, auto_deny: bool, verbose: bool,
                 on_prompt=None):
        """on_prompt: optional callback ``(prompt: dict, send) -> None``. When
        set, an incoming ``prompt`` (or a snapshot-embedded pending prompt) is
        handed to the callback INSTEAD of being auto-decided here. The callback
        owns the decision round-trip (e.g. route it to a browser button and
        later write ``EVT: permission ...`` / ``EVT: reply ...`` back through
        ``send``). When None, the legacy auto-decision behaviour applies
        (``once``/``deny`` after ``decision_delay_ms``) — used by the standalone
        mock and the bench/replay harnesses."""
        self.state = DeviceState()
        self.decision_delay_ms = decision_delay_ms
        self.auto_deny = auto_deny
        self.verbose = verbose
        self.on_prompt = on_prompt
        self._lock = threading.Lock()

    def _log(self, *a):
        if self.verbose:
            print("[mockv1]", *a, file=sys.stderr, flush=True)

    def handle_line(self, line: str, send) -> None:
        line = line.strip()
        if not line:
            return
        argv = self._tokenise(line)
        if not argv or argv[0] != "dash":
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
                self._log("MALFORMED", verb, str(e), argv[2][:80])
                send(f"ERR: dash {verb}: malformed JSON ({e})\n")
                return
        with self._lock:
            self._dispatch(verb, payload, send)

    def _dispatch(self, verb, payload, send) -> None:
        s = self.state
        self._log("RX", verb, payload)
        if verb == "idle":
            s.current_scene = "idle"
            send('OK: {"scene":"idle"}\n')
            return
        if verb == "snapshot":
            s.last_snapshot = payload or {}
            s.snapshots_received += 1
            s.agent_count = len((payload or {}).get("agents") or [])
            # Pending prompt from v1 agents[].prompt?
            pending = None
            for a in (payload or {}).get("agents", []):
                if a.get("prompt"):
                    pending = a["prompt"]; break
            if pending:
                s.current_scene = "prompt"
                s.pending_prompt = pending
                self._offer_prompt(pending, send)
            elif s.agent_count > 0:
                s.current_scene = "sessions"
            else:
                s.current_scene = "idle"
            send(f'OK: {{"scene":"{s.current_scene}","total":{s.agent_count}}}\n')
            return
        if verb == "prompt":
            s.current_scene = "prompt"
            s.pending_prompt = payload
            s.prompts_received += 1
            self._offer_prompt(payload, send)
            send(f'OK: {{"scene":"prompt","id":"{payload["id"]}"}}\n')
            return
        if verb == "push":
            # Top-slide banner; fire-and-forget. No scene change, no EVT.
            send('OK: {"banner":true}\n')
            return
        if verb == "event":
            s.events_received += 1
            send('OK: {"appended":true}\n')
            return
        if verb == "tokens":
            send('OK: {"updated":true}\n')
            return
        if verb == "config":
            for k in ("device_name", "owner", "theme"):
                if payload and k in payload:
                    setattr(s, k, payload[k])
            send('OK: {"config":"applied"}\n')
            return
        if verb == "time":
            if payload:
                s.epoch_unix = int(payload.get("epoch_unix", 0))
                s.tz_offset_seconds = int(payload.get("tz_offset_seconds", 0))
            send('OK: {"time":"set"}\n')
            return
        if verb == "health":
            body = json.dumps({
                "device_name": s.device_name,
                "owner": s.owner,
                "scene": s.current_scene,
                "uptime_s": int(time.time() - s.boot_unix),
                "heap_free": 84200,
                "heap_min": 78400,
                "fps": 33.4,
                "battery_pct": 87,
                "snapshots_received": s.snapshots_received,
                "prompts_received": s.prompts_received,
                "decisions_sent": s.decisions_sent,
                "connection_age_s": int(time.time() - s.connection_age_started),
                "agent_count": s.agent_count,
            })
            send(f"OK: payload follows tag=HEALTH\nHEALTH_BEGIN fmt=json bytes={len(body)}\n{body}\nHEALTH_END\n")
            return

    @staticmethod
    def _tokenise(line: str) -> list[str]:
        """G-7 tokeniser (esp-harness@664b14e):
        - Token starting with `"` is quote-leading: strip the leading `"`,
          accumulate every char (including inner `"` and inner whitespace)
          until the LAST `"` of the line that is immediately followed by
          whitespace or end-of-line. That closes the token.
        - Token NOT starting with `"` uses legacy toggle-on-any-quote
          (whitespace inside `"..."` is part of the token, all `"` are
          stripped).
        """
        argv: list[str] = []
        n = len(line)
        i = 0
        while i < n:
            while i < n and line[i].isspace():
                i += 1
            if i >= n:
                break
            if line[i] == '"':
                # Quote-leading token: find the closing `"` followed by ws/EOL
                i += 1  # drop leading "
                start = i
                close = -1
                j = i
                while j < n:
                    if line[j] == '"':
                        if j + 1 == n or line[j + 1].isspace():
                            close = j
                            break
                    j += 1
                if close == -1:
                    # No matching close — take everything to EOL
                    argv.append(line[start:n])
                    i = n
                else:
                    argv.append(line[start:close])
                    i = close + 1
            else:
                # Legacy mode
                in_quote = False
                cur: list[str] = []
                while i < n:
                    ch = line[i]
                    if not in_quote and ch.isspace():
                        break
                    if ch == '"':
                        in_quote = not in_quote
                        i += 1
                        continue
                    cur.append(ch)
                    i += 1
                argv.append("".join(cur))
        return argv

    def _offer_prompt(self, prompt: dict, send) -> None:
        """A prompt arrived. Either hand it to the on_prompt callback (manual
        / browser-driven decision) or fall back to the auto-decision timer."""
        if self.on_prompt is not None:
            try:
                self.on_prompt(prompt, send)
            except Exception as e:  # never let a UI callback kill the device loop
                self._log("on_prompt raised", str(e))
            return
        self._schedule_decision(send, prompt.get("id", ""))

    def _schedule_decision(self, send, prompt_id: str) -> None:
        decision = "deny" if self.auto_deny else "once"
        delay = self.decision_delay_ms

        def fire():
            time.sleep(delay / 1000.0)
            send(f"EVT: permission id={prompt_id} decision={decision}\n")
            with self._lock:
                self.state.current_scene = "idle"
                self.state.pending_prompt = None
                self.state.decisions_sent += 1
        threading.Thread(target=fire, daemon=True).start()


def serve(host: str, port: int, mock: MockDeviceV1) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"[mockv1] listening on {host}:{port}", file=sys.stderr, flush=True)
    while True:
        conn, addr = srv.accept()
        print(f"[mockv1] connection from {addr}", file=sys.stderr, flush=True)
        mock.state.connection_age_started = time.time()
        try:
            buf = b""
            def send(s: str) -> None:
                try:
                    conn.sendall(s.encode("utf-8"))
                except OSError:
                    pass
            while True:
                try:
                    chunk = conn.recv(4096)
                except (ConnectionResetError, ConnectionAbortedError, OSError):
                    break
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    one, buf = buf.split(b"\n", 1)
                    line = one.decode("utf-8", errors="replace")
                    mock.handle_line(line, send)
        except (ConnectionResetError, ConnectionAbortedError, OSError):
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9876)
    ap.add_argument("--decision-delay-ms", type=int, default=500)
    ap.add_argument("--auto-deny", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    mock = MockDeviceV1(decision_delay_ms=args.decision_delay_ms,
                        auto_deny=args.auto_deny, verbose=args.verbose)
    try:
        serve(args.host, args.port, mock)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
