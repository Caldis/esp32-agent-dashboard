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
- `main/scenes/` -- UI scenes (dashboard, weather, clock). Retired,
  sources kept unbuilt: overview + prompt (v5.2; `dash idle` aliases to
  dashboard, `dash prompt` is a no-op ACK; prompt_active must stay
  false forever -- see agent_snapshot_apply.c) and awaiting (v6.0 --
  see below). BOOT cycles dashboard <-> weather. v6.0: the dashboard
  gold pose IS the "your turn" view -- ring + TITLE greeting word
  (8-word rotation for CONTINUE, fixed word for approve/pick/type/
  clarify) + project chip ("cc esp32-agent-dashboard"); the word is
  TITLE(52) in ALL states so it never changes size; the cluster
  ink-centres in the chrome band and glides between its chip/no-chip
  poses (ambient_slide_to). A fresh awaiting RISING edge (gated on
  host alive) PULLS the display to the dashboard, one-shot: keying
  away is final for that round, reconnect re-push re-fires (v4.7
  contract), stale awaiting can't pull while host_lost. There is no
  dedicated takeover page and no dismissal machinery any more.
  Colour follows STATE device-wide, never the page: gold = your move,
  teal = thinking, dim = idle. v5.3: the pet mascot is retired (pet.c
  kept unbuilt) — the breathing pulse ring is the ONE status glyph
  device-wide, colour = state. The panel's single core job: tell the
  user an agent finished its turn — the hook→bridge→snapshot
  (awaiting_kind)→pull-to-dashboard chain is the critical path;
  everything else is ambience.
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
USE THE WRAPPER: `pwsh tools/dev_flash.ps1` does the whole dance in one
shot -- hook cooldown, bridge kill, `esp-harness cycle` (with PYTHONUTF8=1),
cooldown removal, bridge restart (`-NoBridge` to skip the restart). Prefer
it over hand-rolling the steps below.

Background (what the wrapper automates): `idf.py flash` needs exclusive
COM9, but stopping the bridge makes the next tool-call hook auto-start a
new one that grabs the port mid-flash (esptool then fails to connect).
The wrapper writes a future `start_ts` into
`$TEMP/claude_buddy_hook_state.json` (every hook goes into cooldown so
none spawns), kills the bridge, flashes, then deletes the state file and
restarts the bridge. `export CLAUDE_BUDDY_AUTOSTART=0` also works for a
shell that owns the hooks.

Toolchain notes: `esp-harness` is an editable install from
`D:/Code/esp-harness/tools/esp-harness` (source edits take effect
immediately); its subprocess captures pass `encoding="utf-8"` so zh-CN
GBK never breaks cycle output again. PYTHONUTF8=1 is set in
`.claude/settings.json` for Claude sessions as belt-and-braces.

### Weather (v4.9)
scene_weather = weather+clock combo, third BOOT-cycle stop (dashboard →
overview → weather; clock stays saver/lock-only). Data flows host→device:
the bridge's WeatherPoller fetches yesterday+today+3 days and pushes
`dash weather` (compact JSON, WMO codes) on fetch/reconnect — weather has
NO keepalive, so reconnect re-push matters. Providers: xiaomi (domestic
route, default-first; weathercn→WMO map in bridge) and open-meteo
(international egress is BLACKHOLED on this network — measured, don't
retry). Region hardcoded 深圳福田; override via `[weather]` in
`~/.claude-buddy/config.toml` (lat/lon/name/location_key/provider) or
--weather-* flags. On-device: quarter-hour minutes (:00/:15/:30/:45,
sec<30) swell the clock for 30 s via the plan-B zero-scale morph; fade
transitions use authoritative per-object base opacities encoded in
user_data (wx_mark) — NEVER snapshot current opacities as fade bases (0
is an absorbing state; it bricked labels once). Icon rebuilds must stay
outside transition windows (trans_until_ms gate) — the fade table holds
raw pointers. The big illustration animates via the accent system:
tick-stepped opa waveforms (16 steps/3 s) with per-line phase offsets —
BREATH (rays/rain/snow/fog/back-cloud) and FLASH (lightning burst).
Never add per-frame position/size anims to it. In weather-major the
small clock sits TOP-RIGHT (scene-local re-align of its own status_bar
time_lbl — other scenes keep TOP_MID). Test quarter-hour by pushing
`dash time` with a crafted epoch; verify with --size 320 screenshots.

### Scene transitions (v5.0 — main/scene_trans.c)
ALL scene switches go through scene_trans_switch() (never call
scene_fw_show directly): exit → black-frame instant switch → enter.
Actors (per-scene element lists) spring in from OFF-SCREEN
(anim/spring.c — real damped-harmonic-oscillator LUTs: spring_disp
ζ=0.68 overshoots ~5.5%, spring_opa ζ=0.92 never overshoots) and
reverse out (last-in-first-out, apple_ease_in). The instant switch on
an all-black frame is what killed the old "scanline" feel — the
framework's full-container widget-opa crossfade double-composited two
466px layers per frame (scene_fw_show_instant was added to
esp-harness-core for this). THE TIME IS A FIXED ANCHOR: it never
leaves the screen across transitions, and it must be a TRUE morph of
one entity (user contract — no small/big cross-fade sleight of hand).
Consensus pose = top-center 48px small clock. Size changes step
through FONT RUNGS (clock face 48/66/84/102/120/135, weather corner
clock 26/33/40/48 — each rung one native tiny_ttf rasterisation;
transform_scale stays banned) while position glides continuously on
spring_disp. CLOCK_CACHE_MAX in cjk_font.c must hold every rung (12).
Switching is async (~0.9 s); rapid re-targeting is coalesced. Scenes
without a bound profile degrade to instant black-frame cuts — migrate
by binding actors in init(). Notes: a stray "clock locked" toast after
reflash is a leftover AXP2101 PWR IRQ polled on boot (pre-existing);
`?dump` on the clock scene with SYNCED time can reboot the device if
the minute rolls over (two 135px glyph re-rasters mid-dump rides the
TWDT) — screenshot the clock early in the minute, or while "--:--".

### Your-turn pull (v6.0, replaces the v4.9 dismissable takeover)
scene_awaiting is retired -- it and the dashboard gold pose were two
near-identical gold pages a key press flipped between (user call).
scene_auto_switch_cb now PULLS the display to the dashboard on the
rising edge of (any_awaiting && !host_lost), one-shot: BOOT/PWR
afterwards behave exactly like on any scene (no dismiss API, no
pre-takeover restore -- after a round clears the display stays where
it is). Edge-on-effective-awaiting preserves both old suppressions:
host_lost drops the signal, the reconnect force-push raises it again.

Device font is a GB2312 SimHei subset (`main/zh.ttf`). The bridge gates all
device-bound text through its actual cmap (`_device_safe`), mapping
unrenderable chars (fullwidth punct, ✓→v, emoji) to ASCII or dropping them, so
".notdef" boxes can't appear. To render a NEW glyph on-device you must add it to
the font subset (`tools/make_cjk_font.py`) AND reflash — host mapping only
downgrades.

### Build/flash toolchain (this machine)
ESP-IDF v6.0.1 via EIM. Activate in PowerShell before `idf.py`:
`. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'`
