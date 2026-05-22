# Stability + performance benchmarks (V1-E)

Snapshot of `tools/stress.py` run against `docs/mock_device.py`.
Bridge throttle / connection logic is exercised by the suite — what
this proves is the *transport* survives load. Numbers re-baselined
after the G-7 (firmware tokeniser) and G-8 (mock parity) fixes.

## Suite (5 tests)

| Test | What it stresses | Pass criterion |
|---|---|---|
| `flood` | 1000 snapshot pushes back-to-back, no host throttle | ≥ 95 % `OK:` replies, 0 `ERR:` |
| `oversize` | one 2000-byte line followed by `dash idle` | recovery `OK:` arrives (post-overflow drain) |
| `reconnect` | open → send → close → reopen → send | both halves get `OK:` |
| `prompt-latency` | 5 prompts, measure round-trip to `EVT: permission` | 5/5 decisions received |
| `idle-keepalive` | open, sleep 1 s, send | connection survives |

## Last run

```
[PASS] flood (1000 snapshots, no throttle)  (10.03s)  ok=1000 err=0
        throughput_per_s: 99
[PASS] oversize line: one ERR + no spurious cmd  (2.54s)  replies=2 ok=1
[PASS] reconnect: two sessions to same mock  (2.56s)  s1=1 s2=1
[PASS] prompt round-trip latency (5 prompts)  (15.30s)  got 5/5 decisions
        median_ms: 3057   max_ms: 3082   min_ms: 3032
[PASS] idle then act: connection survives 1s idle  (2.02s)  replies=1

all 5 passed
```

`prompt-latency` median of ~3.05 s is dominated by the mock's
`--decision-delay-ms` (default 500 ms, set to 3000 ms here to model a
realistic human reaction). Wire round-trip is < 50 ms — measure with
`--decision-delay-ms 0` to see the floor.

## Gap surfaced

The first run failed `flood` (0/1000 ok) and `prompt-latency` (0/5).
Root cause: `docs/mock_device.py` had its own copy of the firmware
tokeniser, written *before* the G-7 fix landed in
`components/aurora-harness/src/console_protocol.c`. The mock's stale
parser rejected the bridge's correctly-shaped JSON.

This is logged as **G-8 — consumer mocks re-implement the framework
tokeniser and drift silently** in [`../HARNESS_GAPS.md`](../HARNESS_GAPS.md).
Long-term fix: ship a Python reference implementation inside
esp-harness so every consumer mock imports the canonical tokeniser.

## Re-running

```bash
# Terminal 1 — start mock
python docs/mock_device.py --port 9876 --decision-delay-ms 200

# Terminal 2 — run suite
python tools/stress.py --all --port 127.0.0.1:9876

# Or one test at a time
python tools/stress.py --test flood --port 127.0.0.1:9876
```

Suite exits non-zero on the first failure. CI can wire this directly.
