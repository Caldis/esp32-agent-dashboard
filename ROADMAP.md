# Roadmap

Goal: ship at least 20 more versions (`v0.2.0` → `v2.2.0`) overnight,
each with real value-deliver, while continuing to harden the
underlying esp-harness framework via the gaps this project surfaces.

Mantra: **"tokens are infinite fuel"** — use parallel subagents
aggressively, fail-fast then converge, never accept a version with
broken build, broken tests, or broken docs.

## Versions

| Version | Theme | Headline deliverables | Driver agents |
|---|---|---|---|
| v0.2.0 | Framework hardening | close `G-1` (persistent console session), `G-3` (shared port API), `G-4` (self-describing manifest), `G-H1` (`PayloadFollowsReader`), `G-H3` (ERR-callback). Bridge swaps subprocess loop for the new API. | G2, H2 |
| v0.3.0 | Adversarial primitive | `esp-harness adversarial` command: spawns N falsification agents that try to break the consumer's smoke gate; bug count → 0 over rounds. v1.8 north star landed. | G2, A1, A2 |
| v0.4.0 | Wire v2 + transports | BLE NUS transport + WiFi push + mDNS discovery, fallback chain. PROTOCOL.md v2 with explicit transport negotiation. | TRANS1, F2 |
| v0.5.0 | Agent kind expansion | First-class support for Cursor, Aider, qwen-code in addition to Claude Code / Codex. Pluggable agent-kind registry in firmware + bridge. Theme.h auto-allocates accent colours. | F2, H3 |
| v0.6.0 | OTA + security | NVS-encrypted credentials, signed firmware OTA over WiFi, TLS for bridge↔device WiFi, signed snapshot envelopes (anti-spoof). | SEC1, F2 |
| v0.7.0 | Performance + scaling | Support 8 concurrent agents on one device; scene_dashboard adapts grid 1→2→4→8. Bridge handles 10k events/min sustained. Memory/heap profiling docs. | PERF1, F2 |
| v0.8.0 | Community kit | Example mini-projects, hardware purchase guide, soldering steps, "build it yourself" video script. README rewrite for newcomers. | COMM1, DOC1 |
| v0.9.0 | Observability | Opt-in anonymous telemetry, on-device crash dump capture, perf metrics exposed via `dash health`, Grafana dashboard JSON. | OBS1, H3 |
| v1.0.0 | Public RC | All prior versions stabilised. Public release. HN/Reddit-ready demo video. Full E2E test gate. | REL1, all |
| v1.1.0 | Plugin/extension system | Third-party scene plugins via signed component packs. SDK doc. | PLUG1, F2 |
| v1.2.0 | Multi-device fleet | One bridge ↔ N devices ("wall of dashboards"). Device discovery + per-device routing. | TRANS1, H3 |
| v1.3.0 | WiFi BLE provisioning | First-boot pairing UX: phone-app or laptop-side wizard puts wifi creds via BLE. No more `.env` hard-codes. | TRANS1, U2 |
| v1.4.0 | Voice (TTS prompt-read + STT reply) | Speaker reads `dash prompt` hint; mic captures "yes" / "no" / "explain" — same physical decision as the buttons, without picking up the device. | F3, AUDIO1 |
| v1.5.0 | Session replay / timeline scrubbing | Per-agent recording on device (last 30 min ring); rotate-and-press scrubs back, exports to bridge as JSONL. | F3, H4 |
| v1.6.0 | Mobile companion (iOS / Android BLE) | Thin app: mirrors scenes, push notifs for prompts, decide-from-phone. | MOBILE1 |
| v1.7.0 | Power management | Deep-sleep when idle 30 min; wake on `dash prompt` BLE adv; USB-C PD negotiation; battery + charge gauge in scene_status. | F3, SEC1 |
| v1.8.0 | i18n (zh-CN / en / ja) | Per-device language pref; bridge auto-detects host locale. Brand SVGs internationalised. | F3, COMM2 |
| v1.9.0 | On-device AI summarisation | Tiny Qwen / Phi distilled model embedded; summarises 10-line transcript → one-line msg field. Runs on PSRAM. | AI1, F3 |
| v2.0.0 | Public 2.0 — SDK + marketplace launch | Plugin marketplace landing; SDK 1.0 docs; press kit; HN/Reddit launch. | REL2, COMM1 |
| v2.1.0 | Web dashboard mirror | Same scenes rendered in-browser by the bridge for desk-without-device dev. WASM lvgl. | F3, WEB1 |
| v2.2.0 | Native desktop client (Tauri) | macOS + Windows tray app with the full scene set; long-term stretch goal. | WEB1, COMM1 |

## New agent roles (the "team" I'm hiring)

Documented in [`docs/AGENT_ROLES.md`](docs/AGENT_ROLES.md). Summary:

| Role | Owns | First version |
|---|---|---|
| **G2** (framework engineer) | `D:\Code\esp-harness\` core | v0.2.0 |
| **A1 / A2** (adversaries) | adversarial-primitive falsifiers | v0.3.0 |
| **U1** (UX reviewer) | visual critique, no source ownership | continuous |
| **DOC1** (technical writer) | `README.md`, `CHANGELOG.md`, `docs/*.md` (non-tech) | continuous |
| **REL1** (release manager) | version bumps, tags, release notes | every version |
| **TRANS1** (transport engineer) | BLE / WiFi / mDNS layers | v0.4.0 |
| **F2** (firmware engineer, v1-A successor) | `main/` everything except scene-specific | v0.4.0+ |
| **H2 / H3** (bridge engineers, v1-B successors) | `tools/claude_buddy_bridge.py` + adjacent | v0.2.0+ |
| **SEC1** (security engineer) | OTA, NVS crypto, threat model | v0.6.0 |
| **PERF1** (performance engineer) | profiling, scaling, benchmarks | v0.7.0 |
| **COMM1** (community manager) | examples, demos, gallery | v0.8.0 |
| **OBS1** (observability engineer) | telemetry, crash dumps, dashboards | v0.9.0 |
| **PLUG1** (plugin engineer) | extension SDK | v1.1.0 |
| **U2** (UX reviewer, gen 2) | provisioning flow critique | v1.3.0 |
| **AUDIO1** (audio engineer) | TTS + STT integration | v1.4.0 |
| **F3** (firmware engineer, gen 3) | replay / power / i18n | v1.5.0+ |
| **H4** (bridge engineer, gen 4) | replay + export pipeline | v1.5.0 |
| **MOBILE1** (mobile engineer) | iOS/Android companion app | v1.6.0 |
| **COMM2** (i18n / community gen 2) | localisation + outreach | v1.8.0 |
| **AI1** (on-device AI engineer) | tiny LLM integration | v1.9.0 |
| **REL2** (release manager, gen 2) | 2.0 launch coordinator | v2.0.0 |
| **WEB1** (web/desktop engineer) | browser + Tauri ports | v2.1.0+ |

## Quality gates (every version)

Before tagging:
1. Firmware builds clean (0 warnings) with esp-idf v6.0.1.
2. `tools/stress.py --all --port 127.0.0.1:<mock>` returns 5/5.
3. esp-harness `pytest -q` passes (parity + manifest + doctor + sim-diff).
4. `claude_buddy_bridge.py replay tools/sample_dual.jsonl --dry-run` exits 0
   with no unrecognised events.
5. README hero + brand images resolve (no 404 in `git ls-files docs/img/`).
6. CHANGELOG `[Unreleased]` cleared into the new version stanza.
7. v_N.M.K tag + push + GitHub release notes.

Failing any gate → version doesn't ship. No "we'll fix it next version".

## Boundaries

- COM9 (real device) is a finite shared resource. Only one agent flashes
  at a time. Other agents use `docs/mock_device.py` (v0) or
  `tools/mock_device_v1.py` (v1 + multi-agent) on TCP.
- esp-harness commits go to `D:\Code\esp-harness\master`. Dashboard
  commits go to `D:\Code\esp32-agent-dashboard\master`. Agents do NOT
  cross.
- Sibling agents do NOT modify each other's files. The orchestrator
  (me) does final integration commits.

## Off-roadmap principles

- "Token-as-electricity" — spawn parallel subagents whenever
  independent work can be split, even if the cost looks "wasteful".
  A failed branch costs less than a slow serial sweep.
- Adversarial > validation. Every commit is one bug-finder away from
  proving itself wrong. Make that bug-finder a first-class harness
  primitive (`v0.3.0`).
- Framework benefits the consumer benefits the framework. Every gap
  surfaced here either lands as a code fix upstream or as a documented
  recipe so the next consumer doesn't repeat the discovery.
