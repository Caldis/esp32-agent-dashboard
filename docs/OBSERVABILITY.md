# Observability runbook

What to look at when the dashboard goes weird. Ordered roughly by
"things you'll need first" — start at the top, work down only if the
earlier signal didn't tell you enough.

If you're new here and just want to verify the device is alive,
skip to §2 (`dash health`).

---

## 1. Signals at a glance

| Signal | Where | Cost | Tells you |
|---|---|---|---|
| Bridge log | stderr of `claude_buddy_bridge.py` | free | hook ingestion + serial round-trip |
| `dash health` | one console call to the device | ~10 ms | device-side rolling stats |
| `dash health --jsonl` | one console call | ~10 ms | last N telemetry samples (30 s each) |
| `EVT:` stream | bridge stdout | free | async events: scene changes, low_heap, crash_dump_available |
| Crash dump | `dash dump crash` | ~50 ms | last panic reason + tail of EVT log |
| Grafana | `~/.claude-buddy/telemetry/*.jsonl` → Grafana | one-time setup | cross-device trends |

## 2. `dash health` — the first thing to check

```bash
$ dash health
OK: payload follows tag=HEALTH
=== BEGIN HEALTH fmt=json bytes=378 ===
{"device_name":"Clawd","owner":"alice","scene":"dashboard",
 "theme":"warm","uptime_s":18342,"heap_free":172864,
 "heap_min":138224,"snapshots_received":4821,"prompts_received":3,
 "decisions_sent":3,"connection_age_s":2,"agent_count":2,
 "fps_p50":30,"fps_p95":28,"telemetry_samples":61}
=== END HEALTH ===
```

What each field means:

- **`scene`** — current scene id. If this is stuck on `idle` while
  the bridge claims to be pushing snapshots → connection is dead,
  see §3.
- **`uptime_s`** — seconds since boot. A small number after you
  *didn't* reboot is the loudest possible "I crashed" signal.
- **`heap_free`** — bytes free in internal RAM, *right now*.
- **`heap_min`** — lowest `heap_free` has ever been since boot.
  If `heap_min` is within ~30 KB of zero you're one bad snapshot
  away from a panic.
- **`connection_age_s`** — seconds since the last accepted
  `dash snapshot` (or other dash command). Should normally be
  under 11 (10 s keepalive + slack).
- **`agent_count`** — slots currently `in_use`. If this disagrees
  with what the bridge thinks it pushed → bridge-side bug, not
  device-side.
- **`telemetry_samples`** — entries in the rolling ring buffer
  (max `TELEMETRY_RING_LEN`). At a 30 s sample interval the ring
  spans the last ~30 minutes.
- **`fps_p50` / `fps_p95`** — median / p95 of frame ticks recorded
  by `harness_record_frame()` over the ring window.

## 3. Bridge log — when the device looks frozen

```bash
$ tail -f ~/.claude-buddy/bridge.log
```

What to look for:

- `OK: applied` lines — the device accepted a snapshot.
- `ERR: …` lines — the device rejected something. The reason
  is the rest of the line.
- `EVT: scene_changed idx=N id=…` — device-driven scene change
  (idle ↔ dashboard).
- `EVT: low_heap free=N` — heap dropped under 50 KB at least once.
  This is debounced to once per 60 s; if you see it spam-repeating,
  something is leaking.
- `EVT: crash_dump_available bytes=N` — the device booted with a
  pending crash dump. **Fetch it before it gets overwritten**:
  `dash dump crash > crash-$(date +%s).json`
- `EVT: agent_added kind=… session_id=…` / `agent_removed` —
  slot table churn.
- Long gap with no traffic at all → check the serial port. On
  Windows: `Get-PnpDevice -Class Ports`. On macOS: `ls /dev/tty.usb*`.

## 4. Rolling telemetry samples — when `health` isn't enough

```bash
$ dash health --jsonl
OK: payload follows tag=TELEMETRY
=== BEGIN TELEMETRY fmt=jsonl bytes=2918 ===
{"t":18012,"heap_free":174208,"fps":30,"scene":"dashboard","agents":2}
{"t":18042,"heap_free":173824,"fps":30,"scene":"dashboard","agents":2}
{"t":18072,"heap_free":174208,"fps":30,"scene":"dashboard","agents":2}
{"t":18102,"heap_free":172864,"fps":29,"scene":"sessions","agents":2}
...
=== END TELEMETRY ===
```

Use this when:

- You suspect a slow leak and want to see if `heap_free` trends
  down across the window.
- You want to confirm a UI freeze: `fps` will drop to <5 while
  `t` (uptime) keeps advancing.
- You want to correlate scene transitions with heap pressure:
  the `scene` column comes from the same sample.

The ring buffer holds `TELEMETRY_RING_LEN` samples (default 60),
sampled every 30 s, so this covers the last ~30 minutes. Older
data has rolled off; if you need cross-day trends, that's what
Grafana is for (§7).

## 5. Crash dump retrieval

After a panic the firmware writes a structured record to the
`crashdump` NVS namespace before resetting. On the next boot it
emits:

```
EVT: crash_dump_available bytes=412
```

Fetch and clear:

```bash
$ dash dump crash
OK: payload follows tag=CRASHDUMP
=== BEGIN CRASHDUMP fmt=json bytes=412 ===
{
  "schema": "dash.crashdump/v1",
  "fw_version": "0.9.0",
  "boot_count_at_crash": 47,
  "uptime_s_at_crash": 9214,
  "reason": "panic",
  "panic_reason_code": 3,
  "panic_excvaddr": 0,
  "panic_text": "LoadProhibited",
  "task_name": "main",
  "stack_pc": "0x40080a14",
  "heap_free_at_crash": 18432,
  "evt_tail": [
    "scene_changed idx=2 id=sessions",
    "agent_added kind=codex session_id=cx_xyz",
    "low_heap free=32128"
  ]
}
=== END CRASHDUMP ===
```

Reading the dump *also clears it* — there's only one slot. If you
want to keep it, redirect to a file (`dash dump crash > crash.json`).

**Privacy**: the dump is local-only and never uploaded by the
bridge, even with `telemetry=on`. To share, gist it explicitly:

```bash
gh gist create -p crash.json
```

## 6. Grafana dashboard — long-term trends

See [`tools/grafana/README.md`](../tools/grafana/README.md) for the
three-step setup. The dashboard at `tools/grafana/dashboard.json`
shows, per device:

- **Uptime trend** — `uptime_s_max` over time; sawtooth = reboot.
- **Heap-free** — `heap_free_p50` and `heap_free_min` as two
  series; the gap between them is jitter.
- **FPS p50 / p95** — render-loop health.
- **Prompt latency histogram** — from bridge bench data, the
  bucketed `prompt_latency_buckets_ms` field.
- **Error rate** — `err_per_1k_ok`; a step up is the loudest
  "your bridge change broke something" signal.

## 7. When things really go wrong

Severity ladder:

1. **`scene_changed` events stop arriving** but bridge log shows
   pushes going out → device is frozen. `?ping` will time out.
   Power-cycle the device; on next boot `dash dump crash` may
   contain a watchdog reset record.
2. **`EVT: low_heap` repeats >5×/minute** → real leak. Diff
   `dash health` `heap_free` over a few minutes; expect to find
   a snapshot shape that grows unbounded.
3. **`?ping` works but `dash snapshot` returns `ERR:`** → JSON
   shape mismatch. Echo a known-good snapshot through the bridge:
   `tools/mock_device_v1.py --emit one-shot | bridge`.
4. **Device reboots in a loop** → bootloop. Connect `idf.py
   monitor` directly, bypass the bridge, look at the panic
   immediately before `rst:0x1`.
5. **Bridge log says `device not responding`** → serial port
   handoff. Make sure no other process holds the port
   (`fuser /dev/ttyUSB0` on Linux).

## 8. Privacy posture for operators

Everything in this runbook is **local**. None of `dash health`,
`dash dump crash`, the bridge log, or the rolling samples leaves
your machine unless you copy-paste them somewhere or enable the
opt-in remote endpoint (see [TELEMETRY_SPEC.md](TELEMETRY_SPEC.md)).

The remote endpoint, if enabled, only sees the 6-hourly
aggregate envelope — not the individual `dash health` calls, not
the rolling samples, not the crash dumps. The runbook above
relies on data that *never leaves the device* even with telemetry
fully on.
