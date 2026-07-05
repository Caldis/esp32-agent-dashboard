# Current Architecture

This file records what is part of the current shipping build, and what is
scaffold or design material. Treat `main/CMakeLists.txt` as the build source
of truth; this file explains that truth in one place.

## Current Firmware Modules

The current ESP32 firmware build includes:

- Entry and boot wiring: `main/esp32_agent_dashboard_main.c`
- Shared state: `main/agent_state.c`
- Physical buttons: `main/buttons.c`
- Theme and icons: `main/theme.c`, `main/tool_icons.c`
- JSON parser: `main/tiny_json.c`
- Console command Interface: `main/harness/agent_commands.c`
- Scenes (4, registered in `esp32_agent_dashboard_main.c`): `dashboard`,
  `idle`, `prompt`, `awaiting`. (`sessions`, `tokens`, `status` were removed
  in commit 1a3036c — do not reintroduce references to them.)
- Animation helper: `main/anim/apple_ease.c`

These Modules are linked with `esp-harness-core`, LVGL, the Waveshare board
Module, `nvs_flash`, `esp_timer`, and the Espressif button dependency.

## Current Host Modules

The current host path includes:

- `tools/claude_buddy_bridge.py`: long-running bridge, registry, publisher,
  transport session, CLI modes
- `tools/bridge_runtime.py`: esp-harness Python runtime discovery
- `tools/awaiting_classifier.py`: text-to-awaiting-kind classification
- `tools/hook_dispatch.py`: short-lived Claude Code hook forwarder
- `tools/codex_wrapper.py`: best-effort Codex JSONL wrapper
- `tools/mock_device_v1.py`: TCP mock for bridge and CI checks

## Scaffold Modules

These files are intentionally not part of the current firmware build unless
`main/CMakeLists.txt` is changed:

- `main/transport/*`: serial abstraction plus BLE NUS / WiFi TLS scaffold
- `main/telemetry/*`: telemetry and crash dump scaffold
- `main/secure/*`: OTA / NVS crypto / signature scaffold
- `main/plugin/*`: plugin loader scaffold
- `main/ai/*`: on-device summary scaffold
- `main/audio/*`: voice scaffold
- `main/replay/*`: replay data structure scaffold
- `main/provisioning/*`: provisioning scaffold

Docs may describe these as planned or scaffolded capabilities. They are not
current runtime behaviour until they enter `main/CMakeLists.txt` and pass the
normal build / flash / verify cycle.

## Architecture Rules

- If a new firmware file is meant to ship now, add it to `main/CMakeLists.txt`
  in the same change.
- If a file is scaffold only, say so in its header and keep docs language in
  future tense or scaffold tense.
- Host-only Modules should be executable by direct `python tools/<name>.py`
  commands and covered by a small script test when practical.
- Runtime discovery must prefer installed packages and explicit environment
  variables before local development fallbacks.
