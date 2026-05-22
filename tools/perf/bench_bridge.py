#!/usr/bin/env python3
"""bench_bridge.py — repeatable host-bridge benchmark suite.

Extends `claude_buddy_bridge.py bench` (which is a single-shot bench)
with:
  - parametric sweeps (events / throttle / pace),
  - results persisted as JSON under tools/perf/results/,
  - `compare` command that diffs the latest two runs.

Default target is the TCP mock at 127.0.0.1:9876, so this can run on
any dev box without COM9. The bench runs `--dry-run` internally — the
goal here is to measure host-side throughput, not device wire RTT.
See bench_firmware.py for the device-side counterpart.

Usage:
    python tools/perf/bench_bridge.py run               # default sweep
    python tools/perf/bench_bridge.py run --quick       # 1 cell, faster
    python tools/perf/bench_bridge.py run --label v0.7  # tag the result
    python tools/perf/bench_bridge.py list              # show saved runs
    python tools/perf/bench_bridge.py compare           # latest vs prev
    python tools/perf/bench_bridge.py compare A.json B.json

Exit code is non-zero if any sweep cell underperforms the floor
configured in `THROUGHPUT_FLOOR_EPS` (default 100 ev/s) — CI can use
this as a regression gate once v0.7.0 lands.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

# Paths
THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent
RESULTS_DIR = THIS_DIR / "results"
BRIDGE_PY = REPO_ROOT / "tools" / "claude_buddy_bridge.py"

# Floors (regression gate). Bridge bench at full speed should clear
# these on any modern dev box; if a future change drops below, that's
# a regression we want CI to scream about.
THROUGHPUT_FLOOR_EPS = 100.0       # events/s
MEDIAN_EVENT_CEIL_US = 100.0       # per-event handle time, p50

# Default sweep — three cells that span the realistic space:
#   - 1k events full-speed: stress one push window
#   - 5k events full-speed: amortise warm-up, near steady-state
#   - 10k events full-speed (no pace): the roadmap's "10k events/min"
#     target measured as a pure throughput question
DEFAULT_SWEEP = [
    {"events":  1000, "pace_ms": 0, "throttle_ms":  250},
    {"events":  5000, "pace_ms": 0, "throttle_ms":  250},
    {"events": 10000, "pace_ms": 0, "throttle_ms":  250},
]

# Extended sweep — opt-in via --extended. Adds a paced cell that
# simulates realistic 10k/min arrival; takes ~60s on its own.
EXTENDED_SWEEP = DEFAULT_SWEEP + [
    {"events": 10000, "pace_ms": 6, "throttle_ms":  250},   # ~10k/min
    {"events":  2000, "pace_ms": 0, "throttle_ms":  500},   # slower throttle
]

# Quick sweep for "is the box healthy?" sanity checks.
QUICK_SWEEP = [
    {"events": 1000, "pace_ms": 0, "throttle_ms": 250},
]


def _ensure_results_dir() -> None:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)


def _run_one_cell(cell: dict, *, tcp_port: str) -> dict:
    """Spawn `claude_buddy_bridge.py bench` and parse its JSON stdout."""
    cmd = [
        sys.executable, str(BRIDGE_PY), "bench",
        "--events", str(cell["events"]),
        "--pace-ms", str(cell["pace_ms"]),
        "--throttle-ms", str(cell["throttle_ms"]),
        "--port-kind", "tcp",
        "--port", tcp_port,
    ]
    t0 = time.monotonic()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    wall = time.monotonic() - t0

    # The bench prints a stream of "[DRY] dash snapshot ..." lines then
    # a JSON block at the end. We snip from the last `{` to EOF.
    out = proc.stdout
    last_brace = out.rfind("{\n")
    if last_brace < 0:
        last_brace = out.rfind("{")
    if last_brace < 0:
        raise RuntimeError(
            f"bench did not emit JSON\nstdout:\n{out[-400:]}\nstderr:\n{proc.stderr[-400:]}"
        )
    try:
        result = json.loads(out[last_brace:])
    except json.JSONDecodeError as e:
        raise RuntimeError(
            f"bench JSON unparseable: {e}\nstdout tail:\n{out[-400:]}"
        ) from e

    result["wall_s"] = round(wall, 3)
    result["cmd_exit"] = proc.returncode
    return result


def _platform_meta() -> dict:
    return {
        "python": platform.python_version(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "host": platform.node(),
    }


def cmd_run(args) -> int:
    _ensure_results_dir()
    if args.quick:
        sweep = QUICK_SWEEP
    elif args.extended:
        sweep = EXTENDED_SWEEP
    else:
        sweep = DEFAULT_SWEEP
    if args.events:
        # Single-cell override
        sweep = [{
            "events": args.events,
            "pace_ms": args.pace_ms,
            "throttle_ms": args.throttle_ms,
        }]

    print(f"[bench_bridge] running {len(sweep)} cell(s) → {args.port}", file=sys.stderr)
    cells = []
    started = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    fail = False
    for i, spec in enumerate(sweep, 1):
        print(
            f"  [{i}/{len(sweep)}] events={spec['events']} "
            f"pace_ms={spec['pace_ms']} throttle_ms={spec['throttle_ms']}",
            file=sys.stderr,
        )
        try:
            result = _run_one_cell(spec, tcp_port=args.port)
        except Exception as e:
            print(f"    FAIL: {e}", file=sys.stderr)
            cells.append({"spec": spec, "error": str(e)})
            fail = True
            continue
        cell = {**spec, **result}
        cells.append(cell)
        eps = result.get("events_per_s") or 0.0
        med = result.get("median_event_us") or 0.0
        marker = ""
        if eps < THROUGHPUT_FLOOR_EPS:
            marker += " UNDER_FLOOR"
            fail = True
        if med > MEDIAN_EVENT_CEIL_US:
            marker += " OVER_CEIL"
            fail = True
        print(
            f"    ev/s={eps:7.1f}  p50={med:5.1f}us  "
            f"pushes={result.get('snapshot_pushes')}  elapsed={result.get('elapsed_s')}s{marker}",
            file=sys.stderr,
        )

    summary = {
        "schema": "bench_bridge.v1",
        "started_utc": started,
        "label": args.label,
        "platform": _platform_meta(),
        "sweep": cells,
        "summary": _summarise(cells),
    }

    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d-%H%M%S")
    out_path = RESULTS_DIR / f"bench-{stamp}.json"
    if args.label:
        out_path = RESULTS_DIR / f"bench-{stamp}-{args.label}.json"
    out_path.write_text(json.dumps(summary, indent=2))
    print(f"\nsaved → {out_path}", file=sys.stderr)
    print(json.dumps(summary["summary"], indent=2))
    return 1 if fail else 0


def _summarise(cells: list[dict]) -> dict:
    eps_vals = [c.get("events_per_s") for c in cells if c.get("events_per_s")]
    med_vals = [c.get("median_event_us") for c in cells if c.get("median_event_us")]
    p95_vals = [c.get("p95_event_us") for c in cells if c.get("p95_event_us")]
    out = {
        "cells_total": len(cells),
        "cells_with_result": len(eps_vals),
    }
    if eps_vals:
        out["events_per_s_min"] = round(min(eps_vals), 1)
        out["events_per_s_max"] = round(max(eps_vals), 1)
        out["events_per_s_mean"] = round(statistics.mean(eps_vals), 1)
    if med_vals:
        out["median_event_us_mean"] = round(statistics.mean(med_vals), 1)
    if p95_vals:
        out["p95_event_us_max"] = round(max(p95_vals), 1)
    return out


def _list_runs() -> list[Path]:
    if not RESULTS_DIR.exists():
        return []
    return sorted(RESULTS_DIR.glob("bench-*.json"))


def cmd_list(args) -> int:
    runs = _list_runs()
    if not runs:
        print("(no runs)", file=sys.stderr)
        return 0
    for p in runs:
        try:
            obj = json.loads(p.read_text())
            s = obj.get("summary", {})
            label = obj.get("label") or ""
            print(
                f"{p.name:60s}  ev/s_mean={s.get('events_per_s_mean','-'):>7}  "
                f"p50us_mean={s.get('median_event_us_mean','-'):>6}  {label}"
            )
        except (OSError, json.JSONDecodeError):
            print(f"{p.name}  (unreadable)")
    return 0


def _load(path: Path) -> dict:
    return json.loads(path.read_text())


def _delta(a: float | None, b: float | None) -> str:
    if a is None or b is None:
        return "-"
    if a == 0:
        return "+inf%" if b else "0%"
    pct = (b - a) / a * 100.0
    sign = "+" if pct >= 0 else ""
    return f"{sign}{pct:.1f}%"


def cmd_compare(args) -> int:
    if args.a and args.b:
        a_path = Path(args.a)
        b_path = Path(args.b)
    else:
        runs = _list_runs()
        if len(runs) < 2:
            print("need at least 2 runs to compare", file=sys.stderr)
            return 2
        a_path, b_path = runs[-2], runs[-1]
    a, b = _load(a_path), _load(b_path)
    print(f"A: {a_path.name}  ({a.get('started_utc')})  label={a.get('label','')}")
    print(f"B: {b_path.name}  ({b.get('started_utc')})  label={b.get('label','')}")
    sa, sb = a.get("summary", {}), b.get("summary", {})
    keys = [
        "events_per_s_min", "events_per_s_mean", "events_per_s_max",
        "median_event_us_mean", "p95_event_us_max",
    ]
    print(f"\n{'metric':28s}  {'A':>10s}  {'B':>10s}  {'delta':>10s}")
    print("-" * 64)
    for k in keys:
        av, bv = sa.get(k), sb.get(k)
        print(f"{k:28s}  {str(av):>10s}  {str(bv):>10s}  {_delta(av, bv):>10s}")
    # Per-cell view
    print("\nper-cell:")
    cells_a = {(c.get("events"), c.get("pace_ms"), c.get("throttle_ms")): c for c in a.get("sweep", [])}
    cells_b = {(c.get("events"), c.get("pace_ms"), c.get("throttle_ms")): c for c in b.get("sweep", [])}
    keys_combined = sorted(set(cells_a) | set(cells_b))
    for key in keys_combined:
        ca, cb = cells_a.get(key), cells_b.get(key)
        ea = (ca or {}).get("events_per_s")
        eb = (cb or {}).get("events_per_s")
        ev, pace, thr = key
        print(f"  events={ev} pace_ms={pace} throttle_ms={thr}: "
              f"A={ea} B={eb} delta={_delta(ea, eb)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="bench_bridge",
        description="Repeatable host-bridge throughput benchmark.",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="run the benchmark sweep")
    p_run.add_argument("--port", default="127.0.0.1:9876",
                       help="TCP mock host:port (default 127.0.0.1:9876)")
    p_run.add_argument("--quick", action="store_true",
                       help="single small cell (sanity check)")
    p_run.add_argument("--extended", action="store_true",
                       help="extended sweep with paced + slow-throttle cells (~90s)")
    p_run.add_argument("--events", type=int, default=0,
                       help="override sweep with single cell, this event count")
    p_run.add_argument("--pace-ms", type=int, default=0,
                       help="(with --events) pace between events")
    p_run.add_argument("--throttle-ms", type=int, default=250,
                       help="(with --events) snapshot publisher throttle")
    p_run.add_argument("--label", default="",
                       help="tag the result filename (e.g. v0.7-pre)")

    p_list = sub.add_parser("list", help="show saved runs")

    p_cmp = sub.add_parser("compare", help="diff two runs (default: last two)")
    p_cmp.add_argument("a", nargs="?", default=None)
    p_cmp.add_argument("b", nargs="?", default=None)

    args = p.parse_args(argv)
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "list":
        return cmd_list(args)
    if args.cmd == "compare":
        return cmd_compare(args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
