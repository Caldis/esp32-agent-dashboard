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
- `esp-harness screenshot --size 480` -- full-panel capture, 1:1 since
  v7.3 (the panel IS 480x480 — see "Panel geometry" below; `--size 466`
  silently downscales the framebuffer and is what the repo used to
  believe was "full panel"). Safe since v6.4; do NOT settle for 320, it
  hides glyph/alignment bugs.
- `esp-harness verify` -- screenshot + REAL golden diff (v6.4). Creates
  `.harness/golden/<scene>.png` on first run, then fails with exit 50
  and writes a *-diff.png when mean abs channel diff exceeds
  --tolerance (default 1.5, absorbs clock-digit churn). Before v6.4
  it captured a 128px image and unconditionally printed "pass".
- `esp-harness console --cmd "?stat" --json` -- device health. `fps` here
  is REAL since v6.4 (LV_EVENT_RENDER_READY); before that it was a 33 ms
  timer counting itself and reported 30 no matter what.
- `esp-harness console --cmd "?perf" --json` -- render vs flush-wait vs
  dirty-pixel timing, windowed (read-and-reset). Read `frame_ms`, not
  `fps_win`: the window average gets diluted by any idle tail. Companion
  A/B switches: `?bake 0|1` (transition sprite baking), `?refr <ms>`
  (idle-tier refresh period).

## Key Files
- `harness.json` -- project config (board=esp32_s3_touch_amoled_2_16, port=COM9, modules)
- `main/esp32_agent_dashboard_main.c` -- entry point
- `main/scenes/` -- UI scenes (dashboard, weather, clock). Retired,
  sources moved to `attic/` in v6.4 (see attic/README.md) --
  overview + prompt (v5.2; `dash idle` aliases to
  dashboard, `dash prompt` is a no-op ACK; prompt_active must stay
  false forever -- see agent_snapshot_apply.c) and awaiting (v6.0 --
  see below). v6.6: three keys, three views, direct. The physical
  left-to-right order is BOOT, PWR, USER — NOT the naming order — so the
  mapping is BOOT=dashboard, PWR=clock, USER=weather (device-verified
  v7.0 via `dash btn` + scene_trans logs; an earlier revision of this
  file had PWR/USER swapped), and nav_dots at
  25/50/75% of the width mirror the physical keys. Pressing the key for the view you are
  already on flashes a border highlight (scene_flash) instead of
  switching. Replaces the v4 multi-modal scheme (BOOT cycling,
  PWR clock-lock toggle, USER focus cycling); USER's focus cycling
  has no physical key any more. v6.0: the dashboard
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
  ui_type.c. Sized for 0.6-1 m viewing distance on the 2.16" 480x480
  panel -- do NOT introduce ad-hoc pixel sizes or anything
  below CAPTION.
- `tools/claude_buddy_bridge.py` -- host bridge daemon
- `tools/hook_dispatch.py` -- Claude Code hook forwarder

## Panel geometry (v7.3 — MEASURED, replaces a long-standing myth)
The visible area is the WHOLE LVGL space: **480x480, origin (0,0), corner
radius 76**. Edge-hugging elements use `UI_LV_W` directly — no origin
compensation, no safety inset.

ui_screen.h used to assert "LVGL is 480 wide, the panel lights only 466 of
it, origin `a` unknown (bracketed to [1,6])". Every part of that was
wrong; 466 is a spec number nobody had ever checked against LVGL
coordinates (BSP_LCD_H_RES has said 480 all along). It cost two visible
defects: edge elements anchored to a 466 box sat 3 px in on the left/top
but 17 px in on the right/bottom (the "gap on the right and bottom"), and
corner arcs drawn at radius 60 fell inside the bezel's much rounder 76.

Measured with `?vis` (ui_calib.c), which is kept for the next panel batch:
  ?vis 1/2/4/5  edge rulers — bars at 0/3/7/12 px inward on ALL four
                edges were ALL visible, including row/column 0 and 479.
  ?vis 3        corner arcs, one candidate radius per corner. A candidate
                SMALLER than the true radius falls in the region the bezel
                cuts away → invisible; larger → visible with a gap. So the
                smallest complete arc is the answer. Four halvings:
                (60,78] → (72,76] → 76.
Screenshots cannot answer any of this: `?dump` renders the whole 480x480
framebuffer SCALED to the requested size, not a visible-area crop (proven:
LVGL column 465 lands at column 451 of a 466-wide capture = 465*466/480).
Only a human looking at the panel can read these rulers.

Still on 466 and knowingly deferred: layout constants (`SCREEN_W 466` in
the scenes, nav_dots' 25/50/75% positions). They centre content on 233
instead of 240, i.e. the whole UI sits ~7 px left of true centre. Fixing
that needs a visual re-tune because hand-placed absolute coordinates
(illustration x=36, strip DX, …) are interlocked with the centred ones.

## Rendering performance (hard-won, do not regress)
- (v7.3) Three reusable facts, found while building the dispersion glow:
  · LVGL's rounded-corner AA coverage cache is keyed on RADIUS ALONE and
    defaults to 4 entries (`lv_draw_sw_mask.c`). Keep animated geometry
    OFF the radius — translation and box-size changes do not change the
    key, so they stay cached; animating the radius itself recomputes
    every corner mask every frame (glow: 37.7 → 31.6 ms just from making
    the radius a per-ring constant). Raising the cache to 16 to "fix"
    the same problem is a MEASURED DEAD END (no effect on transitions or
    on the glow — ledger in sdkconfig.defaults). Stabilise the key, don't
    enlarge the table.
  · `lv_obj_clear_flag(LV_OBJ_FLAG_HIDDEN)` invalidates the WHOLE object.
    For a full-screen overlay that is a full-screen repaint on every
    show (measured `inval_max_px` 230400, render_max ≈48 ms). Keep such
    overlays always-visible and converge them to opa 0 instead.
  · The `lv_obj_enable_style_refresh(false)` batching trick works ONLY
    for properties with no LAYOUT flag (opa is fine). Position/size are
    skipped entirely — refresh_style returns early and the layout is
    never marked dirty, so the geometry change silently does nothing.
    Animated geometry on a screen-sized element therefore needs a custom
    `LV_EVENT_DRAW_MAIN` painter (one object, N shapes, hand-invalidated
    bands), which is what ui_glow.c became.
- (v7.1/v7.2) Weather PRE-COMPOSITING, default on (`?wxcomp 0|1` A/B):
  vector content lives on off-screen stages (parked outside the parent
  clip) and is baked AT REST via lv_snapshot into lv_images
  (wx_compose) — transition frames blit bitmaps instead of
  re-rasterising ~90 vector objects. v7.2 form: OPAQUE RGB565 with the
  scene background baked in (2 B/px, no-alpha copy path), and the
  whole 5-day strip (15 labels + 5 icons) moves house to a strip stage
  (strip_set_home) and bakes as ONE 466×160 bitmap. Paired A/B vs live
  vectors: c→w render 40.0→29.3 ms (−27%), d→w −30%, w→c −19%; idle
  breath repaint 8.6→4.1 ms (−53%); drawn +3~5/transition. Key facts:
  the effective background under scenes is the scene root's OPAQUE
  BLACK (scene_framework.c paints it for the black-frame switch) — NOT
  theme->bg, which never shows through; wx_effective_bg walks up to
  the first covering ancestor instead of hardcoding either. An opaque
  bake with stage ext_draw != 0 would show a black ring (buffer
  cleared, not bg) — wx_compose guards and reverts loudly. ARGB8565 is
  snapshot-only on LVGL 9.4 and esp_lvgl_port's S3 SIMD asm covers
  only plain fill + no-alpha RGB565 copy (~3% fill share here) — both
  surveyed, do not retry. Content-identity caches (big_code,
  strip_sig) skip recomposition when content is unchanged — on_show
  force-redraws would otherwise pay synchronous off-screen renders for
  nothing; theme switches invalidate identities (bg is baked in).
  Compose failure reverts to live vectors loudly (WARN). Bench
  methodology (glow settle 15 s, pre-warm after arm switch, arm-order
  counterbalancing, row-1 flash pollution, duplicate-bridge / USB-reset
  environment artifacts): docs/PERF_TRANSITIONS.md.
- (v7.0) LVGL's anim timer runs at compile-time LV_DEF_REFR_PERIOD
  (33 ms) and does NOT follow the display refr timer — raising the
  refresh tier alone leaves motion sampled at 30 Hz with half the
  refresh cycles finding nothing dirty. ui_motion's apply_period now
  sets `lv_anim_get_timer()` to the same period as the display. Side
  effect measured: at 16 ms steps the per-frame displacement halves, so
  dirty unions shrink — weather-transition render_avg fell 34→24-26 ms
  from this change alone, and drawn frames per transition rose 15-35%.
- (v7.0) The v6.5 bake panic is root-caused and fixed: ghost_begin's
  lv_snapshot_take (a full SW render) ran on the CALLER's task —
  physical keys = 3072-3584 B stacks, stack depth varies with weather
  icon content. Slow presses crash round 1; rapid presses never crash
  because mid-transition retargets run outro/intro in step_cb on the
  LVGL task. button_router now defers the switch via lv_async_call
  (queue under bsp_display_lock; the callback must NOT re-lock), so
  every trigger path shares the LVGL-task stack. `dash btn` and real
  keys are now the same class again. Bake itself STAYS DEFAULT OFF: at
  16 ms sampling its A/B is ±2 ms (the old 17% figure was measured
  under 33 ms sampling); see the ledger in scene_trans.c.
- (v7.0) status_bar_update no longer calls lv_label_set_text
  unconditionally per tick (LVGL never compares text — same-text sets
  still realloc + invalidate); it compares first. Scene ticks are
  500 ms so this is idle hygiene, not a transition lever.
- (v7.0) Transition baseline + regression gate:
  `& ./tools/with_port.ps1 { python tools/perf/trans_bench.py --compare baseline }`
  — exit 1 on >15% frame_ms/render regression, exit 2 on invariant
  break (mid-run reboot / `?ghost` mismatch / held-count drift /
  drawn=0). Numbers + methodology + next-lever ranking:
  docs/PERF_TRANSITIONS.md. Caveat: weather-pair rows vary ±20% with
  the LIVE weather's icon complexity — A/B two arms back-to-back,
  don't compare across weather changes.
- MEASURE FIRST, with `?perf`. Three optimisations that were "obviously"
  right made things WORSE on this board and are recorded as dead ends in
  place (sdkconfig.defaults, scene_trans.c): a second SW draw unit
  (render 21.7->30.8 ms and a reboot), transition-window full-screen
  coalescing (14.7->29.7 ms), and sprite-baking the dashboard's ambient
  cluster (21.8->26.6 ms). The recurring lesson:
  **render cost tracks the CONTENT that must be regenerated, not the
  dirty area.** Batching the weather breath's 12 invalidations into one
  cut dirty pixels 4.3x (115k -> 27k) and moved render time by 6%.
- `trans_actor_t.bake` (transition sprite baking) pays ONLY when an
  actor's content is expensive relative to its area. Measured: weather's
  actors (30 antialiased lines + 15 labels) 38.5 -> 32.0 ms/frame, so
  they bake; the dashboard's ambient cluster is a 466x224 container
  holding one 96 px ring and two short text lines -- mostly empty pixels
  -- and baking it cost 23%, so it deliberately does not.
- Sprite ghosts must cover EXACTLY obj-expanded-by-ext_draw. The snapshot
  buffer is `obj + 2*ext` and its content origin is `obj.coords.x1 - ext`
  (lv_snapshot.c), but a ghost placed by copying align + style x/y follows
  a different rule: under TOP_MID the extra 2*ext width splits evenly so x
  lands right by luck, while TOP_* never compensates height, so y is off by
  a whole ext. Symptom: elements JUMP the instant a transition hands back
  to the real object, and only aligned labels do it (set_pos containers are
  immune) — measured ext=10, 16 of 28 ghosts misplaced. Fixed by measuring
  the delta after placement and folding it into style x/y, with the offset
  kept as the ghost<->object coordinate conversion used by the animation
  and the handback. `?ghost` reports baked/mismatched counts; it must stay
  0. Derive ext from the buffer (`buf->header.w - obj_w) / 2`), not
  lv_obj_get_ext_draw_size — that one lives in a private header.
- The golden diff CANNOT catch this class of bug: it only ever sees
  resting frames, and the defect exists only during the handoff frame.
  Transient invariants need runtime self-checks (`?ghost`), not
  screenshots. And log-only diagnostics are not enough — every console
  invocation opens a fresh serial session, so anything logged from a
  timer callback (i.e. all of intro/outro) is gone before the next
  command reads. Make it queryable.
- Capturing goldens with the bridge stopped: set `offline_clock_min` to 0
  first and restore it after. Otherwise the device retreats to the clock
  scene mid-capture (host silent >5 min) and you baseline the wrong scene
  under the right name.
- A snapshot colour format must be in BOTH whitelists: the switch in
  `lv_snapshot.c` AND `CONFIG_LV_DRAW_SW_SUPPORT_*`. RGB565A8 is only in
  the second, so `lv_snapshot_take` returns NULL for it -- which made
  sprite baking silently do NOTHING for a whole release while an A/B
  "measured" a 5-15% win that was pure drift. ARGB8888 is in both. If a
  feature has a silent fallback, log it at WARN.
- NEVER per-frame animate size/transform_scale/widget-opa on big tiny_ttf
  labels or containers overlapping them (marquee freeze, clock plan A,
  overview ring breath, prompt pulse -- all bisected to this). Use
  low-frequency stepped styles from the scene tick instead.
- (FIXED in v6.4 — was: full-size `--size 466` rides the TWDT edge and
  reboots on extra render load, so verify at `--size 320`.) The `?dump`
  emit loop now yields every 200 ms, so full-panel capture is safe on
  every scene: 5 consecutive 466 dumps across dashboard/weather/clock
  with no reboot, ~4.7 s each. **Verify at 466** — 320 downsampling hides
  exactly the class of bug you are looking for (it hid the clipped-glyph
  font bug, and it renders 今天/明天 as illegible mush).

### Keys and the border highlight (v6.6)
- Decide against `scene_trans_target()`, NEVER `scene_fw_current_index()`.
  Transitions are async: the current index still reports the OLD scene
  until the black frame ~370 ms in, so a second press inside that window
  reasons about a stale present. The retired PWR toggle died exactly
  there — it re-entered its "I'm on the clock" branch, found the
  remembered previous scene already consumed to -1, and fell back to
  index 0, which is what "PWR should return me to weather but sometimes
  jumps to the dashboard" actually was.
- scene_flash's ring lives on lv_layer_top() and its bounding box is the
  WHOLE screen, so a plain border_opa change repaints every scene object
  underneath: measured 53.4 ms/frame (19 fps), 7 overruns per flash. Fix
  is the same batching trick the weather breath uses — suppress the
  automatic invalidate and hand-invalidate the four edge bands instead.
  18.5 ms/frame (53 fps) after.
- A short motion window needs the fast refresh tier just like a
  transition does: at the 66 ms idle period a 33 ms envelope step is
  faster than the display refreshes and frames are simply dropped. The
  flash raises the period for its duration and hands it back — but only
  if `!scene_trans_busy()`, so it never steals the tier from a running
  transition.

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

For ANYTHING ELSE that needs the port (screenshots, `?perf` sampling,
console sweeps), use `& ./tools/with_port.ps1 { ...block... }` -- same
dance, wraps an arbitrary script block, and restores the bridge even if
the block throws. Call it with `&` from an existing pwsh session, NOT as
`pwsh tools/with_port.ps1 {...}` (the CLI mangles the script block).
This matters more than it looks: a hook firing mid-run auto-starts a
bridge that grabs COM9 and kills the measurement -- it destroyed three
separate `?perf` sampling runs during the v6.3 work before this existed.

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
by binding actors in init().

### Shared elements (v6.2 — the continuity layer)
The time anchor was the first "element that survives a transition", but
it needs morph callbacks because the two poses differ. The common case
is two scenes whose element is IDENTICAL — dashboard and clock share the
same status_bar footer, same coordinates. Such an element must NOT fly
out and fly back; it waits in place (user call).
Mechanism: `trans_actor_t.key` is a cross-scene identity. Before every
outro AND intro the framework intersects the (from, to) actor tables:
same key **and** fully equal pose (dir / opa channel / base_opa / align /
BOTH axes, neither HIDDEN) ⇒ both sides mark the actor `held` and just
pin it at rest. On the black frame the two objects coincide pixel for
pixel — same principle as the time anchor, no callback needed. Key
mismatch or pose drift degrades silently to a normal in/out, so
`scene_trans` logs `outro <scene>: N/M held` per switch — that count is
the only way to notice drift. Expected today: dashboard↔clock 4 held
(footer), anything↔weather 0 (weather cuts the footer layer entirely and
declares no footer actors).
The footer's actor definition lives in ONE place —
`status_bar_trans_actors()` — because pose equality is what the matcher
tests; a hand-copied second definition would drift and silently bring
the fly-out back.
Dynamic rest poses: `trans_profile_t.sync_rest` is called before outro
and before intro so a scene can write the CURRENT resting position into
`actor->rest_pos` (dashboard's ambient cluster slides between its
chip/no-chip poses; fleet card y depends on row count). Corollary:
`ambient_slide_to()` must no-op while `scene_trans_busy()` — otherwise
two animations fight over the same y.
Actor design rules learned here: group many small objects into ONE
container actor (weather's 5-day strip = 15 objects, the decorative
stars = 6 lines) — 15 parallel position anims dirty nearly the whole
screen, one container is a single union repaint. And on weather use
displacement only (TROPA_NONE): every content object there is already
driven by the wx_mark fade table, and a second writer on opa can lose a
race into the absorbing 0 state. Notes: a stray "clock locked" toast after
reflash is a leftover AXP2101 PWR IRQ polled on boot (pre-existing);
(the old "`?dump` on the clock scene with SYNCED time can reboot the
device if the minute rolls over" hazard is FIXED in v6.4 — the dump emit
loop yields; verified with repeated 466 captures on the clock.)

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
