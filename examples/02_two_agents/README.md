# 02_two_agents — concurrent agents + the slot model

Pushes ~8 snapshots over 4 s with two agents present (`claude-code` /
`cc_main` and `codex` / `cx_aux`). Every other snapshot one of the two
flips between `running` and `waiting`, so on a real device the sessions
scene visibly shimmers between the two rows.

## What this teaches

**`(kind, session_id)` is the device's slot key.** The device does NOT
care what order `agents[]` arrives in — it groups by the tuple. This
example deliberately alternates the array order between ticks (claude
first / codex first) to prove the point.

**`status` is per-tick, not sticky.** The device redraws the status pip
from each snapshot, so if you stop sending a snapshot, the displayed
status freezes — that's why the bridge issues a 10 s keepalive.

**`tokens` and `tokens_today` accumulate on the device.** This example
shows the raw cumulative number you'd send; the device's tokens scene
takes deltas to build the sparkline.

## Run it

```bash
# Terminal A
python tools/mock_device_v1.py --port 9876 -v

# Terminal B
python examples/02_two_agents/run.py --duration 6
```

Expected output ends with `OK — pushed N snapshots across 2 agents.`
and exit code 0.

## CLI options

| Flag | Default | What it does |
|---|---|---|
| `--host` | `127.0.0.1` | mock device host |
| `--port` | `9876` | mock device port |
| `--duration` | `4.0` | seconds to run for |
| `--interval` | `0.5` | seconds between snapshots |

## Try this next

- Bump `--interval 0.1` and watch the bridge's throttle kick in (real
  bridge caps at one push per 250 ms — see `PROTOCOL.md`).
- Add a third agent with `"kind": "other"` to see the fallback palette
  slot.
