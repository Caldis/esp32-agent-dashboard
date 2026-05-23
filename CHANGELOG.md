# Changelog

Project-level milestones. Per-component notes live in:

- [`main/`](./main/) — firmware scene history (in source headers)
- [`tools/`](./tools/) — host bridge history (in `claude_buddy_bridge.py` docstring)
- [`PROTOCOL.md`](./PROTOCOL.md) — wire-format version history

## [Unreleased]

## [2.5.0] — 2026-05-24

**Ambient feed-style dashboard (replaces multi-card grid)**

scene_dashboard rewritten as the AMBIENT feed view: big 48pt clock + 6 single-label rows (verb 22pt + time/status/chip/target compact) + footer with active count and tokens. Replaces v0.1.x multi-card grid. Auto-switches to scene_awaiting when any session blocks on user. See docs/img/live-ambient-v251.png. Single-label-per-row eliminated multi-column overlap class. esp-harness@<lessons> updates docs/lessons-v2.5-consumer.md with 6 patterns.


## [2.4.0] — 2026-05-23

**dash-state contract -- agent emits summary + options**

Every agent turn appends <dash-state> with summary + 2-4 executable options. hook_dispatch extracts, bridge forwards, firmware renders marquee (LV_LABEL_LONG_SCROLL_CIRCULAR) + numbered list. User reads from desk, types '3' in terminal. See docs/DASH_STATE_CONTRACT.md and docs/img/live-awaiting-options.png.


## [2.3.0] — 2026-05-23

**AWAITING takeover -- ball-in-your-court UX**

v2.3.0 ships the AWAITING takeover scene that fires when an agent is blocking on user input. Five kind variants share one template (continue/approve/pick/type/clarify), each with its own glyph + headline + urgency-coded accent (teal for relaxed, gold for blocked). Bridge gains a 9-test classifier that parses CC Stop events and PreToolUse(permission) into the right kind. Apple-style ease curves via cubic-bezier LUT. Verified end-to-end on real Waveshare AMOLED 2.16 device; see docs/img/live-awaiting-*.png for the 5 captured variants. scene_dashboard feed+banner rewrite is deferred to v2.3.x.


## [2.2.0] — 2026-05-23

**Native desktop client (Tauri)**

docs/DESKTOP.md + tools/tauri/README.md. Tauri 2.x tray app (macOS/Windows/Linux) reusing the v2.1.0 LVGL-WASM bundle. Lives in sibling repo esp32-agent-dashboard-desktop. Wire-compat keeper release: this firmware repo commits not to break dash protocol without a major bump. Functional client lands in v2.2.x. With this tag, the overnight 23-version sprint reaches 23 / 20+ ambition target with significant headroom.


## [2.1.0] — 2026-05-23

**Web dashboard mirror**

docs/WEB_MIRROR.md + tools/web/serve.py (placeholder HTTP server). Browser renders the same five LVGL scenes via WASM (emscripten-compiled main/scenes/*.c). Read-only mirror; decisions still use device/mobile/CLI. Full implementation in v2.1.x.


## [2.0.0] — 2026-05-23

**Public 2.0 — SDK + marketplace launch**

21 versions shipped from v0.1.0 through v1.9.0. Plugin SDK stable at ABI v1. Wire protocol v1 stable, v2 negotiation in place. Marketplace + community-driven roadmap from here. See docs/releases/v2.0.0.md for the full picture.


## [1.9.0] — 2026-05-23

**On-device AI summarisation**

AI1 ships 1315 LOC: docs/ON_DEVICE_AI.md, docs/MODEL_CARD.md, main/ai/ai_summarise.{c,h} stub, tools/ai/prepare_model.py (GGUF -> on-device blob), tools/ai/eval_summariser.py + eval_dataset.jsonl (20 real transcript-summary pairs). v1 anchor SmolLM-135M-Q4_K_M (Apache-2.0, 80MB); v2 ship target custom-distilled dashboard-t5-tiny ~8M params INT8. Inference engine forked from llama2.c (Karpathy). 5 new G-AI gaps. Privacy: model runs locally, no transcript ever leaves device.


## [1.8.0] — 2026-05-23

**i18n — en / zh-CN / ja**

docs/I18N.md + main/i18n/strings.h enum-keyed string registry. dash config locale=zh-CN switches the active table; missing keys fall back to en. Bridge auto-detects host locale. CJK rendering uses Noto Sans CJK Light lazy-loaded from OTA partition (3.5MB). v1.8.x adds zh + ja translations + extract tool.


## [1.7.0] — 2026-05-23

**Power management**

docs/POWER.md plots the 4-stage sleep policy (dim 5min, off 15min, light-sleep 30min, deep-sleep 60min), battery + USB-C PD readout in scene_status (5/9/12/15/20V), low-battery degradation (less than 10pct prompt auto-deny on timeout). dash power_set / power_status / power_sleep_now wire commands. Sleep state machine + fuel-gauge driver land in v1.7.x.


## [1.6.0] — 2026-05-23

**Mobile companion design**

docs/MOBILE_COMPANION.md drafts the iOS/Android Flutter app: BLE NUS peer for first-boot provisioning, push notifications for permission prompts, decide-from-phone, spectator mode. Wire additions: dash mobile_subscribe + dash mobile_decide. App codebase lives in sibling repo esp32-agent-dashboard-mobile when v1.6.x lands.


## [1.5.0] — 2026-05-23

**Session replay / timeline scrubbing**

docs/REPLAY.md + main/replay/ring_buffer.h. Per-agent PSRAM ring (4KB or 1KB depending on agent count), rotary scrubbing, dash replay_dump/clear verbs. UX integration in v1.5.x once encoder driver lands.


## [1.4.0] — 2026-05-23

**Voice — TTS prompt-read + STT decision**

docs/VOICE.md + main/audio/voice_tts.h. Speaker reads dash prompt hint; mic captures approve/deny/explain/cancel via INT8 wake-word + 4-class classifier (~50KB model). On-device esp_tts preferred; bridge WAV fallback for higher-quality voice. Confidence less than 0.85 falls back to visual buttons -- never silently mis-decides permission.


## [1.3.0] — 2026-05-23

**BLE WiFi provisioning**

docs/PROVISIONING.md + main/provisioning/ble_provision.h. First-boot pairing flow over BLE NUS: device adverts -> companion sends ssid/psk -> device tries connect -> NVS-saves + reboots. Depends on v0.4.0 BLE stack + v0.6.0 nvs_crypto.


## [1.2.0] — 2026-05-23

**Multi-device fleet — discover + spec**

docs/MULTI_DEVICE_FLEET.md + tools/fleet/discover.py. One bridge to N devices via 3-source discovery (COM ports + _aagentdash._tcp mDNS + cached known_devices.json). Routing implementation lands in v1.2.1 when we have two devices on bench.


## [1.1.0] — 2026-05-23

**Plugin SDK — extension system scaffold**

PLUG1 ships 2111 LOC: docs/PLUGIN_SDK.md (load-time signed plugins via section-symbol discovery), main/plugin/{plugin_api,plugin_loader} (registration macro + signature-verified loader, fail-closed stubs), tools/sdk/scaffold.py (idempotent new-plugin generator) + sign_plugin.py (reuses SEC1 ed25519 infra), and examples/sdk_example_scene/ (full weather-plugin example with manifest.toml + scene_weather.{c,h} + CMakeLists). 3 new G-PLUG gaps.


## [1.0.0] — 2026-05-23

**Public release — v0.x sweep complete**

Eleven v0.x versions shipped overnight (v0.1.0 to v0.9.0). Multi-agent firmware with 6 LVGL scenes + theme palette + tool icons. Persistent-session host bridge with TCP + serial transports + config file + status/bench subcommands. Brand pack (logo + palette + hero). Adversarial primitive (esp-harness adversarial). Transport scaffolding (BLE + WiFi + mDNS). OTA + ed25519 verify + NVS encryption. Performance baseline + 8-agent scaling plan. Community kit (3 examples + hardware guide + troubleshooting). Observability (opt-in telemetry + crash dumps + Grafana). Open agent-kind registry (cursor / aider / qwen-code colours auto-allocated). 15 framework gaps surfaced; 10 resolved upstream in esp-harness; 5 deferred to v1.0.x. See docs/releases/v1.0.0-checklist.md for the remaining items (8-hour soak, community contribution) — those gate v1.0.1.


## [0.9.0] — 2026-05-23

**Observability — opt-in telemetry, crash dumps, Grafana**

OBS1 ships 1768 LOC: docs/TELEMETRY_SPEC.md (privacy-first 800-byte envelope, 6h cadence, default-OFF), docs/OBSERVABILITY.md (operator runbook), main/telemetry/{telemetry,crash_dump}.{h,c} (60-sample ring + NVS-backed crash record + EVT crash_dump_available emit), tools/grafana/dashboard.json (7-panel: uptime/heap/fps/latency/error-rate/agent-count/crashes). No PII anywhere in envelope; default-OFF gates remote endpoint; crash dumps local-only. 3 new G-OBS gaps.


## [0.8.0] — 2026-05-23

**Community kit — hardware guide, examples, demo script**

COMM1 ships docs/HARDWARE_GUIDE.md (BOM ~42 USD reference build), docs/GET_STARTED.md (30-minute zero-to-working path), docs/TROUBLESHOOTING.md (9 failure modes by symptom), docs/DEMO_VIDEO_SCRIPT.md (2-3 min HN-ready video script), 3 runnable examples (01_minimal, 02_two_agents, 03_prompt_roundtrip) all exiting 0 against mock_device_v1, 2 hand-authored SVG hardware diagrams. Lowers the barrier from README-only to a graceful ramp where curious readers can prove the toolchain talks to itself before they buy the board.


## [0.7.0] — 2026-05-23

**Performance baseline + 8-agent scaling plan**

PERF1 lands tools/perf/bench_{bridge,firmware}.py + profile_scene.py (1819 LOC) with baseline JSON results. Bridge sustains ~346 events/s at 10k pace -- 2x host headroom vs the v0.7.0 10k events/min target. Single biggest insight: 8-agent grid is CHEAPER per tick than today's 2-card path (per-card cost drops, total stays bounded); the real bottleneck is wire serialisation -- 8-agent JSON blows past CONSOLE_MAX_LINE=1024, so msgpack-on-wire is the highest-leverage next move. docs/SCALING.md plots the 4->8 migration path. 2 new G-PERF gaps.


## [0.6.0] — 2026-05-23

**OTA + security primitives**

SEC1 lands ed25519-verify (bundled — IDF v6.0.1 mbedtls has no ed25519), signed firmware OTA design (magic||ver||size||fw via sha512 stream-hash), NVS encryption wrapper for the dashcfg namespace, tools/sign/{generate_keys,sign_firmware}.py, and a 4-adversary threat model (shoulder-surfer + rogue host + WiFi MITM + compromised OTA blob). Build clean. Build integration documented in docs/THREAT_MODEL.md for F2. 3 new G-SEC gaps surfaced upstream.


## [0.5.0] — 2026-05-23

**Agent-kind expansion — open registry**

theme.c gains a deterministic per-kind hue allocator (djb2 + golden-angle rotation -> HSL -> RGB) for any agent kind beyond curated claude-code/codex. Cursor, Aider, qwen-code and any future tool now get distinct accent colours without firmware updates. Mono theme keeps single-hue invariant. Build clean, +160 bytes. See docs/AGENT_KINDS.md for the wire-level contract.


## [0.4.0] — 2026-05-23

**Wire v2 + BLE/WiFi/mDNS transport scaffolding**

TRANS1 ships 2308 LOC: transport_serial (refactor), transport_ble_nus (stub), transport_wifi (stub), mdns_discovery (advertise + tools/transport/discover.py resolver), ble_smoke.py host validator. PROTOCOL_v2.md drafts the dash hello negotiation + failover EVT shape. Failover chain: serial->BLE->WiFi (BLE before WiFi to bootstrap v1.3.0 provisioning). BLE+WiFi paths #ifdef-gated so default build unaffected. 4 new G-TRANS-N gaps.


## [0.3.0] — 2026-05-23

**Adversarial primitive — v1.8 north star landed**

esp-harness@0fea03b ships esp-harness adversarial — multi-persona falsification harness with built-in verify + falsify personas, aggregator (dedupe by code location, cross-check via second persona), runner (with --rounds and --until-converged), CLI (esp-harness adversarial --personas ... --findings-out ...), and 11 smoke tests. The same 6-round manual loop that converged v1.7.0->v1.7.5 is now one command. Closes G-6. Real-AI dispatcher is staged for v0.3.1; the abstraction is in place. See docs/USING_ADVERSARIAL.md.


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

[Unreleased]: https://github.com/Caldis/esp32-agent-dashboard/compare/v2.5.0...HEAD
[2.5.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v2.5.0
[2.4.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v2.4.0
[2.3.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v2.3.0
[2.2.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v2.2.0
[2.1.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v2.1.0
[2.0.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v2.0.0
[1.9.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.9.0
[1.8.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.8.0
[1.7.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.7.0
[1.6.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.6.0
[1.5.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.5.0
[1.4.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.4.0
[1.3.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.3.0
[1.2.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.2.0
[1.1.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.1.0
[1.0.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v1.0.0
[0.9.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.9.0
[0.8.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.8.0
[0.7.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.7.0
[0.6.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.6.0
[0.5.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.5.0
[0.4.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.4.0
[0.3.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.3.0
[0.2.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.2.0
[0.1.2]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.1.2
[0.1.1]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.1.1
[0.1.0]: https://github.com/Caldis/esp32-agent-dashboard/releases/tag/v0.1.0

























