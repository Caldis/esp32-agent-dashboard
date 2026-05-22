#!/usr/bin/env python3
"""bench_firmware.py — sample `dash health` from a running device or mock.

Polls `dash health` every `--period-s` seconds for `--duration-s` seconds,
collecting fps + heap_free trajectories and emitting a JSON summary with
histograms, percentiles, and drift indicators.

Defaults to the TCP mock at 127.0.0.1:9876 so this can run on any dev
box. Pass `--port-kind serial --port COM9` for a real device (note:
that requires `tools/claude_buddy_bridge.py serve` running so the
serial pusher is the one that holds COM9 — bench_firmware should not
fight the bridge for the port; see the "Implementation note" in
PERFORMANCE.md for the gap this surfaces).

Output schema (`tools/perf/results/firmware-<ts>.json`):

    {
      "schema": "bench_firmware.v1",
      "started_utc": "...",
      "duration_s": 60,
      "period_s": 1.0,
      "samples": [{"t_s": 0.0, "fps": 33.4, "heap_free": 84200, ...}, ...],
      "fps": {"min": ..., "max": ..., "mean": ..., "p50": ..., "p95": ..., "hist": {...}},
      "heap_free": {"min": ..., "max": ..., "mean": ..., "p50": ..., "p95": ..., "delta_total": ...},
      "drift": {"heap_free_per_min": ..., "fps_slope": ...}
    }

Usage:
    python tools/perf/bench_firmware.py                    # 60s @ 1Hz, mock
    python tools/perf/bench_firmware.py --duration-s 300   # 5 min sample
    python tools/perf/bench_firmware.py --port 127.0.0.1:9876 -v
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import socket
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
RESULTS_DIR = THIS_DIR / "results"

# Health reply schema (mirrors mock_device_v1.py / future firmware
# cmd_health). Numeric fields we care about for perf tracking:
NUMERIC_FIELDS = (
    "fps",
    "heap_free",
    "heap_min",
    "uptime_s",
    "snapshots_received",
    "prompts_received",
    "decisions_sent",
    "agent_count",
)


def _ensure_results_dir() -> None:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)


def _parse_host_port(s: str) -> tuple[str, int]:
    host, _, port = s.rpartition(":")
    return host or "127.0.0.1", int(port)


class HealthClient:
    """Tiny line-buffered TCP client. Sends `dash health`, reads back the
    payload-follows envelope, parses the JSON body, returns the dict.

    Reconnects on transport error; samples that fail return None and are
    counted toward `failures` but don't abort the run.
    """

    def __init__(self, host: str, port: int, *, verbose: bool):
        self.host = host
        self.port = port
        self.verbose = verbose
        self._sock: socket.socket | None = None
        self._buf = b""
        self.failures = 0

    def _log(self, *a):
        if self.verbose:
            print("[bench_fw]", *a, file=sys.stderr, flush=True)

    def _connect(self) -> bool:
        self._close()
        try:
            self._sock = socket.create_connection(
                (self.host, self.port), timeout=2.0
            )
            return True
        except OSError as e:
            self._log("connect failed:", e)
            self._sock = None
            return False

    def _close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._buf = b""

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
        return line.decode("utf-8", errors="replace")

    def sample(self) -> dict | None:
        if not self._sock and not self._connect():
            self.failures += 1
            return None
        try:
            self._sock.sendall(b"dash health\n")
        except OSError as e:
            self._log("send failed:", e)
            self._close()
            self.failures += 1
            return None
        deadline = time.monotonic() + 2.0
        # Expect: "OK: payload follows tag=HEALTH"
        #         "HEALTH_BEGIN fmt=json bytes=NNN"
        #         "<json>"
        #         "HEALTH_END"
        body = None
        for _ in range(8):  # bound the search
            line = self._read_line(deadline)
            if line is None:
                break
            line = line.rstrip("\r")
            if line.startswith("HEALTH_BEGIN"):
                body_line = self._read_line(deadline)
                if body_line is None:
                    break
                body = body_line.rstrip("\r")
                # Eat HEALTH_END if present (best effort)
                _ = self._read_line(deadline)
                break
        if body is None:
            self._log("no HEALTH_BEGIN; rotating connection")
            self._close()
            self.failures += 1
            return None
        try:
            return json.loads(body)
        except json.JSONDecodeError as e:
            self._log("malformed health json:", e, body[:120])
            self.failures += 1
            return None

    def close(self) -> None:
        self._close()


def _histogram(values: list[float], buckets: list[float]) -> dict:
    """Return a count-per-bucket dict, edges given as upper bounds
    (last bucket is `+inf`). Used for fps / heap distributions."""
    if not values:
        return {}
    out = collections.OrderedDict()
    for b in buckets:
        out[f"<{b}"] = 0
    out[f">={buckets[-1]}"] = 0
    for v in values:
        placed = False
        for b in buckets:
            if v < b:
                out[f"<{b}"] += 1
                placed = True
                break
        if not placed:
            out[f">={buckets[-1]}"] += 1
    return dict(out)


def _summarise(values: list[float], *, hist_buckets: list[float] | None = None) -> dict:
    if not values:
        return {"count": 0}
    s = sorted(values)
    n = len(s)
    out = {
        "count": n,
        "min": round(min(s), 2),
        "max": round(max(s), 2),
        "mean": round(statistics.mean(s), 2),
        "stdev": round(statistics.pstdev(s), 2) if n > 1 else 0.0,
        "p50": round(s[n // 2], 2),
        "p95": round(s[min(n - 1, int(math.ceil(0.95 * n) - 1))], 2),
    }
    if hist_buckets:
        out["hist"] = _histogram(values, hist_buckets)
    return out


def _linear_slope(xs: list[float], ys: list[float]) -> float:
    """Tiny least-squares slope. Returns 0 on degenerate input."""
    n = len(xs)
    if n < 2:
        return 0.0
    mx = statistics.mean(xs)
    my = statistics.mean(ys)
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den else 0.0


def run(args) -> int:
    _ensure_results_dir()
    host, port = _parse_host_port(args.port)
    client = HealthClient(host, port, verbose=args.verbose)

    started = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    print(
        f"[bench_fw] sampling {host}:{port} every {args.period_s}s "
        f"for {args.duration_s}s",
        file=sys.stderr,
    )
    samples: list[dict] = []
    t_start = time.monotonic()
    next_sample = t_start
    deadline = t_start + args.duration_s

    while True:
        now = time.monotonic()
        if now >= deadline:
            break
        if now < next_sample:
            time.sleep(min(0.05, next_sample - now))
            continue
        t_rel = now - t_start
        s = client.sample()
        if s is not None:
            row = {"t_s": round(t_rel, 3)}
            for k in NUMERIC_FIELDS:
                if k in s:
                    row[k] = s[k]
            samples.append(row)
            if args.verbose:
                print(
                    f"  t={t_rel:5.1f}s  fps={s.get('fps'):>5}  "
                    f"heap_free={s.get('heap_free'):>7}  "
                    f"scene={s.get('scene','?')}",
                    file=sys.stderr,
                )
        next_sample += args.period_s

    client.close()

    fps_vals = [s["fps"] for s in samples if "fps" in s]
    heap_vals = [s["heap_free"] for s in samples if "heap_free" in s]
    heap_min_vals = [s["heap_min"] for s in samples if "heap_min" in s]
    ts = [s["t_s"] for s in samples]

    heap_delta = heap_vals[-1] - heap_vals[0] if len(heap_vals) >= 2 else 0
    span_min = max((ts[-1] - ts[0]) / 60.0, 1e-9) if len(ts) >= 2 else 1e-9
    heap_per_min = (heap_delta / span_min) if heap_vals else 0.0
    fps_slope = _linear_slope(ts, fps_vals) if fps_vals else 0.0

    out = {
        "schema": "bench_firmware.v1",
        "started_utc": started,
        "endpoint": f"{host}:{port}",
        "duration_s": args.duration_s,
        "period_s": args.period_s,
        "samples_total": len(samples),
        "sample_failures": client.failures,
        "samples": samples if args.include_samples else samples[-5:],  # last 5 as a tail
        "fps": _summarise(fps_vals, hist_buckets=[15, 20, 25, 28, 30, 32]),
        "heap_free": _summarise(heap_vals, hist_buckets=[40000, 60000, 75000, 85000, 100000]),
        "heap_min": _summarise(heap_min_vals),
        "drift": {
            "heap_free_per_min": round(heap_per_min, 1),
            "fps_slope_per_s": round(fps_slope, 4),
            "first_sample_t_s": ts[0] if ts else None,
            "last_sample_t_s": ts[-1] if ts else None,
        },
    }

    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d-%H%M%S")
    label_suffix = f"-{args.label}" if args.label else ""
    out_path = RESULTS_DIR / f"firmware-{stamp}{label_suffix}.json"
    out_path.write_text(json.dumps(out, indent=2))
    print(f"saved → {out_path}", file=sys.stderr)

    # Print a compact one-screen summary for humans.
    compact = {
        "samples_total": out["samples_total"],
        "sample_failures": out["sample_failures"],
        "fps":   {k: out["fps"].get(k)   for k in ("min", "p50", "mean", "max", "stdev")},
        "heap_free": {k: out["heap_free"].get(k) for k in ("min", "p50", "mean", "max", "stdev")},
        "drift": out["drift"],
    }
    print(json.dumps(compact, indent=2))
    return 0 if samples else 2


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="bench_firmware",
        description="Sample dash health to track fps / heap over time.",
    )
    p.add_argument("--port", default="127.0.0.1:9876",
                   help="TCP host:port (default 127.0.0.1:9876)")
    p.add_argument("--duration-s", type=int, default=60)
    p.add_argument("--period-s", type=float, default=1.0)
    p.add_argument("--include-samples", action="store_true",
                   help="store every raw sample in the JSON (default: tail only)")
    p.add_argument("--label", default="")
    p.add_argument("-v", "--verbose", action="store_true")
    return run(p.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
