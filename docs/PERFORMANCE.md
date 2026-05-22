# Performance baseline — v0.6 going into v0.7.0

This is PERF1's pre-cycle snapshot of where every layer of the
dashboard stack sits, what dominates at each layer, and the
shortest path to the v0.7.0 north star:

- **8 concurrent agents on one device** (today: `AGENT_SLOT_MAX = 4`)
- **Bridge sustains 10 000 events / minute** (today: ~335 ev/s mean
  → headroom for ~20 000 ev/min on the host side; the gating layer
  is the wire, not the host)

Every number in this file is reproducible. The exact bench commands
that produced each row are listed inline. Tools live under
[`tools/perf/`](../tools/perf/) — see those for usage.

## 1. Baseline numbers (real runs, 2026-05-22)

### 1.1 Host bridge — synthetic event throughput

Tool: `tools/perf/bench_bridge.py run --label baseline-v0.6`
(invokes `claude_buddy_bridge.py bench` under the hood, dry-run,
TCP mock target at `127.0.0.1:9876`).

| Cell | Events | pace_ms | throttle_ms | events/s | p50 event µs | p95 event µs | snapshot pushes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 000  | 0 | 250 | 318.0 |  5.0 | ~37 500 | 13  |
| 2 | 5 000  | 0 | 250 | 341.5 |  5.1 | ~37 500 | 59  |
| 3 | 10 000 | 0 | 250 | 346.5 |  4.7 | ~37 500 | 116 |

Result file: `tools/perf/results/bench-2026-05-22-174953-baseline-v0.6.json`.

**Observation 1.** The p50 per-event cost is **5 µs** — the bridge
spends almost no CPU on the *event* path. The p95 of ~37 500 µs is
entirely the publisher's throttled push window flushing under the
1.5 × `throttle_ms` drain at the end of the bench. Steady-state
sustained event handling is bounded by *the throttled snapshot
publisher loop and its serialisation cost*, not event ingestion.

**Observation 2.** Mean throughput improves from 318 → 346 ev/s as the
event count grows. This is warm-up amortisation (interpreter spin-up,
first-snapshot import paths). At >5 k events the curve is flat — the
ceiling is wire serialisation, not the Python event loop.

**Conclusion.** At 346 ev/s × 60 = ~20 700 events/min, the host bridge
**already exceeds** the 10k/min v0.7.0 target by 2 ×. The host is not
the bottleneck.

### 1.2 Stress flood — wire round-trip

Tool: `python tools/stress.py --test flood --port 127.0.0.1:9876`
(documented in [`STRESS_RESULTS.md`](STRESS_RESULTS.md)).

| Test | Snapshots | OK | ERR | Throughput |
|---|---:|---:|---:|---:|
| flood | 1 000 | 1 000 | 0 | **99 / s** |

The flood test sends 1 000 snapshots **back-to-back, waiting for `OK:`
before the next push** — it measures wire RTT under no host throttle.
99 snapshots/s ≈ 10 ms per round-trip is dominated by the mock's reply
serialisation + Python TCP loopback. On a real device over USB-Serial,
RTT was previously measured at ~186 ms (G-1 in `HARNESS_GAPS.md`).

### 1.3 Firmware health sample stability (mock, 30 s @ 1 Hz)

Tool: `python tools/perf/bench_firmware.py --duration-s 30 --include-samples --label mock-30s`

| Metric | Min | p50 | Mean | Max | StDev |
|---|---:|---:|---:|---:|---:|
| fps        | 33.4 | 33.4 | 33.4 | 33.4 | 0.00 |
| heap_free  | 84 200 | 84 200 | 84 200 | 84 200 | 0.00 |

Result file: `tools/perf/results/firmware-2026-05-22-174427-mock-30s.json`.

The mock returns fixture values — that's why every column is
identical. **The real device numbers are still N/A — needs device.**
Existing v1 firmware report cited ~84 KB heap free at idle and ~33 fps
during steady redraw of scene_dashboard (matches the mock fixture by
design). Once F2 brings a real device online (or the mock is enriched
to vary fps under sim-load), this row gets re-baselined.

### 1.4 Per-scene profile (mock, 6 scenes)

Tool: `python tools/perf/profile_scene.py --settle-s 0.5 --samples 3 --label mock-baseline`

| Scene | fps_mean | heap_free_mean | heap_delta_observed | force_supported |
|---|---:|---:|---:|---|
| idle      | 33.4 | 84 200 | 0 | yes |
| dashboard | 33.4 | 84 200 | 0 | yes |
| sessions  | 33.4 | 84 200 | 0 | yes |
| prompt    | 33.4 | 84 200 | 0 | yes |
| tokens    | 33.4 | 84 200 | 0 | **no** (encoder-only) |
| status    | 33.4 | 84 200 | 0 | **no** (encoder-only) |

Result file: `tools/perf/results/profile_scene-2026-05-22-174752-mock-baseline.json`.

The mock can't differentiate per-scene heap cost because it's a single
fixture. **Real-device per-scene heap deltas are N/A — needs device.**
The plumbing is in place: once F2 ships either a real device or a
mock that simulates per-scene LVGL allocations, this table comes
alive without any change to the bench script.

### 1.5 Firmware build size (esp-idf v6.0.1, ESP32-S3 noir profile)

From `D:/Code/esp32-agent-dashboard/build/`:

| Artifact | Size | Notes |
|---|---:|---|
| `esp32_agent_dashboard.bin`       |    652 832 B | factory partition (6 MB cap) |
| `bootloader.bin`                  |     21 088 B | |
| `esp32_agent_dashboard.elf`       | 10 279 540 B | with symbols, off-target |
| `esp32_agent_dashboard.map`       |  6 231 394 B | linker map |

From the map file (segment summary):

| Segment       | Address    | Size       | Purpose |
|---             |---         |---:        |---|
| `.iram0.text` | 0x40374404 |  70 891  B | hot loop instructions in IRAM |
| `.dram0.data` | 0x3fc95900 |  14 262  B | initialised data |
| `.dram0.bss`  | 0x3fc990b8 |   9 448  B | zero-init globals |
| `.flash.text` | 0x42000020 | 437 596  B | rest of app code |

Idle heap free **~84 KB** matches the v1 firmware report. The
ESP32-S3-Touch-LCD-1.85C target has ~330 KB internal SRAM after IDF
overhead; LVGL's double-buffer + image cache + scene state account
for the ~245 KB delta. PSRAM (8 MB octal) is mapped but not yet
hosting any allocations — see §3.3 for the path to using it.

## 2. Profile-by-profile: what dominates at each layer

### 2.1 Host bridge (Python)

What event-handling actually does, in cost order:

1. **JSON serialisation of the snapshot payload.** Every push goes
   through `json.dumps()` of the full slots[] tree. At 2 agents +
   5 entries each the line is ~700–900 bytes (close to
   `WIRE_MAX_BYTES = 900`). For 8 agents we'd be at ~2.5× the line
   length, which already exceeds `CONSOLE_MAX_LINE = 1024` on the
   device — see §3.2.
2. **TCP send + readline drain of the `OK:` reply.** Wire RTT to the
   mock is ~10 ms; that limits the *push rate*, not the *event ingest
   rate*. The publisher batches: `throttle_ms` (default 250) clamps
   push rate to 4/s independent of incoming event rate.
3. **Snapshot tree assembly.** `SessionRegistry` builds a fresh dict
   tree on every push. This is `O(slots × entries)` but trivial at
   today's slot count.

The host bridge has **2 × headroom for v0.7.0**: at the current
346 ev/s steady-state, 10k ev/min (167 ev/s) costs roughly half the
budget. The next 2 × on the host comes from:

- **msgpack on the wire** would cut bytes ~30 % vs JSON, letting us
  fit 8 agents under `CONSOLE_MAX_LINE` without truncating entries.
  Trade-off: harder to debug by eyeball; the bridge already speaks
  JSON to every host-side consumer (Claude Code, Codex, replay) so
  msgpack is purely a *wire* concern, transparent to upstream.
- **Cooperative throttling per-slot** — today every push pushes
  *all* slots. If slot 0 churns while slots 1-3 are idle, we still
  re-serialise their full state every 250 ms. A dirty-flag bitfield
  in `SnapshotPublisher` would let us emit per-slot deltas, ~30-40 %
  less bytes on the wire for the realistic case of one busy + N idle.

### 2.2 Wire / console (esp-harness console protocol)

- Single line, terminated by `\n`. Max 1024 bytes per line on the
  device side (`CONSOLE_MAX_LINE`). The bridge already self-limits
  to 900 bytes (`WIRE_MAX_BYTES`).
- One in-flight command at a time per console session. `dash health`
  uses the *payload-follows* envelope (`HEALTH_BEGIN`/`HEALTH_END`)
  to ferry replies larger than one line. The bench scripts in this
  directory share a *single* TCP socket for both pushes and health
  sampling for this reason — see `ScenePerfClient` in
  `profile_scene.py`.
- USB-Serial RTT ~186 ms on a real device, dominated by `dash <verb>`
  start-up cost. Bench scripts run against the TCP mock so this
  cost is excluded from host-side numbers; for end-to-end latency
  the device is the gating factor.

### 2.3 Firmware (scene timers + LVGL)

- Scene timers tick at **250 ms** (`scene_dashboard.c:330`). Each
  tick: lock `agent_state` → memcpy `slots[AGENT_SLOT_MAX]` into a
  stack snapshot (~4 KB at AGENT_SLOT_MAX=4, ~8 KB at AGENT_SLOT_MAX=8)
  → unlock → mutate LVGL widgets. The memcpy is the hot path.
- `lv_label_set_text()` is cheap when the text doesn't change, but
  scenes call it every tick unconditionally — LVGL string-compares
  internally, so this is fine today but at 8 cards × 5 labels each =
  40 set_text calls per 250 ms tick, the compare cost adds up.
- LVGL line-rendering for the sparkline uses
  `lv_point_precise_t spark_pts[AGENT_SPARK_SAMPLES * AGENT_SLOT_MAX]`
  — 32 × 4 = 128 points today. At AGENT_SLOT_MAX=8 the buffer
  becomes 256 points (2 KB stack pressure on the scene state). Move
  to one shared 32-point buffer (since we only render *one* primary
  agent's sparkline) — that fix is free.
- `scene_dashboard.c` allocates its `dash_state_t` once via
  `lv_malloc_zeroed()` at scene init. No per-tick malloc. The
  *agent_state* itself is all bounded `char[]` and POD numerics —
  zero malloc churn.

### 2.4 Memory layout (current, AGENT_SLOT_MAX=4)

Calculated from `main/agent_state.h`:

- `agent_entry_t` = role[16] + text[80] + tool[16] + ts[8] +
  monotonic_ms(4) = **124 B** (probably padded to 128 by compiler).
- `agent_slot_t`  = in_use(1) + kind(16) + session_id(32) + cwd(64)
  + msg(128) + status(4) + entries[5]×124(=620) + entry_count(4)
  + entry_seq(4) + tokens_cumulative(8) + tokens_today(8)
  + spark[32]×4(=128) + spark_count(4) + spark_head(4)
  + last_active_unix(4) + last_seen_monotonic_ms(4)
  ≈ **1 035 B** plus padding ⇒ call it **1 040 B / slot**.
- `agent_state_t` = 4 × slot + ~250 B aggregate fields
  ≈ **4 410 B** today.
- At AGENT_SLOT_MAX=8 → **8 × 1 040 + 250 ≈ 8 570 B**, i.e.
  +4 160 B vs today. Comfortably within the 84 KB idle heap budget,
  but see §3.3 — there's a non-obvious second hit.

## 3. Where the next 2 × lives

### 3.1 msgpack wire encoding (the biggest single win)

JSON for a 2-agent, 5-entry-per-agent snapshot lands at ~750 B.
msgpack of the same tree is ~520 B (~30 % smaller; numbers vary by
content). For 8 agents the difference is the difference between
"fits one console line" (msgpack at ~1 050 B exceeds `CONSOLE_MAX_LINE
= 1024` by ~30 B, but a `CONSOLE_MAX_LINE` bump to 2048 — already
discussed in HARNESS_GAPS as easy) and "must split across multiple
pushes". JSON at 8 agents would be ~1 850 B — that requires
either chunking or msgpack.

Cost: bridge gets a `msgpack-python` dependency (minimal, already
a transitive of many libs); device gains a 2 KB msgpack decoder
(several public implementations exist). The console verb gets a
`fmt` argument: `dash snapshot --fmt=msgpack <hex>` — keeps the
text-tokeniser happy.

### 3.2 `CONSOLE_MAX_LINE` 1024 → 2048

Today snapshots are silently truncated past 900 B. For 8 agents this
is a hard wall. Doubling the line limit costs `2 × 1024 B` of stack
per console session — affordable. Push the change upstream into
esp-harness (it's the framework's constant). Log as a HARNESS_GAP
if not already in there.

### 3.3 Per-agent transcript window scales down

Today every slot has `AGENT_ENTRY_COUNT = 5` entries × 124 B = 620 B
per slot just for the rolling transcript. Of that, only `scene_sessions`
shows more than 2 entries at any time. At 8 agents we shouldn't
allocate 5 entries per slot — `scene_dashboard` will display only
1-2 lines per agent in the 4×2 thumb grid (see SCALING.md §1).

Two options:

- **Compile-time:** drop `AGENT_ENTRY_COUNT` to 3. Saves
  8 × 2 × 124 = 1 984 B at the 8-agent slot count, at the cost of
  scene_sessions showing only 3 lines instead of 5.
- **Runtime ring with adaptive depth:** keep the buffer at 5 but
  let scenes ask for "give me the latest K". Same memory cost as
  today, but scene_dashboard's painters bail at K=1 or K=2 instead
  of touching the rest. (No memory win, just clarity.)

PERF1's recommendation: option 1, plus the painters key off K
explicitly. The "rolling transcript window per agent" comment in
`agent_state.h:38` is the only doc that needs updating.

### 3.4 Sparkline buffer collapse

`dash_state_t.spark_pts` is sized `AGENT_SPARK_SAMPLES * AGENT_SLOT_MAX`
= 128 today, 256 at AGENT_SLOT_MAX=8 (= 2 KB on the scene state struct).
But `render_sparkline()` only renders **one** agent's sparkline — the
"primary" with the most samples. Collapse the buffer to
`AGENT_SPARK_SAMPLES` (32, 256 B). Free 1.75 KB at the 8-agent boundary.

### 3.5 LVGL label thrash → dirty bit

Each scene tick currently overwrites every label's text regardless of
whether the underlying field changed. `lv_label_set_text` does a
string-compare so this is correct, just wasteful. At 8 cards × 5
labels = 40 strcmps per 250 ms = 160 strcmps/s. Negligible today;
becomes ~640 strcmps/s at 8 agents. A dirty bit per (slot, field)
keyed on `agent_slot.entry_seq` + a `msg_seq` would cut this to 0
on the common idle case.

### 3.6 Ring-buffer instead of malloc on entries[]

`agent_state_push_entry()` currently *shifts* entries on every push:

```c
for (int i = keep; i > 0; --i) {
    slot->entries[i] = slot->entries[i - 1];
}
```

That's a `memcpy(124 B)` × 5 per push = 620 B copied per push per
slot. At 8 slots × 4 pushes/s = ~20 KB/s memory bandwidth chasing
its tail. Switch `entries[]` to a head-indexed ring (mirroring
`spark[]`/`spark_head`/`spark_count`) and the cost drops to one
124 B store per push. ~95 % win on this micro-path.

## 4. The 8-agent goal — what's needed

In rough size order. See [`SCALING.md`](SCALING.md) for the design.

| Change | Owner | Estimated effort |
|---|---|---|
| `AGENT_SLOT_MAX 4 → 8` in `main/agent_state.h` | F2 | trivial |
| `AGENT_ENTRY_COUNT 5 → 3` (see §3.3) | F2 | trivial |
| Collapse `dash_state_t.spark_pts` to AGENT_SPARK_SAMPLES (see §3.4) | F2 | trivial |
| Adaptive grid in `scene_dashboard.c` (1/2/4/8 layouts) | F2 | medium — new layouter |
| Per-agent transcript depth scaling (5→2 lines at 8 agents) | F2 | small |
| `theme.h` accent allocator (golden-angle for kinds 4-8) | F2 | small |
| Entries ring buffer (see §3.6) | F2 | small |
| `dash scene <id>` verb (so `profile_scene.py` works) | F2 | small — adds a verb |
| Bridge: msgpack wire encoding (opt-in) | H2/H3 | medium |
| `CONSOLE_MAX_LINE 1024 → 2048` upstream | G2 | small — esp-harness PR |
| Mock device: vary fps + heap_free with load | (PERF1 once F2 wires the verbs) | small |

## 5. Implementation note (for F2)

**PERF1 does NOT modify any source file** outside `tools/perf/` and
the two docs you're reading. Everything in §3 and §4 is a proposal
for F2 (firmware engineer) and H2/H3 (bridge engineers). The bench
infrastructure under `tools/perf/` is designed so that when F2 makes
any of these changes, `bench_bridge.py compare` immediately
quantifies the delta against the saved baseline
(`bench-2026-05-22-174953-baseline-v0.6.json`). No source file in
`main/` was touched by PERF1.

The single most load-bearing F2 change is in `main/agent_state.h`:
bump `AGENT_SLOT_MAX 4 → 8` and `AGENT_ENTRY_COUNT 5 → 3`. Both are
one-line constants. Every downstream scene already iterates with
`for (int i = 0; i < AGENT_SLOT_MAX; ++i)`, so the slot bump
propagates by recompilation. The entry-count change requires
`scene_sessions.c` to verify it still fits 3 visible lines without
clipping — quick visual check, no code changes if the layout was
done with `lv_obj_set_flex_flow` (it was).

## 6. Re-running this baseline

```bash
# Host bridge sweep — ~30s. Saves to tools/perf/results/.
python tools/perf/bench_bridge.py run --label vX.Y

# Firmware health sampler — needs mock or device on 127.0.0.1:9876.
python tools/mock_device_v1.py --port 9876 &
python tools/perf/bench_firmware.py --duration-s 60

# Per-scene sweep
python tools/perf/profile_scene.py --settle-s 1 --samples 5

# Compare two saved bench runs
python tools/perf/bench_bridge.py compare
```

Every run writes a JSON file. Commit them under
`tools/perf/results/` (already created by the scripts; see the
existing `bench-2026-05-22-174953-baseline-v0.6.json`).

## 7. HARNESS_GAPS surfaced by this work

Two gaps for `D:\Code\esp32-agent-dashboard\HARNESS_GAPS.md` that
should land alongside v0.7.0:

- **G-PERF1 — no `dash scene <id>` verb.** `profile_scene.py` cannot
  deterministically switch the firmware into `tokens` or `status`
  scenes; both are entered only via the rotary encoder. Adding a
  `dash scene <id>` console verb (one line in `agent_commands.c` +
  one entry in the mock) would close this. Workaround in this cycle:
  `profile_scene.py` flags `scene_force_unsupported: true` for those
  rows.
- **G-PERF2 — mock device returns constant `dash health` fixtures.**
  Mock fps/heap_free never change, so `bench_firmware.py` and
  `profile_scene.py` can't see per-scene heap deltas without a real
  device. Recommend the mock either accept a `--health-vary` flag
  that emulates per-scene allocations, or import a Python reference
  implementation of LVGL allocations from esp-harness (closing G-8
  and G-PERF2 in one stroke).
