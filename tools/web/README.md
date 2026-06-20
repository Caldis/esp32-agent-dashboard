# Web Dev Tools — off-device development & data-flow debugging

The web panel is the ESP32 dashboard's **developer tool**, not a second UI. It
mirrors the device's state one-way (agent activity → screen) so you can develop
and debug the data flow **without flashing hardware**:

- **Same data, same bugs** — the firmware's C data layer is compiled to WASM, so
  the browser parses `dash` commands and computes `state_json` exactly like the
  device.
- **Real device + web in parallel** — the bridge drives the physical ESP32 and
  also mirrors every line to the panel (fan-out), so both update together.
- **Pure display** — the device is a one-way status mirror; it never blocks or
  waits on the agent. Synchronous interactions (approve/deny, quick-reply) are
  display-only on the device. The web panel keeps dev-only drive tools.

Open **http://127.0.0.1:8090/** after starting the stack.

## Quick start

```bash
# Real ESP32 on COM9 + web mirror (the usual dev setup)
python tools/web/serve.py --spawn-bridge --serial COM9 --auto off

# Pure mock (no hardware) — a TCP stand-in device + web
python tools/web/serve.py --spawn-bridge

# Use a bridge you started yourself (serve doesn't spawn one)
python tools/web/serve.py
python tools/claude_buddy_bridge.py serve --port-kind serial --port COM9 \
    --mirror 127.0.0.1:9876 --listen 127.0.0.1:7321
```

Shortcut launchers (same as the first command): `tools/web/launch.ps1`
(PowerShell) / `tools/web/launch.sh` (bash); both take an optional COM port,
e.g. `./launch.ps1 COM9`.

## Run modes

| Axis | Options | Notes |
|------|---------|-------|
| device | `--serial COM9` (real) / omit (mock TCP :9876) | real = bridge owns COM, mirrors to the panel |
| permissions | observe (default on real) / gate (`--gate-permissions`, default on mock) | observe never blocks the agent; gate is the approve/deny demo |

## Environment knobs

| Var | Default | Effect |
|-----|---------|--------|
| `CLAUDE_BUDDY_IDLE_TURN_S` | 60 | a silent running agent flips to "your turn" after this (ESC/stall fallback — CC fires no hook on interrupt) |
| `CLAUDE_BUDDY_DEBUG=1` | off | bridge logs every received event + every line pushed (with OVERSIZE flag) |
| `hook_dispatch_debug.on` | off | `touch $TEMP/hook_dispatch_debug.on` → hook_dispatch logs each call's outcome to `$TEMP/hook_dispatch_debug.log` |

## Build & flash firmware (Windows, ESP-IDF via EIM)

Must run from **PowerShell** (idf.py refuses Git Bash). The globally-installed
`esp-harness.exe` may be broken (`No module named esp_harness.cli`); use the
source checkout instead:

```powershell
$env:PYTHONPATH = "D:\Code\esp-harness\tools\esp-harness\src"
python -m esp_harness build
# flashing needs COM9 — stop the serve.py stack first to free the port
python -m esp_harness flash
python -m esp_harness screenshot --size 220 --out shot.png
```

ESP-IDF is at `C:\esp\v6.0.1`, activated via the EIM PowerShell profile
`C:\Espressif\tools\Microsoft.v*.PowerShell_profile.ps1` (esp_harness finds it
automatically).

## Pipeline

```
CC / Codex hook → hook_dispatch.py → bridge(:7321) → device (COM9 | mock TCP :9876)
                                              └ --mirror → serve.py tap(:9876) → SSE → browser → WASM
```

## Panels

- **状态总览 / Agents** — totals + per-agent cards from `state_json`.
- **设备状态机** — inferred device state (zzz / thinking / your turn / pick /
  type / clarify / …), the signal/reason that drove each transition, the full
  state graph (current node highlighted, outgoing edges + conditions), and the
  transition history. First place to look when "the device shows the wrong thing".
- **注入事件 / 屏幕测试驱动** — dev-only: inject hook events (no real agent
  needed) and push raw `dash` signals to the device to exercise UI combos.
- **Hooks 管理** — install/enable/disable hooks in the real `~/.claude` /
  `~/.codex` config (backed up first).
- **原始 dash 帧 / 设备信号** — raw wire log + device EVT/scene signals.

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| Device frozen, screen+buttons dead | Hung firmware (rare, old build) — physical RESET/EN or USB replug. |
| Device briefly shows zzz after restarting the stack | Normal: the bridge registry is in-memory, cleared on restart; the next hook event refills it. |
| "Device state looks wrong" | Read the **设备状态机** panel — current state's trigger reason + history pinpoint who/when/why moved it. |
| Hooks not syncing | Confirm bridge `:7321` is LISTENING; check the circuit breaker in `$TEMP/claude_buddy_cb.json` (`open_until`); enable the debug logs above. |
| `dash snapshot: line too long` on device | A snapshot exceeded `CONSOLE_MAX_LINE` (1023B). `snapshot_v1` caps wire size; if you see this, an oversized field slipped through. |

## Hooks management (CLI)

```bash
python -m tools.hooks_admin status                          # show install/enable state
python -m tools.hooks_admin install --agent claude-code     # install our hooks
python -m tools.hooks_admin disable --agent claude-code     # soft-disable (keeps config)
```

Events installed: SessionStart, UserPromptSubmit, PreToolUse, PostToolUse, Stop,
SessionEnd. CC hot-reloads them (no restart needed).
