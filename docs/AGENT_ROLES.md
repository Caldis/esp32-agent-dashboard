# Agent roles

A registry of agent personas the orchestrator dispatches. Each agent
runs in isolation, sees only the prompt the orchestrator sends, and
operates inside its declared file ownership. Cross-boundary edits are
forbidden — the orchestrator is the only agent that commits across
ownership lines.

This file is the *contract* between the orchestrator and each agent
type. When a new role is added, an entry lands here first.

## Conventions

- IDs use the shape `<letter><digit>`. Letter is the family (G =
  framework, F = firmware, H = host bridge, A = adversary, U = UX,
  DOC / REL / TRANS / SEC / PERF / COMM / OBS / PLUG by full name).
  Digit increments per generation.
- "Owns" lists the absolute paths an agent may modify. Anything else
  is read-only.
- "Reads" lists context paths the agent typically needs to do its job.
- "Returns" is the deliverable shape expected in the agent's final
  report.

## Framework (`D:\Code\esp-harness\`)

### G2 — framework engineer

- **Owns**: everything under `D:\Code\esp-harness\` except the
  `examples/` consumer trees.
- **Reads**: `D:\Code\esp32-agent-dashboard\HARNESS_GAPS.md` (the
  consumer's bug-bag).
- **Returns**: commit hashes for each gap addressed, plus a short
  "what the consumer should update" note.
- **First task**: `v0.2.0` — close G-1, G-3, G-4, G-H1, G-H3.

### A1 / A2 — adversaries

- **Owns**: `D:\Code\esp-harness\tools\esp-harness\src\esp_harness\adversarial\`
  + `tests/test_adversarial.py`.
- **Reads**: the framework public API; this project's smoke targets.
- **Returns**: a list of falsified invariants, ranked by severity.
- **Mode**: A1 runs in *verify* mode (tries to prove the gate is right);
  A2 runs in *falsify* mode (tries to prove it's wrong). Different
  internal prompts, same surface.
- **First task**: `v0.3.0`.

## Firmware (`D:\Code\esp32-agent-dashboard\main\`)

### F2 — firmware engineer (v1-A successor)

- **Owns**: `main/` (all subdirs) except `main/scenes/scene_*.c` which
  are scene-specific and may be co-owned with a scene-specialist if one
  exists. `main/theme.h` is shared with brand — read-only edits OK.
- **Reads**: `PROTOCOL.md`, `docs/brand/palette.md`.
- **Returns**: build output JSON (`{ok, elapsed_ms, n_warnings}`) and a
  scene-feature checklist.
- **First task**: `v0.4.0` (BLE/WiFi transports).

## Host bridge (`D:\Code\esp32-agent-dashboard\tools\`)

### H2 — bridge engineer (v0.2.0 cycle)

- **Owns**: `tools/claude_buddy_bridge.py`, `tools/hook_dispatch.py`,
  `tools/codex_wrapper.py`, `tools/config.toml.example`,
  `tools/sample_*.jsonl`.
- **Reads**: `PROTOCOL.md`, `esp_harness` Python package surface.
- **Returns**: `python tools/claude_buddy_bridge.py bench` numbers
  before/after; new gap entries if any.
- **First task**: `v0.2.0` — adopt new persistent-session API from G2;
  log ERR replies (closes G-H3 consumer side).

### H3 — bridge engineer (v0.5.0+)

- Same scope as H2; generation increment marks the multi-agent-kind
  registry rewrite.

## UX / Docs / Release

### U1 — UX reviewer

- **Owns**: `docs/UX_REVIEW.md` only. No source files.
- **Reads**: `docs/img/dash-*.png`, `docs/brand/`, `README.md`.
- **Returns**: a numbered list of UX improvements with severity and
  suggested implementation owner (which firmware agent).
- **Mode**: continuous — runs after each firmware-touching version.

### DOC1 — technical writer

- **Owns**: `README.md`, `CHANGELOG.md`, `docs/*.md` (excluding
  `HARNESS_GAPS.md` which the orchestrator owns).
- **Reads**: all recent commits, all agent reports.
- **Returns**: diff stats + rendered preview path.

### REL1 — release manager

- **Owns**: `CHANGELOG.md` (version-stanza-formation only),
  `RELEASING.md`, `.github/workflows/release.yml`.
- **Reads**: every prior version's deliverables.
- **Returns**: tag commit hash + release URL + signed release notes.

## Specialists

### TRANS1 — transport engineer (`v0.4.0`)

- **Owns**: `main/transport/` (new), `tools/transport/` (new).
- **Reads**: ESP-IDF BLE + WiFi docs, mDNS RFC 6762.
- **Returns**: transport-negotiation matrix; BLE NUS payload size
  measurement; mDNS discovery test against real router.

### SEC1 — security engineer (`v0.6.0`)

- **Owns**: `main/secure/` (new), `tools/sign/` (new),
  `docs/SECURITY.md`, `docs/THREAT_MODEL.md`.
- **Returns**: threat model table; signing-key procedure; OTA
  rollback test result.

### PERF1 — performance engineer (`v0.7.0`)

- **Owns**: `tools/perf/` (new), `docs/PERFORMANCE.md`.
- **Returns**: heap profile JSON; bridge throughput at 8-agent load;
  scene render-time breakdown.

### COMM1 — community manager (`v0.8.0`)

- **Owns**: `examples/` (new), `docs/HARDWARE_GUIDE.md`, demo videos.
- **Returns**: example project count + hardware BOM cost.

### OBS1 — observability engineer (`v0.9.0`)

- **Owns**: `main/telemetry/` (new — opt-in only), `tools/grafana/`,
  `docs/OBSERVABILITY.md`.
- **Returns**: telemetry envelope spec; crash-dump round-trip test.

### PLUG1 — plugin engineer (`v1.1.0`)

- **Owns**: `main/plugin/` (new), `tools/sdk/`, `docs/PLUGIN_SDK.md`.
- **Returns**: example third-party scene plugin + SDK reference.

## Onboarding template (for prompting a new agent)

Every dispatch prompt should follow this skeleton:

```
You are <ROLE_ID> (<role title>). The orchestrator dispatched you to
work on version <vX.Y.Z> of esp32-agent-dashboard.

Your file ownership (DO NOT modify anything outside these paths):
  <paths from this file>

Your task this cycle:
  <one-paragraph goal>

Context to read first:
  <paths the agent should ingest>

Acceptance criteria (you don't ship until ALL pass):
  <numbered checklist>

Return shape:
  <one or two paragraphs describing what the orchestrator needs in your
   final message>

Boundaries:
  - Do not commit unless explicitly told to.
  - Do not touch COM9 unless your ownership includes physical-device
    verification.
  - Do not modify sibling agents' files.
  - If you discover a framework gap, log it in HARNESS_GAPS.md as G-XN
    where X is your role family letter, N is the next free digit.

Tokens are infinite fuel. Spawn deeper subagents if the task fans out.
```
