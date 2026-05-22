"""tools.perf — repeatable performance benchmarks for the v0.7.0 cycle.

Three entry points:
- bench_bridge.py — host-side event-handling throughput (wraps
  `claude_buddy_bridge.py bench`).
- bench_firmware.py — fps + heap_free sampling via `dash health`.
- profile_scene.py — per-scene fps + heap delta sweep.

All scripts default to the TCP mock at 127.0.0.1:9876 so they can run
on any dev box without COM9 / a real device. Each saves a JSON result
file under `tools/perf/results/`; the bench script's `compare` command
diffs the two latest results.

PERF1 owns this directory.
"""
