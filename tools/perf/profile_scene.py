#!/usr/bin/env python3
"""profile_scene.py — sweep every scene, measure fps + heap delta.

This script is the per-scene counterpart to bench_firmware.py. It walks
through every scene that exists in `main/scenes/*.c`, for each one:
  1. pushes the inputs that force the device into that scene,
  2. waits `--settle-s` seconds for LVGL to redraw,
  3. polls `dash health` `--samples` times,
  4. records fps + heap_free,
  5. moves on to the next scene.

Result JSON tabulates per-scene fps + heap delta, useful for spotting
which scene is the heap hog before v0.7.0's 8-agent grid lands.

Scene-switching strategy:
  Today there is no `dash scene <id>` verb (see PERFORMANCE.md
  "Implementation note" — HARNESS_GAPS would call this G-PERF1).
  Until F2 adds one, we coerce scenes via the side-effecting verbs
  the firmware already exposes:
    idle      → `dash idle`
    dashboard → `dash snapshot {agents:[{kind:claude-code,...}]}`
    sessions  → `dash snapshot {agents:[{...},{...}]}`  (≥2 agents)
    prompt    → `dash prompt {...}`
    tokens    → `dash snapshot` then `dash tokens` (best-effort; the
                scene is usually entered by the user via the encoder)
    status    → cannot be forced from the wire today
                — emits "scene_force_unsupported" in the result.

Defaults to TCP mock at 127.0.0.1:9876.

Usage:
    python tools/perf/profile_scene.py
    python tools/perf/profile_scene.py --settle-s 3 --samples 5 -v
"""

from __future__ import annotations

import argparse
import json
import socket
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# Reuse the host-port parser from bench_firmware. We do NOT reuse
# HealthClient because the mock has listen(1) backlog and the firmware
# console accepts one session at a time; we must multiplex pushes and
# health on a single socket. See ScenePerfClient below.
THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))
from bench_firmware import _parse_host_port  # noqa: E402

RESULTS_DIR = THIS_DIR / "results"

# Scene catalogue (mirrors main/scenes/*.c). The "force" entry is the
# wire sequence we send before sampling. `None` means the scene can't
# be forced from the wire today.
SCENES = [
    {
        "id": "idle",
        "force": [("dash idle", None)],
    },
    {
        "id": "dashboard",
        "force": [
            ("dash snapshot", {
                "agents": [{
                    "kind": "claude-code", "session_id": "perf01",
                    "status": "running", "msg": "perf probe",
                    "entries": [], "tokens": 100, "tokens_today": 100,
                }],
            }),
        ],
    },
    {
        "id": "sessions",
        "force": [
            ("dash snapshot", {
                "agents": [
                    {
                        "kind": "claude-code", "session_id": "perf01",
                        "status": "running", "msg": "left",
                        "entries": [
                            {"t": "10:00", "tool": "Read",  "summary": "Read foo.c"},
                            {"t": "10:01", "tool": "Edit",  "summary": "Edit foo.c"},
                            {"t": "10:02", "tool": "Bash",  "summary": "make test"},
                        ],
                        "tokens": 1000, "tokens_today": 1000,
                    },
                    {
                        "kind": "codex", "session_id": "perf02",
                        "status": "waiting", "msg": "right",
                        "entries": [
                            {"t": "10:03", "tool": "Write", "summary": "Write bar.c"},
                            {"t": "10:04", "tool": "Grep",  "summary": "Grep TODO"},
                        ],
                        "tokens": 800, "tokens_today": 800,
                    },
                ],
            }),
        ],
    },
    {
        "id": "prompt",
        "force": [
            ("dash prompt", {
                "id": "perf-prompt-1",
                "tool": "Bash",
                "hint": "perf probe — auto-deny",
                "agent_kind": "claude-code",
                "session_id": "perf01",
            }),
        ],
    },
    {
        "id": "tokens",
        # Tokens scene is selected by encoder rotation, not the wire.
        # We push a tokens spark sample so the scene would have data
        # if a human switches to it; sample anyway from whatever scene
        # is current to capture a "tokens-payload-applied" heap baseline.
        "force": [
            ("dash tokens", {
                "kind": "claude-code", "session_id": "perf01",
                "cumulative": 12345, "today": 234, "sample": 42,
            }),
        ],
        "scene_force_unsupported": True,
    },
    {
        "id": "status",
        "force": [],
        "scene_force_unsupported": True,
    },
]


class ScenePerfClient:
    """Single-socket TCP client used for both wire pushes AND `dash
    health` sampling. The mock listens with backlog=1, so we cannot
    afford two parallel connections from this script — they would
    queue and the second one would only fire after the first closes.
    Real firmware behaves similarly (one console session at a time)."""

    def __init__(self, host: str, port: int, *, verbose: bool):
        self.host = host
        self.port = port
        self.verbose = verbose
        self._sock: socket.socket | None = None
        self._buf = b""
        self.failures = 0

    def _log(self, *a):
        if self.verbose:
            print("[profile_scene]", *a, file=sys.stderr, flush=True)

    def _ensure(self) -> bool:
        if self._sock:
            return True
        try:
            self._sock = socket.create_connection((self.host, self.port), timeout=2.0)
            return True
        except OSError as e:
            self._log("connect failed:", e)
            self._sock = None
            return False

    def _read_line(self, deadline: float) -> str | None:
        while b"\n" not in self._buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                self._sock.settimeout(max(0.05, remaining))
                chunk = self._sock.recv(4096)
            except (socket.timeout, OSError):
                return None
            if not chunk:
                return None
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return line.decode("utf-8", errors="replace").rstrip("\r")

    def send(self, verb: str, payload: dict | None) -> bool:
        if not self._ensure():
            return False
        if payload is None:
            line = verb + "\n"
        else:
            line = f'{verb} {json.dumps(payload, separators=(",", ":"))}\n'
        try:
            self._sock.sendall(line.encode("utf-8"))
        except OSError as e:
            self._log("send failed:", e)
            self.close()
            return False
        # Drain one OK:/ERR: reply. `dash health` triggers a multi-line
        # envelope, so handle that specially via sample().
        text = self._read_line(time.monotonic() + 1.5)
        if text is None:
            return False
        self._log("  reply:", text[:80])
        return text.startswith("OK:")

    def sample(self) -> dict | None:
        """Send `dash health` and parse the HEALTH_BEGIN/_END envelope."""
        if not self._ensure():
            self.failures += 1
            return None
        try:
            self._sock.sendall(b"dash health\n")
        except OSError as e:
            self._log("send health failed:", e)
            self.close()
            self.failures += 1
            return None
        deadline = time.monotonic() + 2.0
        body = None
        for _ in range(8):
            line = self._read_line(deadline)
            if line is None:
                break
            if line.startswith("HEALTH_BEGIN"):
                body_line = self._read_line(deadline)
                if body_line is None:
                    break
                body = body_line
                _ = self._read_line(deadline)  # HEALTH_END
                break
        if body is None:
            self.failures += 1
            return None
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            self.failures += 1
            return None

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
            self._buf = b""


def _profile_scene(scene: dict, client: ScenePerfClient,
                   settle_s: float, samples: int, verbose: bool) -> dict:
    sid = scene["id"]
    if verbose:
        print(f"[profile_scene] → scene={sid}", file=sys.stderr)
    # Apply force sequence
    force_ok = True
    for verb, payload in scene["force"]:
        ok = client.send(verb, payload)
        force_ok = force_ok and ok
    # Settle, then sample
    if settle_s > 0:
        time.sleep(settle_s)

    health_before = client.sample()
    fps_samples, heap_samples = [], []
    for i in range(samples):
        s = client.sample()
        if s is None:
            continue
        if "fps" in s:        fps_samples.append(s["fps"])
        if "heap_free" in s:  heap_samples.append(s["heap_free"])
        if i < samples - 1:
            time.sleep(0.25)
    health_after = client.sample()

    heap_delta = None
    if health_before and health_after:
        h0 = health_before.get("heap_free")
        h1 = health_after.get("heap_free")
        if h0 is not None and h1 is not None:
            heap_delta = h1 - h0

    return {
        "scene": sid,
        "force_ok": force_ok,
        "scene_force_unsupported": bool(scene.get("scene_force_unsupported")),
        "samples": samples,
        "fps_min":   min(fps_samples) if fps_samples else None,
        "fps_mean":  round(statistics.mean(fps_samples), 2) if fps_samples else None,
        "fps_max":   max(fps_samples) if fps_samples else None,
        "heap_free_min":  min(heap_samples) if heap_samples else None,
        "heap_free_mean": round(statistics.mean(heap_samples), 1) if heap_samples else None,
        "heap_free_max":  max(heap_samples) if heap_samples else None,
        "heap_delta_observed": heap_delta,
        "scene_reported_before": (health_before or {}).get("scene"),
        "scene_reported_after":  (health_after  or {}).get("scene"),
    }


def run(args) -> int:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    host, port = _parse_host_port(args.port)
    client = ScenePerfClient(host, port, verbose=args.verbose)

    started = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    print(
        f"[profile_scene] sweeping {len(SCENES)} scenes @ {host}:{port} "
        f"(settle={args.settle_s}s, samples={args.samples})",
        file=sys.stderr,
    )

    rows = []
    for scene in SCENES:
        row = _profile_scene(
            scene, client,
            settle_s=args.settle_s, samples=args.samples,
            verbose=args.verbose,
        )
        rows.append(row)
        marker = ""
        if row["scene_force_unsupported"]:
            marker = " (force unsupported)"
        fps = row["fps_mean"]
        heap = row["heap_free_mean"]
        print(
            f"  {row['scene']:10s}  fps={fps}  heap_free={heap}  "
            f"delta={row['heap_delta_observed']}{marker}",
            file=sys.stderr,
        )

    client.close()

    out = {
        "schema": "profile_scene.v1",
        "started_utc": started,
        "endpoint": f"{host}:{port}",
        "settle_s": args.settle_s,
        "samples_per_scene": args.samples,
        "rows": rows,
        "notes": [
            "tokens / status scenes are entered via the rotary encoder, "
            "not the wire. Their rows reflect whatever scene the device "
            "was on after the force-attempt — see scene_reported_after.",
            "heap_delta_observed compares one sample before forcing vs "
            "one sample after sampling, so it includes the scene's init "
            "+ first-tick allocations. Non-zero is expected on first run.",
        ],
    }
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d-%H%M%S")
    label_suffix = f"-{args.label}" if args.label else ""
    out_path = RESULTS_DIR / f"profile_scene-{stamp}{label_suffix}.json"
    out_path.write_text(json.dumps(out, indent=2))
    print(f"\nsaved → {out_path}", file=sys.stderr)

    # Compact stdout summary
    table = [
        {"scene": r["scene"], "fps_mean": r["fps_mean"],
         "heap_free_mean": r["heap_free_mean"],
         "heap_delta": r["heap_delta_observed"]}
        for r in rows
    ]
    print(json.dumps(table, indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="profile_scene",
        description="Per-scene fps + heap profile via dash health.",
    )
    p.add_argument("--port", default="127.0.0.1:9876")
    p.add_argument("--settle-s", type=float, default=1.0,
                   help="seconds to wait after force-switching scene (default 1.0)")
    p.add_argument("--samples", type=int, default=3,
                   help="health samples per scene (default 3)")
    p.add_argument("--label", default="")
    p.add_argument("-v", "--verbose", action="store_true")
    return run(p.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
