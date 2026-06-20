"""tools/web/serve.py — web dev-tools server for the ESP32 agent dashboard.

This is the host side of the *off-device development panel*. Its job is NOT to
draw the ESP32 screen. Its job is to give the browser **data + interaction
symmetry** with the firmware so you can develop and debug the data flow without
flashing hardware:

  - Information symmetry: every ``dash`` command the firmware would receive is
    broadcast to the browser over SSE; the browser feeds them to the SAME
    firmware data layer compiled to WASM (same parsing, same agent_state, same
    bugs). The browser computes ``state_json`` exactly like the device.
  - Interaction symmetry: the browser can drive input back through the real
    path — approve/deny a permission, pick a quick-reply option, and INJECT
    simulated hook events so you can exercise the whole bridge->device chain
    with no real agent running.
  - Debug surface: connection/health, raw frame log, EVT/signal stream, and a
    hooks install/enable/disable panel (real ~/.claude + ~/.codex config).

Topology (all loopback):

    browser ──SSE──┐                         ┌── dash cmds ──┐
                   │  GET /events            │               ▼
    browser ──────▶ serve.py (HTTP :8090) ── TCP device :9876 ◀── bridge
       │ POST /inject ──────────────────────────────────────────▶  :7321
       │ POST /decision,/reply  ── EVT permission/reply ─────────▶  (waiter)
       └ POST /hooks/*  ── hooks_admin (real settings.json / hooks.json)

The bridge is the REAL ``claude_buddy_bridge.py`` (reused unchanged). Run it
yourself, or let this server spawn it with ``--spawn-bridge`` so the whole
stack comes up with one command.

Usage:
    python tools/web/serve.py --spawn-bridge      # one command, full stack
    # ...then open http://127.0.0.1:8090/

    # or run the bridge separately:
    python tools/web/serve.py
    python tools/claude_buddy_bridge.py serve --port-kind tcp \\
        --port 127.0.0.1:9876 --listen 127.0.0.1:7321
"""

from __future__ import annotations

import argparse
import http.server
import json
import queue
import socket
import subprocess
import sys
import threading
from dataclasses import asdict
from pathlib import Path

# tools/ on path so we can import the real device + hooks-admin code.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mock_device_v1 import MockDeviceV1  # noqa: E402
import hooks_admin  # noqa: E402
from hooks_admin import state as hooks_state  # noqa: E402

STATIC = Path(__file__).resolve().parent / "static"
REPO_ROOT = Path(__file__).resolve().parents[2]
BRIDGE_SCRIPT = REPO_ROOT / "tools" / "claude_buddy_bridge.py"

# ─────────────────────────────────────────────────────────────────────────────
# SSE fan-out
# ─────────────────────────────────────────────────────────────────────────────

_clients: list[queue.Queue] = []
_clients_lock = threading.Lock()


def _broadcast(line: str) -> None:
    """Push one raw ``dash ...`` line to every connected browser. The browser
    feeds it to the WASM data layer; we do not interpret it here."""
    with _clients_lock:
        for q in list(_clients):
            try:
                q.put_nowait(line)
            except Exception:
                pass


def _client_count() -> int:
    with _clients_lock:
        return len(_clients)


# ─────────────────────────────────────────────────────────────────────────────
# Device link — the live bridge<->device TCP connection
# ─────────────────────────────────────────────────────────────────────────────

class DeviceLink:
    """Holds the currently-connected bridge's send channel + pending prompts.

    The bridge connects to our TCP device (single connection, listen(1)). When
    the bridge pushes a ``dash prompt`` it blocks waiting for the device to
    answer with ``EVT: permission ...`` (or ``EVT: reply ...``). In manual mode
    the *browser* is the device's buttons: it POSTs /decision or /reply and we
    write the EVT back through this link.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._send = None                 # current connection's send(str)->None
        self.pending: dict[str, dict] = {}   # prompt id -> prompt payload

    def attach(self, send) -> None:
        with self._lock:
            self._send = send
            self.pending.clear()

    def detach(self) -> None:
        with self._lock:
            self._send = None
            self.pending.clear()

    @property
    def connected(self) -> bool:
        with self._lock:
            return self._send is not None

    def register_prompt(self, prompt: dict) -> None:
        pid = prompt.get("id")
        if not pid:
            return
        with self._lock:
            self.pending[pid] = prompt

    def send_evt(self, evt_line: str, *, prompt_id: str | None = None) -> bool:
        """Write an EVT line back to the bridge. Returns False if no link."""
        with self._lock:
            send = self._send
            if prompt_id:
                self.pending.pop(prompt_id, None)
        if send is None:
            return False
        if not evt_line.endswith("\n"):
            evt_line += "\n"
        send(evt_line)
        return True

    def pending_list(self) -> list[dict]:
        with self._lock:
            return list(self.pending.values())


_link = DeviceLink()

# ─────────────────────────────────────────────────────────────────────────────
# hooks-admin (real ~ config, user scope) — adapter registry + side state
# ─────────────────────────────────────────────────────────────────────────────

_hooks_agents = hooks_admin.build_agents()
_hooks_state_path = hooks_state.default_state_path()
_hooks_lock = threading.Lock()


def _hooks_status() -> dict:
    with _hooks_lock:
        st = hooks_state.State(_hooks_state_path)
        return {k: asdict(v) for k, v in hooks_admin.status(_hooks_agents, st).items()}


def _hooks_action(action: str, agent: str) -> dict:
    fn = {"install": hooks_admin.install, "enable": hooks_admin.enable,
          "disable": hooks_admin.disable}[action]
    with _hooks_lock:
        st = hooks_state.State(_hooks_state_path)
        targets = list(_hooks_agents) if agent in (None, "all") else [agent]
        for k in targets:
            fn(_hooks_agents, st, k)
    return _hooks_status()


# ─────────────────────────────────────────────────────────────────────────────
# TCP device — the thing the bridge talks to
# ─────────────────────────────────────────────────────────────────────────────

class DeviceServer(threading.Thread):
    """Accepts the bridge connection and reuses MockDeviceV1 for dash handling.

    Every dash line is broadcast to the browser. Permission prompts are routed
    to the browser (manual mode) via the on_prompt callback, or auto-decided
    (approve/deny mode) by MockDeviceV1 itself.
    """

    def __init__(self, host: str, port: int, *, auto: str,
                 mirror_only: bool = False) -> None:
        super().__init__(daemon=True, name="web-device")
        self.host = host
        self.port = port
        self.auto = auto
        # mirror_only: passive tap used in --serial mode. The bridge drives the
        # REAL device over COM and *also* writes a copy of every line here; we
        # only broadcast to the browser (no mock replies, no decision routing —
        # the real device owns those). Gives the web a live mirror of what the
        # hardware screen is showing (the "真机+web 并行" fan-out).
        self.mirror_only = mirror_only
        manual = auto == "off"
        self.mock = None if mirror_only else MockDeviceV1(
            decision_delay_ms=0,
            auto_deny=(auto == "deny"),
            verbose=False,
            on_prompt=self._on_prompt if manual else None,
        )

    def _on_prompt(self, prompt: dict, send) -> None:
        # Manual mode: remember it so /decision and /reply can resolve it, and
        # the browser already sees the `dash prompt` line via _broadcast.
        _link.register_prompt(prompt)

    def run(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.port))
        srv.listen(1)
        role = "mirror tap" if self.mirror_only else "device"
        print(f"[serve] TCP {role} on {self.host}:{self.port} (auto={self.auto})",
              file=sys.stderr, flush=True)
        while True:
            conn, _ = srv.accept()
            print(f"[serve] bridge {role} connected", file=sys.stderr, flush=True)
            self._serve_conn(conn)

    def _serve_conn(self, conn: socket.socket) -> None:
        if self.mirror_only:
            return self._serve_mirror(conn)
        send_lock = threading.Lock()

        def send(s: str) -> None:
            with send_lock:
                try:
                    conn.sendall(s.encode("utf-8"))
                except OSError:
                    pass

        _link.attach(send)
        buf = b""
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    one, buf = buf.split(b"\n", 1)
                    line = one.decode("utf-8", errors="replace")
                    if line.strip():
                        _broadcast(line)
                    self.mock.handle_line(line, send)
        except OSError:
            pass
        finally:
            _link.detach()
            try:
                conn.close()
            except OSError:
                pass
            print("[serve] bridge disconnected", file=sys.stderr, flush=True)

    def _serve_mirror(self, conn: socket.socket) -> None:
        """Passive tap: only broadcast lines to the browser (no replies)."""
        buf = b""
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    one, buf = buf.split(b"\n", 1)
                    line = one.decode("utf-8", errors="replace")
                    if line.strip():
                        _broadcast(line)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass
            print("[serve] bridge mirror disconnected", file=sys.stderr, flush=True)


# ─────────────────────────────────────────────────────────────────────────────
# bridge subprocess (optional, for one-command startup)
# ─────────────────────────────────────────────────────────────────────────────

def spawn_bridge(host: str, device_port: int, bridge_port: int,
                 serial: str | None = None,
                 gate_permissions: bool = False) -> subprocess.Popen:
    extra = []
    if serial:
        # Drive a REAL ESP32 over serial (e.g. COM9). The bridge owns the port
        # via esp_harness; /dash and /inject reach the physical screen. It also
        # mirrors every pushed line to serve.py's tap (--mirror) so the browser
        # gets a live view of exactly what the hardware screen shows.
        transport = ["--port-kind", "serial", "--port", serial]
        extra = ["--mirror", f"{host}:{device_port}"]
    else:
        transport = ["--port-kind", "tcp", "--port", f"{host}:{device_port}"]
    if gate_permissions:
        extra += ["--gate-permissions"]
    cmd = [
        sys.executable, str(BRIDGE_SCRIPT), "serve",
        *transport, *extra, "--listen", f"{host}:{bridge_port}",
    ]
    print(f"[serve] spawning bridge: {' '.join(cmd)}", file=sys.stderr, flush=True)
    proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            cwd=str(REPO_ROOT), text=True)

    def _tee():
        assert proc.stderr is not None
        for ln in proc.stderr:
            print(f"[bridge] {ln.rstrip()}", file=sys.stderr, flush=True)

    threading.Thread(target=_tee, daemon=True, name="bridge-tee").start()
    return proc


# ─────────────────────────────────────────────────────────────────────────────
# HTTP handler
# ─────────────────────────────────────────────────────────────────────────────

_MIME = {".mjs": "text/javascript", ".js": "text/javascript",
         ".wasm": "application/wasm", ".html": "text/html; charset=utf-8",
         ".css": "text/css", ".json": "application/json"}

_DECISIONS = {"once", "allow", "deny"}


class _Handler(http.server.BaseHTTPRequestHandler):
    # set by main()
    bridge_addr: tuple[str, int] = ("127.0.0.1", 7321)
    inject_timeout: float = 70.0
    auto: str = "off"
    serial: str | None = None       # real device port (None = mock TCP)
    gate: bool = False              # permission gating on?

    # ── helpers ───────────────────────────────────────────────────────
    def _json(self, obj, code: int = 200) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def _read_body(self) -> dict:
        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            return json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return {}

    def _forward_to_bridge(self, event: dict) -> dict:
        host, port = self.bridge_addr
        try:
            with socket.create_connection((host, port), timeout=5.0) as sock:
                sock.sendall((json.dumps(event) + "\n").encode("utf-8"))
                sock.settimeout(self.inject_timeout)
                data = sock.makefile("r", encoding="utf-8").readline()
                return json.loads(data) if data.strip() else {"continue": True}
        except (ConnectionRefusedError, socket.timeout, OSError) as e:
            return {"error": f"bridge offline: {e}", "bridge": f"{host}:{port}"}
        except json.JSONDecodeError as e:
            return {"error": f"bad bridge reply: {e}"}

    # ── POST ──────────────────────────────────────────────────────────
    def do_POST(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path.startswith("/hooks/"):
            return self._post_hooks(path)
        if path == "/inject":
            return self._post_inject()
        if path == "/decision":
            return self._post_decision()
        if path == "/reply":
            return self._post_reply()
        if path == "/dash":
            return self._post_dash()
        if path == "/hold":
            return self._post_hold()
        self.send_response(404)
        self.end_headers()

    def _post_hooks(self, path: str) -> None:
        action = path[len("/hooks/"):]
        if action not in ("install", "enable", "disable"):
            self.send_response(404)
            self.end_headers()
            return
        body = self._read_body()
        try:
            self._json(_hooks_action(action, body.get("agent", "all")))
        except Exception as e:
            self._json({"error": str(e)}, code=500)

    def _post_inject(self) -> None:
        """Forward a raw hook event to the bridge, exactly like hook_dispatch.
        Lets the browser drive the whole chain with no real agent. Permission
        events block here until the browser answers via /decision."""
        event = self._read_body()
        if not isinstance(event, dict) or "type" not in event:
            self._json({"error": "body must be a hook event with a 'type'"}, code=400)
            return
        self._json(self._forward_to_bridge(event))

    def _post_decision(self) -> None:
        """Answer a pending permission prompt: {id, decision: once|allow|deny}."""
        body = self._read_body()
        pid = body.get("id")
        decision = body.get("decision", "once")
        if not pid or decision not in _DECISIONS:
            self._json({"error": "need {id, decision in once|allow|deny}"}, code=400)
            return
        ok = _link.send_evt(f"EVT: permission id={pid} decision={decision}",
                            prompt_id=pid)
        self._json({"ok": ok, "sent": {"id": pid, "decision": decision},
                    "device_connected": ok})

    def _post_reply(self) -> None:
        """Answer a quick-reply prompt: {id, choice: <int index>}."""
        body = self._read_body()
        pid = body.get("id")
        try:
            choice = int(body.get("choice"))
        except (TypeError, ValueError):
            self._json({"error": "need {id, choice:int}"}, code=400)
            return
        if not pid:
            self._json({"error": "need {id, choice:int}"}, code=400)
            return
        ok = _link.send_evt(f"EVT: reply id={pid} choice={choice}", prompt_id=pid)
        self._json({"ok": ok, "sent": {"id": pid, "choice": choice},
                    "device_connected": ok})

    def _post_dash(self) -> None:
        """Screen test driver: push one raw `dash <cmd> [json]` line straight
        to the connected device (real ESP32 or mock) to exercise UI combos.
        Body: {cmd: "snapshot"|"prompt"|..., payload: {...}|null}."""
        body = self._read_body()
        cmd = body.get("cmd")
        if not cmd or not isinstance(cmd, str):
            self._json({"error": "need {cmd, payload?}"}, code=400)
            return
        self._json(self._forward_to_bridge(
            {"type": "__dash__", "cmd": cmd, "payload": body.get("payload")}))

    def _post_hold(self) -> None:
        """Freeze/unfreeze the bridge's auto snapshot publisher so a hand-pushed
        UI combo stays on the device screen. Body: {on: true|false}."""
        body = self._read_body()
        self._json(self._forward_to_bridge(
            {"type": "__pause__", "on": bool(body.get("on"))}))

    # ── GET ───────────────────────────────────────────────────────────
    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/hooks":
            return self._json(_hooks_status())
        if path == "/state":
            return self._json(self._state())
        if path == "/events":
            return self._sse()
        return self._static(path)

    def _state(self) -> dict:
        host, port = self.bridge_addr
        return {
            "device_connected": _link.connected,
            "clients": _client_count(),
            "pending": _link.pending_list(),
            "bridge_addr": f"{host}:{port}",
            "bridge_reachable": self._bridge_reachable(),
            "auto": self.auto,
            "serial": self.serial,
            "gate": self.gate,
        }

    def _bridge_reachable(self) -> bool:
        host, port = self.bridge_addr
        try:
            with socket.create_connection((host, port), timeout=0.4):
                return True
        except OSError:
            return False

    def _sse(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q: queue.Queue = queue.Queue()
        with _clients_lock:
            _clients.append(q)
        try:
            self.wfile.write(b": connected\n\n")
            self.wfile.flush()
            while True:
                line = q.get()
                payload = json.dumps(line)
                self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            with _clients_lock:
                if q in _clients:
                    _clients.remove(q)

    def _static(self, path: str) -> None:
        rel = path.lstrip("/") or "index.html"
        f = (STATIC / rel).resolve()
        if (STATIC in f.parents or f == STATIC / rel) and f.is_file():
            self.send_response(200)
            self.send_header("Content-Type", _MIME.get(f.suffix, "application/octet-stream"))
            body = f.read_bytes()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except OSError:
                pass
            return
        self.send_response(404)
        self.end_headers()

    def log_message(self, *a):  # silence default access log
        pass


class _ThreadingHTTP(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ─────────────────────────────────────────────────────────────────────────────
# main
# ─────────────────────────────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--device-port", type=int, default=9876,
                    help="TCP port the bridge connects to (default 9876)")
    ap.add_argument("--http-port", type=int, default=8090,
                    help="HTTP/SSE port for the browser (default 8090)")
    ap.add_argument("--bridge-port", type=int, default=7321,
                    help="bridge hook-listen port for /inject (default 7321)")
    ap.add_argument("--auto", choices=["off", "approve", "deny"], default="off",
                    help="permission handling: off=browser decides (default), "
                         "approve/deny=auto-resolve without the browser")
    ap.add_argument("--spawn-bridge", action="store_true",
                    help="also launch claude_buddy_bridge.py wired to this device")
    ap.add_argument("--serial", default=None, metavar="PORT",
                    help="with --spawn-bridge: drive a REAL device on this serial "
                         "port (e.g. COM9) instead of the mock TCP device. /dash "
                         "and /inject reach the physical screen (web data mirror "
                         "is off in this mode).")
    ap.add_argument("--gate-permissions", action="store_true",
                    help="block tool calls until approved via device/browser "
                         "(default: observe — don't stall the agent)")
    args = ap.parse_args(argv)

    # Gate by default in mock mode (web approve/deny demo); observe on a real
    # serial device (don't stall the live agent). --gate-permissions forces it.
    gate = args.gate_permissions or not args.serial
    _Handler.bridge_addr = (args.host, args.bridge_port)
    _Handler.auto = args.auto
    _Handler.serial = args.serial
    _Handler.gate = gate

    # Mock mode: our TCP device IS the device the bridge talks to.
    # Serial mode: the bridge drives the real device but mirrors every line to
    # us, so run a passive tap on the same port to feed the web data mirror.
    DeviceServer(args.host, args.device_port, auto=args.auto,
                 mirror_only=bool(args.serial)).start()

    bridge_proc = None
    if args.spawn_bridge:
        bridge_proc = spawn_bridge(args.host, args.device_port, args.bridge_port,
                                   serial=args.serial, gate_permissions=gate)
    elif args.serial:
        print("[serve] --serial given without --spawn-bridge: run the bridge "
              "yourself with --port-kind serial --port " + args.serial,
              file=sys.stderr, flush=True)

    mode = f"serial {args.serial}" if args.serial else "mock tcp"
    print(f"[serve] HTTP on http://{args.host}:{args.http_port}/  "
          f"(inject/dash -> bridge {args.host}:{args.bridge_port} -> {mode})",
          file=sys.stderr, flush=True)
    srv = _ThreadingHTTP((args.host, args.http_port), _Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if bridge_proc is not None:
            bridge_proc.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
