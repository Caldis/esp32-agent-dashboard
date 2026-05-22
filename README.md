# esp32-agent-dashboard

**A live dashboard for AI-agent sessions on a 466×466 AMOLED panel.**
Watch what Claude Code or Codex is doing on your desk, approve / deny
permission prompts from a physical button, track token spend in
real time.

Built on the [esp-harness](https://github.com/Caldis/esp-harness)
framework — same console protocol, same LVGL scene framework, same
host CLI. The dashboard is a consumer that exercises esp-harness on
a real application, finds gaps, and feeds the fixes back upstream.

## Hardware

- **Waveshare ESP32-S3-Touch-AMOLED-2.16** (Aurora target board)
- USB-C to host. BLE is a future-version follow-up; v0 uses
  USB-Serial as transport.

## What it shows

| Scene | When | What |
|---|---|---|
| `idle` | no active sessions | gentle pulse, "zZz" |
| `sessions` | one or more active | total / running / waiting counters + last 4-5 transcript lines |
| `prompt` | session blocked on permission | full-screen tool name + hint; **BOOT** approves, **USER** denies, 60 s timeout |
| `tokens` | on demand | cumulative + today's tokens with a sparkline |
| `status` | on demand | battery / heap / uptime |

## How it talks to the host

The device is a `console_protocol` consumer (aurora-harness). The
host bridge pushes one-line JSON over USB-Serial:

```
dash snapshot '{"total":2,"running":1,"waiting":0,"msg":"approve: Bash","entries":[...],"tokens":184502,"tokens_today":31200,"prompt":{...}}'
dash prompt   '{"id":"req_abc","tool":"Bash","hint":"rm -rf /tmp/foo"}'
dash event    '{"role":"assistant","content":[...]}'
dash tokens   '{"cumulative":N,"today":N,"latest_sample":N}'
dash idle
```

The device replies `OK: {...}` / `ERR: ...` and, when the user
presses a physical button on the prompt scene, emits:

```
EVT: permission id=req_abc decision=once
EVT: permission id=req_abc decision=deny
```

The host bridge funnels that decision back into Claude Code / Codex
via their standard hook channels.

## Host bridge

`tools/claude_buddy_bridge.py` is a long-running daemon that:

1. Receives hook events from Claude Code (via `~/.claude/settings.json`)
   and Codex CLI.
2. Maintains a `SessionRegistry` of all running agents and their state.
3. Throttle-pushes a `dash snapshot` to the device on every state
   change (max one per 250 ms) plus a 10 s heartbeat.
4. On a `PreToolUse` that needs explicit approval, pushes
   `dash prompt` and waits for the device's `EVT: permission`
   decision before letting the hook return.

See `tools/README.md` for setup.

## Build + flash

The project scaffold is `--component-source link` — the Waveshare
BSP and the aurora-harness component are auto-wired via
`EXTRA_COMPONENT_DIRS`. From the monorepo root:

```powershell
cd D:\Code\esp-harness
.\tools\esp-harness\.venv\Scripts\python.exe -m esp_harness build --project ..\esp32-agent-dashboard --json
.\tools\esp-harness\.venv\Scripts\python.exe -m esp_harness flash --project ..\esp32-agent-dashboard --port COM9 --json
```

## Inspiration

Anthropic's [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
reference (Nordic UART Service over BLE, JSON-line protocol) was the
shape we copied. We diverged on transport (USB-Serial via
esp-harness's existing console protocol; BLE is a v1 follow-up) and
extended the schema to handle multiple concurrent agents (Claude
Code + Codex) instead of a single Claude Desktop session.

## Status

`v0.1` — scaffolded against esp-harness v1.7.5. Three subagents are
filling in firmware scenes (`main/`), host bridge (`tools/`), and
upstream esp-harness gap fixes (`D:\Code\esp-harness\`) in parallel.
Real end-to-end demo pending.

## Status when this is done

This file's "Status" section gets replaced with the demo log + any
upstream esp-harness changes the project triggered. The point of this
project isn't just to have a dashboard — it's to **use a real
consumer to harden esp-harness**. Track that in
[`HARNESS_GAPS.md`](./HARNESS_GAPS.md).
