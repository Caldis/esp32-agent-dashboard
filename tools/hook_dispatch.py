"""hook_dispatch.py — tiny stdin→bridge forwarder for Claude Code hooks.

Claude Code's hook config points at this script with one positional arg
(the event type, e.g. ``pre_tool_use``). We read the hook payload from
stdin, forward it to the long-running ``claude_buddy_bridge serve``
daemon over loopback TCP, and pipe the daemon's JSON response back to
stdout (which Claude Code reads).

Design contract (unchanged): this hook MUST NOT block Claude Code and
MUST NOT be chatty. In the common failure case we print ``{"continue":
true}`` and exit 0, silently.

Self-healing (v3): if the bridge isn't running, we AUTO-START it
(``claude_buddy_bridge serve``, detached) instead of just failing — so the
user never has to babysit the daemon. A best-effort cooldown + the
bridge's own single-instance guard prevent a spawn stampede across the
many concurrent hook processes.

User-facing messages (``systemMessage``) are the text the user sees in
their Claude Code terminal. They are surfaced ONLY on state transitions
(offline→online, first offline, bridge-wedged) and hard-throttled — no
more one-line-per-tool-call spam. Steady state (bridge healthy, or
bridge briefly unreachable and already being started) is silent.

Why a separate script: ``hooks.command`` is invoked per event, so we want
this hook to be small, dependency-free, and fast (sub-millisecond in the
healthy case).
"""

from __future__ import annotations

import json
import os
import re
import socket
import subprocess
import sys
import time

# CC pipes UTF-8 JSON on stdin. On a non-UTF-8 locale (e.g. Chinese Windows
# cp936) the default text decode mangles any CJK in prompts/tool args and can
# even raise mid-read. Force UTF-8 so the payload survives verbatim. Guarded so
# a stub stdin (tests) without reconfigure() doesn't explode.
try:  # pragma: no cover - environment dependent
    sys.stdin.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

DEFAULT_HOST = os.environ.get("CLAUDE_BUDDY_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("CLAUDE_BUDDY_PORT", "7321"))
# Connect timeout. Kept SHORT: a healthy bridge accepts instantly, and when the
# bridge is down some hosts (loopback firewalls) DROP the SYN rather than
# refusing it, so a long connect timeout would stall every tool call for its
# full duration. 0.5s is plenty for a live local bridge.
CONNECT_TIMEOUT = float(os.environ.get("CLAUDE_BUDDY_CONNECT_TIMEOUT", "0.5"))
# Read timeout — how long we wait for the bridge to REPLY once connected. The
# bridge answers within ~1ms in the common case; this is the "bridge accepted
# the socket but is wedged" ceiling before we give up on this one event.
DEFAULT_TIMEOUT = float(os.environ.get("CLAUDE_BUDDY_TIMEOUT", "3.0"))
# PreToolUse read timeout — the device is observe-only by default (it never
# gates), so the bridge replies instantly. 8s only ever matters for a gate-mode
# device awaiting a physical button press.
PROMPT_TIMEOUT = float(os.environ.get("CLAUDE_BUDDY_PROMPT_TIMEOUT", "8.0"))

# ── self-healing: auto-start the bridge when it's not running ─────────
# Default ON — this is the "I never want to manually start the daemon again"
# behaviour. Set CLAUDE_BUDDY_AUTOSTART=0 to disable.
AUTOSTART = os.environ.get("CLAUDE_BUDDY_AUTOSTART", "1").lower() in (
    "1", "true", "yes", "on",
)
# Don't re-spawn within this window: the bridge needs a few seconds to boot,
# import esp_harness, open the serial port and start listening. Re-spawning
# inside that window just races (the bridge's single-instance guard rejects the
# duplicate, but we avoid the churn).
AUTOSTART_COOLDOWN_S = float(os.environ.get("CLAUDE_BUDDY_AUTOSTART_COOLDOWN", "20.0"))
# After a spawn, skip the connect probe entirely for this grace period — the
# bridge isn't listening yet, so probing just costs every event CONNECT_TIMEOUT
# for nothing. Pass straight through.
AUTOSTART_GRACE_S = float(os.environ.get("CLAUDE_BUDDY_AUTOSTART_GRACE", "4.0"))

# circuit breaker: N consecutive WEDGE timeouts in W seconds → skip for M
# seconds. This now guards ONLY the "connected but the bridge never replied"
# case (a genuinely stuck daemon). Plain "bridge offline" no longer trips it —
# that path auto-starts instead. Kept so a wedged bridge can't stall the agent
# on every single tool call.
CB_THRESHOLD = int(os.environ.get("CLAUDE_BUDDY_CB_THRESHOLD", "3"))
CB_WINDOW_S = float(os.environ.get("CLAUDE_BUDDY_CB_WINDOW", "30.0"))
CB_COOLDOWN_S = float(os.environ.get("CLAUDE_BUDDY_CB_COOLDOWN", "20.0"))

# How often (at most) we surface a *persistent* condition to the user. State
# TRANSITIONS (offline→online, healthy→wedged) always speak once; this only
# throttles the "still offline / still wedged" repeats.
NOTE_INTERVAL_S = float(os.environ.get("CLAUDE_BUDDY_NOTE_INTERVAL", "60.0"))

_TMP = os.environ.get("TEMP", os.environ.get("TMPDIR", "/tmp"))
# Single consolidated state file (atomic writes) — circuit breaker, last-known
# connectivity, last user-message time, last autostart time. One file keeps the
# concurrent-writer surface small and lets us reason about transitions.
_STATE_FILE = os.path.join(_TMP, "claude_buddy_hook_state.json")
_AUTOSTART_LOG = os.path.join(_TMP, "claude_buddy_bridge.autostart.log")

_DBG_FLAG = os.path.join(_TMP, "hook_dispatch_debug.on")
_DBG_LOG = os.path.join(_TMP, "hook_dispatch_debug.log")


def _dlog(msg: str) -> None:
    """Append a diagnostic line iff the sentinel file exists. Off by default
    (one stat), so safe to ship — create the .on file to trace hook events
    end-to-end, delete it to stop."""
    try:
        if not os.path.exists(_DBG_FLAG):
            return
        with open(_DBG_LOG, "a", encoding="utf-8") as f:
            f.write(f"{time.time():.3f} pid={os.getpid()} {msg}\n")
    except OSError:
        pass


# ── agent liveness: find the Claude Code host process pid ───────────
#
# ctrl+c / kill / terminal-close often ends a CC session WITHOUT firing the
# SessionEnd hook (esp. on Windows), so the bridge needs an out-of-band
# liveness signal. Every forwarded event carries `agent_pid` — the nearest ancestor
# process that is NOT an interpreter/shell intermediary (i.e. the CC host,
# typically node.exe / claude.exe / bun.exe). The bridge polls that pid and
# drops the session the moment the process is gone.

_INTERMEDIARY_EXES = {
    # us + launchers CC may interpose between itself and this script
    "python.exe", "pythonw.exe", "python3.exe", "py.exe",
    "cmd.exe", "conhost.exe", "powershell.exe", "pwsh.exe",
    "bash.exe", "sh.exe", "busybox64.exe", "winpty-agent.exe",
    "python", "python3", "sh", "bash", "dash", "zsh",
}


def _win_process_table() -> dict:
    """pid -> (ppid, exe_lower) for all processes (Toolhelp32 snapshot)."""
    import ctypes
    from ctypes import wintypes

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.c_size_t),   # ULONG_PTR
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", ctypes.c_long),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", ctypes.c_wchar * 260),
        ]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    TH32CS_SNAPPROCESS = 0x2
    INVALID_HANDLE = ctypes.c_void_p(-1).value
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap in (0, INVALID_HANDLE):
        return {}
    table: dict = {}
    try:
        ent = PROCESSENTRY32W()
        ent.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        ok = k32.Process32FirstW(snap, ctypes.byref(ent))
        while ok:
            table[int(ent.th32ProcessID)] = (
                int(ent.th32ParentProcessID), ent.szExeFile.lower())
            ok = k32.Process32NextW(snap, ctypes.byref(ent))
    finally:
        k32.CloseHandle(snap)
    return table


def _agent_pid_from_table(table: dict, start_pid: int) -> int:
    """Walk ancestors from start_pid; return the first non-intermediary, or 0.
    Pure function so tests can feed a synthetic table."""
    pid = start_pid
    for _ in range(8):
        ent = table.get(pid)
        if ent is None:
            return 0
        ppid = ent[0]
        pent = table.get(ppid)
        if pent is None or ppid <= 4 or ppid == pid:
            return 0
        if pent[1] not in _INTERMEDIARY_EXES:
            return ppid
        pid = ppid
    return 0


def _find_agent_pid() -> int:
    """Best-effort pid of the CC host process. 0 = unknown (feature off)."""
    try:
        if os.name == "nt":
            return _agent_pid_from_table(_win_process_table(), os.getpid())
        # POSIX: walk /proc when available (Linux); else best-effort parent.
        pid = os.getppid()
        for _ in range(8):
            if pid <= 1:
                return 0
            try:
                with open(f"/proc/{pid}/comm", "r", encoding="utf-8",
                          errors="replace") as f:
                    name = f.read().strip().lower()
                with open(f"/proc/{pid}/stat", "r", encoding="utf-8",
                          errors="replace") as f:
                    ppid = int(f.read().rsplit(")", 1)[1].split()[1])
            except OSError:
                return pid   # no /proc (macOS) — direct parent is our best guess
            if name not in _INTERMEDIARY_EXES:
                return pid
            pid = ppid
        return 0
    except Exception:  # noqa: BLE001 - liveness is best-effort, never block CC
        return 0


# Targeted debug mode. If $TEMP/hook_dispatch_target.json exists, it scopes which
# sessions reach the bridge — used to silence the main session while debugging so
# the device reflects only the test input. Shapes:
#   {"only":    ["<sid-prefix>", ...]}   forward ONLY these sessions
#   {"exclude": ["<sid-prefix>", ...]}   forward all EXCEPT these
# Absent/invalid → forward everything (normal operation).
_TARGET_FILE = os.path.join(_TMP, "hook_dispatch_target.json")


def _forward_allowed(payload: dict) -> bool:
    try:
        if not os.path.exists(_TARGET_FILE):
            return True
        with open(_TARGET_FILE, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except (OSError, json.JSONDecodeError, ValueError):
        return True
    sid = str(payload.get("session_id", ""))
    only = cfg.get("only")
    if only:
        return any(sid.startswith(p) for p in only)
    excl = cfg.get("exclude") or []
    return not any(sid.startswith(p) for p in excl)


# ── shared state (atomic) ────────────────────────────────────────────

def _state_load() -> dict:
    try:
        with open(_STATE_FILE, "r", encoding="utf-8") as f:
            s = json.load(f)
        if isinstance(s, dict):
            return s
    except (OSError, json.JSONDecodeError, ValueError):
        pass
    return {}


def _state_save(state: dict) -> None:
    """Atomic write: temp + os.replace so concurrent hook processes never read a
    half-written (torn) JSON blob. Best-effort; a lost update just means one
    process's bookkeeping is stale, never a crash."""
    tmp = f"{_STATE_FILE}.{os.getpid()}.tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(state, f)
        os.replace(tmp, _STATE_FILE)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def _cb_is_open(state: dict) -> bool:
    return time.time() < float(state.get("cb_open_until", 0.0) or 0.0)


def _cb_record_timeout(state: dict) -> None:
    """Record a WEDGE timeout (connected, no reply) and trip the breaker if the
    threshold is reached within the window."""
    now = time.time()
    cutoff = now - CB_WINDOW_S
    ts = [t for t in state.get("cb_timestamps", []) if t > cutoff]
    ts.append(now)
    if len(ts) >= CB_THRESHOLD:
        state["cb_open_until"] = now + CB_COOLDOWN_S
        ts = []
    state["cb_timestamps"] = ts


def _cb_clear(state: dict) -> None:
    state["cb_timestamps"] = []
    state["cb_open_until"] = 0.0


# ── self-healing: bridge auto-start ──────────────────────────────────

def _bridge_script() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "claude_buddy_bridge.py")


def _spawn_bridge() -> bool:
    """Launch ``claude_buddy_bridge.py serve`` fully detached. The bridge reads
    its own port/config from ~/.claude-buddy/config.toml (COM9 default), so we
    pass no transport args. Returns True if the spawn call itself succeeded."""
    script = _bridge_script()
    if not os.path.exists(script):
        return False
    try:
        logf = open(_AUTOSTART_LOG, "ab")
    except OSError:
        logf = subprocess.DEVNULL
    cmd = [sys.executable, script, "serve"]
    kwargs: dict = dict(
        stdin=subprocess.DEVNULL, stdout=logf, stderr=logf, close_fds=True,
    )
    if os.name == "nt":
        # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW:
        # survive the hook process exiting, no console window flash.
        kwargs["creationflags"] = 0x00000008 | 0x00000200 | 0x08000000
    else:
        kwargs["start_new_session"] = True
    try:
        subprocess.Popen(cmd, **kwargs)
        _dlog(f"spawned bridge: {' '.join(cmd)}")
        return True
    except Exception as e:  # noqa: BLE001 - spawn must never raise into the hook
        _dlog(f"spawn bridge FAILED: {e}")
        return False
    finally:
        if logf not in (subprocess.DEVNULL, None):
            try:
                logf.close()
            except OSError:
                pass


def _maybe_autostart(state: dict) -> str:
    """If enabled and not on cooldown, spawn the bridge. Mutates *state* with
    the attempt time. Returns one of: 'started' (spawned now), 'cooldown'
    (recently attempted, skipped), 'disabled', 'failed'."""
    if not AUTOSTART:
        return "disabled"
    now = time.time()
    if now - float(state.get("start_ts", 0.0) or 0.0) < AUTOSTART_COOLDOWN_S:
        return "cooldown"
    state["start_ts"] = now
    return "started" if _spawn_bridge() else "failed"


# ── user-facing messages (transition-aware, throttled) ───────────────

def _passthrough(msg: str = "") -> int:
    out: dict = {"continue": True}
    if msg:
        out["systemMessage"] = msg
    print(json.dumps(out))
    return 0


def _note(state: dict, conn: str, msg: str) -> str:
    """Decide whether to surface *msg* for connection state *conn*. Speaks on
    every transition (conn changed since last event) and at most once per
    NOTE_INTERVAL_S while the condition persists. Returns the message to show
    ('' = stay silent). Mutates *state* bookkeeping."""
    prev = state.get("conn", "unknown")
    now = time.time()
    transition = conn != prev
    state["conn"] = conn
    if not msg:
        return ""
    if transition or (now - float(state.get("msg_ts", 0.0) or 0.0)) >= NOTE_INTERVAL_S:
        state["msg_ts"] = now
        return msg
    return ""


# ── dash-state extraction (v2.4.0 contract) ──────────────────────────
#
# On Stop we read the transcript's last assistant message and extract any
# `<dash-state>` block (marquee summary + 2-4 options) for the device's
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
            if line.lower().startswith("summary:"):
                summary = line.split(":", 1)[1].strip()
                continue
            if line.lower().startswith("options:"):
                in_options = True
                continue
            if not summary:
                summary = line.strip()
        else:
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
    """Best-effort: pull the most recent assistant message text from a CC
    transcript JSONL. Tail-reads only (~128KB) — Stop is a BLOCKING hook and
    transcripts grow to many MB. Falls back to empty string on any error."""
    if not transcript_path or not os.path.exists(transcript_path):
        return ""
    try:
        with open(transcript_path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            start = max(0, size - 131072)
            f.seek(start)
            data = f.read()
    except OSError:
        return ""
    lines = data.decode("utf-8", errors="replace").splitlines()
    if start > 0 and lines:
        lines = lines[1:]            # drop the partial first line from a mid-file cut
    for ln in reversed(lines):
        ln = ln.strip()
        if not ln:
            continue
        try:
            rec = json.loads(ln)
        except json.JSONDecodeError:
            continue
        if (rec.get("type") or rec.get("role")) != "assistant":
            continue
        msg = rec.get("message") if isinstance(rec.get("message"), dict) else rec
        content = msg.get("content") or msg.get("text") or ""
        if isinstance(content, list):
            parts = []
            for block in content:
                if isinstance(block, dict) and block.get("type", "text") == "text":
                    txt = block.get("text") or ""
                    if isinstance(txt, str) and txt:
                        parts.append(txt)
            content = "\n".join(parts)
        if isinstance(content, str) and content.strip():
            return content
    return ""


def _is_tool_result(content) -> bool:
    """True if a `user` transcript record is actually a tool_result delivery
    (not a real human prompt). CC wraps tool outputs in a user-role record whose
    content list carries type=="tool_result" blocks."""
    if isinstance(content, list):
        for b in content:
            if isinstance(b, dict) and b.get("type") == "tool_result":
                return True
    return False


def _read_turn_output_tokens(transcript_path: str) -> int:
    """Sum output_tokens of the assistant messages in the latest turn (back to
    the previous *real* user prompt). CC hooks carry no token data, so the
    device's 'tokens today' would be stuck at 0 without this.

    Turn boundary = a user record that is a genuine prompt. A user record
    carrying a tool_result is NOT a boundary (it's mid-turn tool output) — the
    old code broke on it, so any turn with tool calls counted only the tokens
    after its final tool_result, systematically under-counting.
    """
    if not transcript_path or not os.path.exists(transcript_path):
        return 0
    try:
        with open(transcript_path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 131072))
            data = f.read()
    except OSError:
        return 0
    total = 0
    for ln in reversed(data.decode("utf-8", errors="replace").splitlines()):
        ln = ln.strip()
        if not ln:
            continue
        try:
            rec = json.loads(ln)
        except json.JSONDecodeError:
            continue
        role = rec.get("type") or rec.get("role")
        if role == "user":
            msg = rec.get("message") if isinstance(rec.get("message"), dict) else rec
            if _is_tool_result(msg.get("content")):
                continue               # tool output, not a turn boundary
            break                      # real user prompt → turn boundary
        if role == "assistant":
            usage = (rec.get("message") or {}).get("usage") or {}
            total += int(usage.get("output_tokens") or 0)
    return total


def _enrich_stop(payload: dict, event_type: str) -> None:
    """v2.4.0: enrich Stop events with the assistant's last text, any
    <dash-state> block, and this turn's output tokens."""
    if event_type != "stop":
        return
    transcript_path = payload.get("transcript_path") or ""
    last_text = _read_last_assistant_text(transcript_path)
    if last_text and "last_assistant_text" not in payload:
        payload["last_assistant_text"] = last_text
    ds = _extract_dash_state(last_text)
    if ds:
        payload["dash_state"] = ds
    payload.setdefault("tokens", _read_turn_output_tokens(transcript_path))


def _forward(payload: dict, read_timeout: float) -> tuple[str, str]:
    """Send *payload* to the bridge and read one reply line.

    Returns (outcome, line) where outcome is:
      'ok'      — bridge replied; *line* is its JSON response
      'offline' — could not connect (bridge down / SYN dropped)
      'wedged'  — connected but no reply within read_timeout (stuck daemon)
    """
    # Phase 1 — connect. A failure here means the bridge isn't listening. On
    # loopback-firewalled hosts this raises socket.timeout (dropped SYN) rather
    # than ConnectionRefused; both mean 'offline', NOT 'wedged'.
    try:
        sock = socket.create_connection((DEFAULT_HOST, DEFAULT_PORT),
                                        timeout=CONNECT_TIMEOUT)
    except (ConnectionRefusedError, socket.timeout, OSError):
        return "offline", ""
    # Phase 2 — send + read. A timeout here means the bridge accepted the socket
    # but never answered → genuinely wedged.
    try:
        with sock:
            sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
            sock.settimeout(read_timeout)
            line = sock.makefile("r", encoding="utf-8").readline()
        return "ok", line.strip()
    except socket.timeout:
        return "wedged", ""
    except OSError:
        return "offline", ""


def main(argv: list[str]) -> int:
    event_type = argv[1] if len(argv) > 1 else "raw"
    agent = argv[2] if len(argv) > 2 else "claude-code"

    try:
        raw = sys.stdin.read().strip()
    except Exception:  # noqa: BLE001 - never let a stdin decode error block CC
        return _passthrough()
    try:
        payload = json.loads(raw) if raw else {}
    except json.JSONDecodeError:
        payload = {"text": raw}

    payload.setdefault("type", event_type)
    payload.setdefault("agent", agent)
    payload.setdefault("pid", os.getpid())
    payload.setdefault("cwd", os.getcwd())

    _enrich_stop(payload, event_type)

    read_timeout = PROMPT_TIMEOUT if event_type == "pre_tool_use" else DEFAULT_TIMEOUT
    sid = str(payload.get("session_id", ""))[:10]

    if not _forward_allowed(payload):
        _dlog(f"{event_type} {sid} FILTERED(targeted-debug) -> passthrough")
        return _passthrough()

    state = _state_load()

    # If we just auto-started the bridge, it isn't listening yet — don't pay the
    # connect timeout on every event during boot; pass straight through.
    if (time.time() - float(state.get("start_ts", 0.0) or 0.0)) < AUTOSTART_GRACE_S:
        _dlog(f"{event_type} {sid} autostart-grace -> passthrough")
        return _passthrough()

    # A wedged bridge tripped the breaker recently — skip quietly (no spam).
    if _cb_is_open(state):
        _dlog(f"{event_type} {sid} CB_OPEN -> drop (silent)")
        return _passthrough()

    # Liveness pid — attached only when we're actually about to forward
    # (the ancestor walk costs ~1-3 ms; skip it on the fast-fail paths above).
    payload.setdefault("agent_pid", _find_agent_pid())

    outcome, line = _forward(payload, read_timeout)

    if outcome == "ok":
        _cb_clear(state)
        # Recovery: only speak if we were previously offline/wedged.
        show = _note(state, "online", "✓ 仪表盘已连接" if state.get("conn") not in (
            "online", "unknown", None) else "")
        _state_save(state)
        _dlog(f"{event_type} {sid} OK resp={line[:40] or 'EMPTY'}")
        # The bridge's reply IS the hook response (may carry hookSpecificOutput
        # for gate decisions). Attach a recovery note without clobbering it.
        if show and line:
            try:
                obj = json.loads(line)
                if isinstance(obj, dict) and "systemMessage" not in obj:
                    obj["systemMessage"] = show
                    print(json.dumps(obj))
                    return 0
            except json.JSONDecodeError:
                pass
        print(line or json.dumps({"continue": True}))
        return 0

    if outcome == "offline":
        result = _maybe_autostart(state)
        if result == "started":
            msg = "仪表盘桥接未运行，正在自动启动…（几秒后自动接管，无需手动干预）"
        elif result == "failed":
            msg = ("仪表盘桥接自动启动失败，请手动运行："
                   "python tools/claude_buddy_bridge.py serve")
        elif result == "disabled":
            msg = "仪表盘桥接离线（自动启动已关闭）"
        else:  # cooldown — a start is already in flight; stay quiet mostly
            msg = ""
        show = _note(state, "offline", msg)
        _state_save(state)
        _dlog(f"{event_type} {sid} OFFLINE autostart={result}")
        return _passthrough(show)

    # wedged: connected but the daemon never replied. Feed the breaker so we
    # stop stalling every tool call, and nudge the user once.
    _cb_record_timeout(state)
    show = _note(state, "wedged", "仪表盘桥接无响应，正在重试…")
    _state_save(state)
    _dlog(f"{event_type} {sid} WEDGED({read_timeout}s)")
    return _passthrough(show)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Exception as e:  # noqa: BLE001 - the hook must NEVER take CC down
        # Absolute last resort: whatever went wrong, don't block the agent.
        try:
            print(json.dumps({"continue": True}))
        except Exception:
            pass
        _dlog(f"main() crashed (caught): {e}")
        sys.exit(0)
