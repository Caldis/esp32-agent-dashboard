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
- `main/scenes/` -- UI scenes (dashboard, overview[wire id "idle"], clock, prompt, awaiting)
- `main/ui_type.h` -- THE type scale. All text takes fonts from its five
  tiers (CAPTION 20 / LABEL 26 / BODY 36 / TITLE 52 / HERO 88) via
  ui_type()/ui_type_bold(); raw ui_font(px)/cjk_font(px) are internal to
  ui_type.c. Sized for 0.6-1 m viewing distance on the 2.16" 466x466
  (305 ppi) panel -- do NOT introduce ad-hoc pixel sizes or anything
  below CAPTION.
- `tools/claude_buddy_bridge.py` -- host bridge daemon
- `tools/hook_dispatch.py` -- Claude Code hook forwarder

## Rendering performance (hard-won, do not regress)
- NEVER per-frame animate size/transform_scale/widget-opa on big tiny_ttf
  labels or containers overlapping them (marquee freeze, clock plan A,
  overview ring breath, prompt pulse -- all bisected to this). Use
  low-frequency stepped styles from the scene tick instead.
- Full-size `esp-harness screenshot --size 466` streams ~5 s and rides the
  TWDT edge; any extra render load mid-dump reboots the device. Verify
  with `--size 320` (~2.5 s, safe).

## Bridge (self-healing)
The bridge occupies COM9 while running. To use `esp-harness screenshot` or `esp-harness flash` directly, stop the bridge first.
Start: `python tools/claude_buddy_bridge.py serve --port-kind serial --port COM9`

You normally DON'T need to start it by hand: `hook_dispatch.py` AUTO-STARTS the
bridge whenever a hook can't reach one (set `CLAUDE_BUDDY_AUTOSTART=0` to opt
out). A single-instance guard makes duplicate starts exit cleanly, and the
bridge reconnects to the device indefinitely (serial open is watchdog-bounded,
so a wedged COM9 never freezes it). Port loss is detected in <1s (reader-thread
on_close callback, not next-push failure); every reconnect/reboot force-pushes
config + time + the full live snapshot immediately, plus a second idempotent
push 3s later — a reboot-triggered push can land mid-boot and be lost (config
and time have no keepalive to self-heal through, unlike snapshots), so the
device never waits for the next keepalive to resync. Firmware side, app_main
registers ALL console commands BEFORE console_protocol_init() starts listening
(register-then-listen), so a mid-boot push buffered in USB-CDC applies once the
console starts instead of bouncing "unknown command: dash" — that bounce was
the permanent-"--:--"-clock bug. When the snapshot stream goes stale the
device shows a small top-center dot within 12s (CONN_STALE_MS) — amber =
never connected, red = host lost — instead of freezing silently (v4.7:
the old "· 等待主机连接 ·"/"· 主机已断开 ·" text pill was demoted to this
dot). After `offline_clock_min` minutes of silence (config, default 5,
0 disables) the device retreats to the clock scene, suppressing any
stale awaiting takeover; reconnect re-fires it from the fresh snapshot.

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
