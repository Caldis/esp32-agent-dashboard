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

### Flashing (COM9 contention with autostart)
`idf.py flash` needs exclusive COM9, but stopping the bridge makes the next
tool-call hook auto-start a new one that grabs the port mid-flash (esptool then
fails to connect). Before flashing, disable autostart for the duration, e.g.:
`printf '{"start_ts": %d}' $(($(date +%s)+3600)) > "$TEMP/claude_buddy_hook_state.json"`
(a future `start_ts` puts every hook in cooldown so none spawns), then kill the
bridge and flash; delete that state file afterwards to re-enable. Or just
`export CLAUDE_BUDDY_AUTOSTART=0` in the shell that owns the hooks.

Device font is a GB2312 SimHei subset (`main/zh.ttf`). The bridge gates all
device-bound text through its actual cmap (`_device_safe`), mapping
unrenderable chars (fullwidth punct, ✓→v, emoji) to ASCII or dropping them, so
".notdef" boxes can't appear. To render a NEW glyph on-device you must add it to
the font subset (`tools/make_cjk_font.py`) AND reflash — host mapping only
downgrades.

### Build/flash toolchain (this machine)
ESP-IDF v6.0.1 via EIM. Activate in PowerShell before `idf.py`:
`. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'`
