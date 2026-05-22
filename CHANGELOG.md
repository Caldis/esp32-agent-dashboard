# Changelog

Project-level milestones. Per-component notes live in:

- [`main/`](./main/) — firmware scene history (in source headers)
- [`tools/`](./tools/) — host bridge history (in `claude_buddy_bridge.py` docstring)
- [`PROTOCOL.md`](./PROTOCOL.md) — wire-format version history

## [Unreleased]

Anything landed on `master` after `v0.1.0` and before the next tag
lives here.

## [0.1.0] — 2026-05-23

Initial public release. Multi-agent USB-Serial dashboard for Claude
Code and Codex CLI, running on the Waveshare ESP32-S3-Touch-AMOLED-2.16.

### Added

#### Firmware (`main/`)

- Five LVGL scenes registered through esp-harness's scene framework:
  - `scene_idle` — pulse + "zZz" + dim ring while no agent is active
  - `scene_sessions` — per-agent rows with status pip, cwd, last 3-5
    transcript entries, totals header
  - `scene_prompt` — full-screen permission prompt with tool name,
    command preview, **BOOT** = approve / **USER** = deny / 60 s timeout
  - `scene_tokens` — cumulative + today's tokens, 24 h sparkline
  - `scene_status` — battery / heap / uptime / build info
- `dash` verb registered on the device console:
  - `dash snapshot <json>` — periodic state push (throttled to ≤ 1 per
    250 ms by the host bridge)
  - `dash prompt <json>` — open the prompt scene + wait for a button
  - `dash event <json>` — append a transcript line / change scene
  - `dash tokens <json>` — update cumulative + today counters
  - `dash idle` — return to the idle scene
- Button events emitted as `EVT: permission id=<req_id> decision=<once|deny>`.

#### Host bridge (`tools/`)

- `claude_buddy_bridge.py` — long-running daemon. Listens on
  TCP 127.0.0.1:7321 for hook events; maintains a `SessionRegistry`
  keyed by `(agent_kind, session_id)`. Throttled snapshot publisher
  (250 ms minimum interval, 10 s keepalive). Permission round-trip via
  a persistent `esp_harness.core.console_session.ConsoleSession`.
- `hook_dispatch.py` — short-lived. Invoked by Claude Code via
  `~/.claude/settings.json` hooks. Forwards each event to the bridge,
  blocks on the bridge's reply when the event needs a permission
  decision (`PreToolUse`), exits cleanly otherwise.
- `codex_wrapper.py` — short-lived. Wraps a `codex exec ...`
  invocation, parses its JSONL stdout, forwards normalized events to
  the bridge. Necessary because Codex CLI has no native hook system
  (see [`HARNESS_GAPS.md`](./HARNESS_GAPS.md) G-5).
- `sample_session.jsonl` — canned event stream used by CI and offline
  iteration.

#### Tooling

- `docs/mock_device.py` — TCP stand-in for the firmware. Mirrors
  `console_protocol.c`'s tokeniser exactly so anything that talks to
  the mock will talk to the real device. Lets the bridge be tested
  without COM9 access.
- `.github/workflows/ci.yml` — replays `tools/sample_session.jsonl`
  through the bridge on every push and PR, asserts the expected
  `dash` verbs were emitted.
- Issue templates (bug / feature / question) and PR template under
  `.github/`. Mirror the esp-harness shape, scoped to this project's
  concerns (wire protocol, scenes, hooks).
- `docs/index.html` — homepage at
  `https://caldis.github.io/esp32-agent-dashboard/`. Hero, terminal
  demo, scene gallery, comparison table, get-started CTA.

#### Wire protocol (`PROTOCOL.md`)

- `v1` — per-agent sessions, richer scene data, device-side config.
- `v0` (pre-release internal) — single-agent USB-Serial. Not shipped
  to the public repo.

### Fixed

- esp-harness commit `664b14e` — tokeniser preserves inner double
  quotes on quote-leading tokens, so nested JSON survives the device's
  command parser. Surfaced against this project (G-7).
- esp-harness commit `98affb0` — console_protocol drains the overflow
  tail after a too-long line, so oversize JSON pushes get exactly one
  `ERR:` instead of one ERR plus a spurious `unknown command` for the
  truncated tail. Surfaced against this project (the "agent-dashboard
  regression" smoke case).

### Known limitations

- USB-Serial only. BLE NUS is on the v1.0 roadmap.
- `docs/demo_inputs.jsonl` uses Claude-Code-hook PascalCase event
  names (`SessionStart`, `UserPromptSubmit`, …); the bridge expects
  snake_case `type:` keys. The CI uses `tools/sample_session.jsonl`
  which is in the bridge's expected shape. Cross-format conversion
  is on the to-do list — track in
  [`HARNESS_GAPS.md`](./HARNESS_GAPS.md).
- Codex event-type strings are inferred from public behavior and may
  drift between Codex releases. Track in `codex_wrapper.py`.
- Firmware is not built in CI (cross-compiling ESP-IDF in a Linux
  runner is slow and image-fragile). Real-hardware verification
  happens at release time via `tools/smoke.ps1` (planned).

[Unreleased]: https://github.com/Caldis/esp32-agent-dashboard/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.1.0
