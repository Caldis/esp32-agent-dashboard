# Changelog

Project-level milestones. Per-component notes live in:

- [`main/`](./main/) — firmware scene history (in source headers)
- [`tools/`](./tools/) — host bridge history (in `claude_buddy_bridge.py` docstring)
- [`PROTOCOL.md`](./PROTOCOL.md) — wire-format version history

## [Unreleased]

## [0.2.0] — 2026-05-23

**Framework hardening — close G-1, G-3, G-4, G-H1, G-H3, G-F1b**

esp-harness ships the public persistent-session API (G-1, G-3), PayloadFollowsReader (G-H1), ERR-line surfacing (G-H3), explicit payload tags (G-4), and ?dump w-honour (G-F1b). Bridge adopts the new APIs; net -122 LOC. 27 new framework tests (69 total).


Anything landed on master after `v0.1.2` and before the next tag.
Current contents (will fold into the next version stanza when the
overnight wave integrates):

### Added (docs)
- `docs/UX_REVIEW.md` — U1's 1953-word firmware UX critique with
  top-10 ranked improvements.
- `docs/releases/{v0.1.0,v0.1.1,README}.md` — per-version release
  notes pages.
- `docs/blog/2026-05-23-multi-agent-from-scratch.md` — 2029-word
  story of the multi-agent overnight build.
- `docs/blog/README.md` + index.html nav link.
- `docs/releases/v1.0.0-checklist.md` — stable-API gate-list.
- `docs/AGENT_KINDS.md` — v0.5.0 agent-kind registry spec.

### Fixed
- `main/theme.c` — palettes realigned to `docs/brand/palette.md`
  (U1 P0); 8-token drift closed, rust/teal restored to brand
  source-of-truth values.

## [0.1.2] — 2026-05-23

**Orchestrator rails + post-v0.1.1 polish**

tools/release.ps1 + docs/RELEASE_PROCESS.md + ROADMAP.md + docs/AGENT_ROLES.md ship the infrastructure for the overnight 20-version push. Also lands H1 reflection patches (tiny_json polish, hook_dispatch fidelity, TransportError wrap) and v1 acceptance shots.


### Added
- `tools/release.ps1` — one-command per-version release script enforcing
  the 10 quality gates from `ROADMAP.md` (build, pytest, stress, bench,
  CHANGELOG bump, commit, tag, push). Refuses to tag on the first red gate.
- `docs/RELEASE_PROCESS.md` — orchestrator runbook covering waves,
  conflict policy, per-version release-notes template.
- `ROADMAP.md` — 20-version overnight delivery plan (v0.2.0 → v2.2.0).
- `docs/AGENT_ROLES.md` — orchestrator/agent contract registry.

### Fixed
- `main/tiny_json.c` — tighter tolerant key-matcher (post G-F1a polish).
- `tools/hook_dispatch.py` — forward bridge permission reply verbatim
  instead of dropping unrecognised fields.
- `tools/claude_buddy_bridge.py` — wrap TCP connect failure in
  `TransportError` so the retry loop treats it as recoverable.

### Documented
- HARNESS_GAPS additions G-H1 (PayloadFollowsReader), G-H2 (resolved by
  esp-harness@85770d8 — whitespace-in-JSON parity test), G-H3 (ERR-line
  logging), G-F1a (resolved — tiny_json depth-tracking), G-F1b (open —
  `?dump` 128×128 hard cap).

## [0.1.1] — 2026-05-23

**v1 firmware + bridge integration.** The first follow-up after v0.1.0
brings the multi-agent v1 protocol implementation to both the firmware
and the host bridge, plus the brand assets the public v0.1.0 README
already referenced but the commit hadn't included.

### Added
- **Firmware**: multi-agent state model (`AGENT_SLOT_MAX=4`, stable
  left/right placement by `(kind, session_id)`); three named themes
  (noir / lab / mono); `scene_dashboard` as the new default scene;
  tool icons; heap watchdog.
- **Bridge**: pluggable transport `--port-kind {serial,tcp}`,
  `~/.claude-buddy/config.toml` + 11-flag CLI surface, `status` +
  `bench` subcommands, persistent reader thread (closes G-1's
  steady-state floor), reboot detection, multi-agent
  `SessionRegistry`.
- **Brand**: logo + dark variant + wordmark + favicon + social card +
  palette.md + hero PNG + scenes strip.
- **Stress**: 5-test suite (`tools/stress.py`) — flood / oversize /
  reconnect / prompt-latency / idle-keepalive. 5/5 PASS at 99 snap/s.

### Fixed (upstream)
- `esp-harness@fb5a549` — G-8: `esp_harness.core.parser` Python
  reference tokeniser + 25 parity tests. Closes consumer-mock drift
  class structurally.
- `esp-harness@85770d8` — G-8 corpus extension: whitespace-inside-JSON
  parity cases (surfaced by H1's accidental tokeniser re-implementation).

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

[Unreleased]: https://github.com/Caldis/esp32-agent-dashboard/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.2.0
[0.1.2]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.1.2
[0.1.1]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.1.1
[0.1.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.1.0


