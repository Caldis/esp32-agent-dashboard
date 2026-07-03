# CLAUDE.md

## Bootstrap
Run `esp-harness manifest --json` to discover all project capabilities.
Do this at the start of every session.

## Development Cycle
Run `esp-harness cycle` after code changes (build + flash + verify).

## Adding Features
- New scene: create `main/scenes/scene_<name>.c`, register in `esp32_agent_dashboard_main.c`
- New command: `console_protocol_register()` -- auto-surfaces in manifest
- New module: `esp-harness add <module>`

## Verification
- `esp-harness screenshot` -- capture device screen
- `esp-harness verify` -- screenshot + visual regression
- `esp-harness console --cmd "?stat" --json` -- device health

## Key Files
- `harness.json` -- project config (board=esp32_s3_touch_amoled_2_16, port=COM9, modules)
- `main/esp32_agent_dashboard_main.c` -- entry point
- `main/scenes/` -- 4 UI scenes (dashboard, idle, prompt, awaiting)
- `tools/claude_buddy_bridge.py` -- host bridge daemon
- `tools/hook_dispatch.py` -- Claude Code hook forwarder

## Bridge (self-healing)
The bridge occupies COM9 while running. To use `esp-harness screenshot` or `esp-harness flash` directly, stop the bridge first.
Start: `python tools/claude_buddy_bridge.py serve --serial-port COM9`

You normally DON'T need to start it by hand: `hook_dispatch.py` AUTO-STARTS the
bridge whenever a hook can't reach one (set `CLAUDE_BUDDY_AUTOSTART=0` to opt
out). A single-instance guard makes duplicate starts exit cleanly, and the
bridge reconnects to the device indefinitely (serial open is watchdog-bounded,
so a wedged COM9 never freezes it). The device shows "· 等待主机连接 ·" /
"· 主机已断开 ·" when the snapshot stream goes stale rather than freezing.
