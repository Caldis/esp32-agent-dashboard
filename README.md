<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/brand/logo-dark.svg">
  <img src="docs/brand/logo.svg" alt="esp32-agent-dashboard" width="120" height="120" onerror="this.style.display='none'">
</picture>

# esp32-agent-dashboard

**A live dashboard for AI-agent sessions on a 466×466 AMOLED panel.**

Watch what Claude Code and Codex are doing on your desk in real time.
Approve or deny permission prompts with a physical button. Track token
spend session-by-session. Multi-agent by design.

[![CI](https://img.shields.io/github/actions/workflow/status/Caldis/esp32-agent-dashboard/ci.yml?branch=master&label=bridge+roundtrip)](https://github.com/Caldis/esp32-agent-dashboard/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/tag/Caldis/esp32-agent-dashboard?label=release&color=b8431a)](https://github.com/Caldis/esp32-agent-dashboard/releases)
[![license](https://img.shields.io/github/license/Caldis/esp32-agent-dashboard?color=344a36)](./LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0%2B-1c1814)](https://docs.espressif.com/projects/esp-idf/)
[![LVGL](https://img.shields.io/badge/LVGL-9.x-1c1814)](https://lvgl.io/)
[![esp-harness](https://img.shields.io/badge/esp--harness-v1.7.5-b8431a)](https://github.com/Caldis/esp-harness)
[![docs](https://img.shields.io/badge/docs-homepage-b8431a)](https://caldis.github.io/esp32-agent-dashboard/)

[**Quickstart**](#quickstart-30-seconds) ·
[**What this is**](#what-this-is) ·
[**Scenes**](#scenes) ·
[**Protocol**](./PROTOCOL.md) ·
[**Contributing**](./CONTRIBUTING.md) ·
[**Homepage**](https://caldis.github.io/esp32-agent-dashboard/)

<br>

<img src="docs/img/hero.png" alt="esp32-agent-dashboard hero" width="720" onerror="this.style.display='none'">

</div>

---

## What this is

A purpose-built physical dashboard for AI coding agents — Claude Code,
Codex CLI, and friends. It lives next to your keyboard and shows what
your agents are doing without you having to context-switch into the
terminal that owns them. A long-running host bridge funnels hook events
from each agent into a single state view; the device renders the
current state on a 466×466 AMOLED panel and lets you respond to
permission prompts with a physical button.

| Layer | What it gives you |
|---|---|
| 🔥 **Firmware** (`main/`) | 5 LVGL scenes (idle / sessions / prompt / tokens / status) on the Waveshare ESP32-S3-Touch-AMOLED-2.16. Built on the [esp-harness](https://github.com/Caldis/esp-harness) console protocol + scene framework. |
| 🐍 **Host bridge** (`tools/claude_buddy_bridge.py`) | Long-running daemon. Ingests Claude Code hooks (`hook_dispatch.py`) and Codex JSONL (`codex_wrapper.py`). Maintains a per-agent `SessionRegistry`. Pushes throttled `dash snapshot` to the device + blocks `PreToolUse` hooks on the device's permission button. |
| 🔌 **Wire format** ([`PROTOCOL.md`](./PROTOCOL.md)) | One-line JSON over USB-Serial: `dash snapshot`, `dash prompt`, `dash event`, `dash tokens`, `dash idle`. Device replies `OK:` / `ERR:` / `EVT:`. v1 ships with multi-agent + config + health. v2 will add BLE NUS. |

## Quickstart (30 seconds)

```bash
# 1. Get the esp-harness toolkit (provides the build / flash / console CLI)
git clone https://github.com/Caldis/esp-harness
pip install -e esp-harness/tools/esp-harness/

# 2. Get this repo
git clone https://github.com/Caldis/esp32-agent-dashboard
cd esp32-agent-dashboard

# 3. Build + flash the firmware (Waveshare ESP32-S3-Touch-AMOLED-2.16)
esp-harness build --project .
esp-harness flash --project . --port COM9   # adjust to your serial port

# 4. Wire the bridge into Claude Code
#    Add to ~/.claude/settings.json:
#    {
#      "hooks": {
#        "PreToolUse":  "python D:/Code/esp32-agent-dashboard/tools/hook_dispatch.py pre_tool_use",
#        "PostToolUse": "python D:/Code/esp32-agent-dashboard/tools/hook_dispatch.py post_tool_use",
#        "Stop":        "python D:/Code/esp32-agent-dashboard/tools/hook_dispatch.py stop"
#      }
#    }

# 5. Start the bridge
python tools/claude_buddy_bridge.py serve --serial-port COM9
# → "[bridge] serving on 127.0.0.1:7321 | dry_run=False | serial=COM9"
```

Now start a Claude Code session. The device wakes from `idle` →
shows the running session → if a `Bash` tool needs approval, the
prompt scene takes over and waits for **BOOT** (approve) or **USER**
(deny). 60 s timeout falls through to "deny" by default.

> **No board?** You can still iterate the bridge against the included
> TCP mock device:
> ```bash
> python docs/mock_device.py --port 9876 &
> python tools/claude_buddy_bridge.py replay tools/sample_session.jsonl --dry-run
> ```
> The mock mirrors the firmware's console-protocol tokeniser exactly,
> so anything that talks to the mock will talk to the real device.

For the full end-to-end runbook, see [`docs/E2E_DEMO.md`](./docs/E2E_DEMO.md).
For the integration map, see [`docs/HOST_INTEGRATION.md`](./docs/HOST_INTEGRATION.md).

## Scenes

The dashboard cycles through five scenes, all rendered with LVGL 9.x
on a 466×466 round AMOLED:

<div align="center">
<img src="docs/img/scenes-strip.png" alt="all five scenes side-by-side" width="780" onerror="this.style.display='none'">
</div>

| Scene | When it shows | What it shows |
|---|---|---|
| **idle** | no active sessions (60 s+ since last event) | gentle "zZz" pulse, dim ring |
| **sessions** | one or more agents running or waiting | per-agent rows: name, status pip, cwd, last 3-5 transcript lines, total / running / waiting counters |
| **prompt** | a `PreToolUse` event needs explicit approval | full-screen tool name, the exact command preview, **BOOT** = approve once, **USER** = deny, 60 s timeout countdown |
| **tokens** | on demand (`dash tokens`) | cumulative + today's tokens, 24 h sparkline, per-agent breakdown |
| **status** | on demand (`dash event {scene:'status'}`) | battery %, heap free, uptime, WiFi state (v2), firmware build |

Captured live:

| | | |
|:-:|:-:|:-:|
| [`docs/img/dash-idle.png`](./docs/img/dash-idle.png) | [`docs/img/dash-sessions.png`](./docs/img/dash-sessions.png) | [`docs/img/dash-prompt.png`](./docs/img/dash-prompt.png) |
| **idle** | **sessions** | **prompt** |
| [`docs/img/dash-tokens.png`](./docs/img/dash-tokens.png) | [`docs/img/dash-status.png`](./docs/img/dash-status.png) | |
| **tokens** | **status** | |

## How it compares

There's a small but growing space of "physical surfaces for AI agents".
Where this one fits:

| Project | Transport | Agents | Hardware | Permission button |
|---|---|---|---|---|
| [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) | BLE (Nordic UART) | Claude Desktop (single) | reference: nRF / generic BLE | No (display-only) |
| [claude-dashboard plugin](https://github.com/Caldis/claude-dashboard) | in-app status line | Claude Code | none (terminal-resident) | No (no hardware) |
| **esp32-agent-dashboard** *(this repo)* | USB-Serial (v0.1) → BLE NUS (v1.0) → WiFi (v2.0) | Claude Code + Codex (multi-agent) | Waveshare ESP32-S3-Touch-AMOLED-2.16 | **Yes** (BOOT = approve, USER = deny) |

We took the protocol *shape* (JSON-over-line, one verb per state class)
from claude-desktop-buddy and extended it to handle several concurrent
agents at once. The transport choice is different (USB-Serial first
because that's what esp-harness ships with; BLE is on the roadmap,
not a precondition). And the physical button is the point — we wanted
the loop where you glance at the panel and tap to unblock the agent,
not just a passive display.

## Status / roadmap

| Milestone | Status | What ships |
|---|---|---|
| **v0.1** *(this release)* | ✅ shipped | USB-Serial multi-agent: 5 scenes, hook bridge for Claude Code + Codex, permission button, mock device for CI |
| **v1.0** | 🟡 in design | BLE NUS transport for Claude Desktop pairing (no USB tether). Same wire format over BLE characteristics. |
| **v2.0** | 🔵 sketched | WiFi push for headless dev boxes — dashboard sits on your desk while the agent runs on a server. mDNS discovery, TLS to localhost-on-laptop, fallback to USB. |

We track the consuming-side gaps surfaced against the framework in
[`HARNESS_GAPS.md`](./HARNESS_GAPS.md) — each one feeds back into
esp-harness or becomes a documented recipe. As of v0.1, 2 gaps have
landed upstream and 5 are in design.

## Predecessors and inspirations

| Project | What we took from it |
|---|---|
| [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) | Reference shape of the wire protocol (JSON-over-line, NUS framing). The idea that a desktop AI agent deserves a physical surface. |
| [Caldis/esp-harness](https://github.com/Caldis/esp-harness) | Everything below the application layer — console protocol, LVGL scene framework, host CLI, simulator, build & flash. This project is the first non-Aurora consumer of esp-harness and exists in part to harden the framework. |
| [Caldis/claude-dashboard](https://github.com/Caldis/claude-dashboard) | The information-density question: how much state can you usefully show at one glance? Answers from the terminal-status-line variant of that question carry over to the 466×466 surface. |

## License

MIT — see [`LICENSE`](./LICENSE).

---

<div align="center">

Made with intent, in pursuit of <em>seeing the loop</em>.

</div>
