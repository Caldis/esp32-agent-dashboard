# Scaling design — 8-agent rendering on a 466 × 466 round AMOLED

Companion to [`PERFORMANCE.md`](PERFORMANCE.md). That doc quantifies
*where* the dashboard is today. This doc designs *how* the dashboard
draws 1, 2, 3-4, and 5-8 agents on the same physical screen without
losing legibility, without losing identity (which agent is which),
and within the ~84 KB idle heap budget.

Owner: **F2** (firmware engineer) implements; **PERF1** specifies +
benchmarks. Sibling agents leave `main/scenes/scene_dashboard.c`,
`main/agent_state.h`, and `main/theme.{h,c}` to F2.

## 0. The constraint

ESP32-S3-Touch-LCD-1.85C — round panel, 466 × 466 pixels, ~1.85"
diagonal, single rotary encoder, three side buttons. The user
glances at it. **8 agents is not "look at 8 things at once"** —
8 agents is "I know all 8 sessions are alive and which one needs me
right now". The grid is a status lattice, not a wall of TVs.

The roadmap target (v0.7.0):

- `AGENT_SLOT_MAX = 8` (today 4)
- `scene_dashboard` adapts grid 1 → 2 → 4 → 8
- still hits the 33 fps redraw envelope at 8 agents
- still holds ≥ 40 KB free heap at 8-agent steady state

## 1. Adaptive grid

The grid switches mode based on `slot_count` (number of `in_use`
slots) computed once per scene tick.

```
┌──────────────────────────┐   ┌────────────┬─────────────┐
│                          │   │            │             │
│                          │   │            │             │
│      [single card]       │   │   [left]   │   [right]   │
│         260 × 200        │   │   half     │    half     │
│                          │   │            │             │
│                          │   │            │             │
└──────────────────────────┘   └────────────┴─────────────┘
        slot_count == 1                slot_count == 2

┌────────────┬─────────────┐   ┌────┬────┬────┬────┐
│            │             │   │ 0  │ 1  │ 2  │ 3  │   row 0
│   [0]      │    [1]      │   ├────┼────┼────┼────┤
├────────────┼─────────────┤   │ 4  │ 5  │ 6  │ 7  │   row 1
│   [2]      │    [3]      │   └────┴────┴────┴────┘
│            │             │
└────────────┴─────────────┘
   slot_count ∈ {3, 4}         slot_count ∈ {5, 6, 7, 8}
```

### 1.1 Per-mode dimensions (target rectangles inside the 466² circle)

The round AMOLED clips the corners. Effective drawable rect is
inset by ~10 px so type doesn't fall off the bezel.

| Mode  | Card size (W × H) | Margins (h, v) | Lines per card |
|---:---|---:|---:|---:|
| 1     | 320 × 220 (full)  | 73, 70  | 5 entries (header + 4) |
| 2     | 218 × 240 each    | 10, 60  | 4 entries |
| 3-4   | 218 × 130 each    | 10, 30  | 3 entries |
| 5-8   | 108 × 100 each    |  6, 30  | 1 entry (msg only, no transcript) |

The "lines per card" column is what the **per-agent transcript
window scales down to**, as required by §1.3.

### 1.2 Layouter shape (proposal for F2)

```c
typedef struct {
    int card_w, card_h;     /* per-card pixel size */
    int origin_x, origin_y; /* top-left of the grid */
    int gap_x, gap_y;       /* gap between cards */
    int cols, rows;
    int entries_per_card;   /* how many transcript lines to paint */
    bool show_tokens;       /* hide on mode 5-8 (no room) */
    bool show_spark;        /* aggregate sparkline only on mode 1 / 2 */
} dash_layout_t;

static dash_layout_t layout_for(int slot_count)
{
    if (slot_count <= 1) return (dash_layout_t){ 320, 220, 73, 90, 0, 0, 1, 1, 5, true,  true  };
    if (slot_count == 2) return (dash_layout_t){ 218, 240, 10, 90, 10, 0, 2, 1, 4, true,  true  };
    if (slot_count <= 4) return (dash_layout_t){ 218, 130, 10, 90, 10, 8, 2, 2, 3, true,  false };
    return                       (dash_layout_t){ 108, 100,  6, 90,  6, 8, 4, 2, 1, false, false };
}
```

The layouter is pure; it does not touch LVGL. `dash_tick()` picks a
layout, then iterates `for i in [0, slot_count)` and paints each card
at `(origin_x + col * (card_w + gap_x), origin_y + row * (card_h + gap_y))`.

### 1.3 Per-card content by mode

| Mode | Header | Status dot | Message body | Transcript | Tokens | Sparkline |
|---|---|---|---|---|---|---|
| 1   | kind + sid | yes | full `msg` | 5 entries | yes | yes (full width) |
| 2   | kind       | yes | full `msg` | 4 entries | yes | yes (right card only) |
| 3-4 | kind       | yes | first 36ch | 3 entries | yes | no |
| 5-8 | kind icon  | yes | first 12ch | none       | no  | no |

Mode 5-8's "kind icon" is a 12 × 12 colored circle in the accent of
that agent kind — see §2 — plus the kind initial letter (`C` for
claude-code, `X` for codex, the first letter of `kind[]` for others).

### 1.4 Rotation / zoom for the 5-8 case

8 thumbs at 108 × 100 each fit the screen but the message is at most
12 chars. The user will want to *focus* one. Two options, in order of
how cheap they are:

1. **Encoder rotates the focus ring.** A single ~2-px-thick accent
   ring highlights the currently-focused thumb; rotating moves it
   through 0 → 7 → 0. **Encoder press** zooms: the focused thumb
   expands to the centre at 218 × 200 size with the full mode-2
   content; the other thumbs ghost out to 30 % opacity in their
   slots. Press again to un-zoom. Implementation: one extra
   `focus_idx` field on `dash_state_t`, painted in the layouter.
2. **Auto-rotation when nothing is focused.** Every 5 s, cycle the
   "zoomed" thumb among the slots that currently have
   `status == AGENT_STATUS_RUNNING`. This is the "ambient" mode.
   Disabled the moment the user touches the encoder.

PERF1's recommendation: ship (1) in v0.7.0; (2) is a v0.8 ambient
polish. The roadmap promise is the *grid*, not the rotation, and
adding focus-ring + zoom is one new state machine inside
`scene_dashboard.c`.

## 2. Accent allocator

`theme.c` currently maps three named accents to three named kinds:

```c
uint32_t theme_accent_for_kind(const char *kind)
{
    if (strcmp(kind, "claude-code") == 0) return s_current->accent_claude;
    if (strcmp(kind, "codex")       == 0) return s_current->accent_codex;
    return s_current->accent_other;       /* fallback for everything else */
}
```

`s_current->accent_other` is **one colour**. With v0.5.0 expanding to
Cursor + Aider + qwen-code, *and* v0.7.0 allowing 8 concurrent agents,
this fallback collapses every "other" kind to the same hue. Two
codex sessions and two aider sessions in the 5-8 grid look identical.

### 2.1 Golden-angle generator

Allocate fallback accents from a deterministic generator keyed on the
agent kind string:

```c
/* Golden angle in HSV space. ~137.508° between successive samples
 * gives maximum visual separation. */
uint32_t theme_accent_generate(const char *kind)
{
    /* Hash kind → seed bucket index in [0, 12) so the same kind name
     * always lands on the same hue across reboots. */
    uint32_t h = 5381;
    for (const char *p = kind; *p; ++p) h = h * 33 + (uint32_t)*p;
    uint32_t bucket = h % 12;

    /* Walk 12 hues around the wheel at the golden angle. Convert to
     * RGB at a fixed saturation/value that fits the current theme's
     * intensity. */
    float hue_deg = fmodf(bucket * 137.508f, 360.0f);
    /* hsv2rgb(hue_deg, 0.70, 0.85) → 24-bit hex */
    return hsv_to_hex(hue_deg, 0.70f, 0.85f);
}
```

### 2.2 Integration with the existing palette

The named-kinds (`claude-code`, `codex`) keep their hand-picked
brand colours from `palette.md` — they are *known* identities,
deserving curated hues. Everything else falls through to the
generator:

```c
uint32_t theme_accent_for_kind(const char *kind)
{
    if (!kind || !kind[0]) return s_current->accent_other;
    if (strcmp(kind, "claude-code") == 0) return s_current->accent_claude;
    if (strcmp(kind, "codex")       == 0) return s_current->accent_codex;
    /* New: deterministic generator for unknown kinds. */
    return theme_accent_generate(kind);
}
```

### 2.3 Theme-aware modulation (noir vs lab vs mono)

The generator's output is one hue + saturation. The three themes
modulate differently:

- **noir** (dark bg): high value (0.85), high saturation (0.70) —
  generator output passes through unchanged. Works as designed.
- **lab** (light bg): the bright accents wash out on paper. Multiply
  value by 0.6 — bring it down to 0.51 so it sits on light without
  glare.
- **mono** (one hue): the generator is **bypassed**; every kind
  collapses back to `accent_other` because mono is a deliberate
  flatten of identity. The 5-8 grid is still distinguishable by
  position (slot order), not colour.

The bypass is a one-liner at the top of `theme_accent_generate`:
`if (s_current->id == THEME_MONO) return s_current->accent_other;`.

### 2.4 Determinism across reboots

The generator hashes the kind string; same input → same output.
Same agent kind keeps the same colour across reboots and across
devices, *modulo* the theme. This is critical: a user looking at
their device should see the same colour for "aider" today as last
week.

## 3. Per-agent transcript window scales down

Quoted from §1.3:

| slot_count | entries shown per card |
|---:|---:|
| 1 | 5 |
| 2 | 4 |
| 3-4 | 3 |
| 5-8 | 1 (just `msg`, no historic entries) |

**The on-device storage stays at `AGENT_ENTRY_COUNT` entries per
slot.** What scales is the *rendering*: scenes paint
`min(layout.entries_per_card, slot->entry_count)` entries. The
storage cap can drop independently (see PERFORMANCE.md §3.3
recommendation: 5 → 3 in storage saves ~2 KB at 8 slots), but the
"display K of N" mechanism is independent of the storage cap.

Why we don't drop storage to 1: `scene_sessions` shows the full
transcript regardless of grid mode, and the user will rotate the
encoder to switch from `scene_dashboard` (the 8-agent grid) to
`scene_sessions` (focused detail on one agent). The transcript
needs to survive the dashboard tick.

## 4. Heap budget

From PERFORMANCE.md §2.4: each `agent_slot_t` is ~1 040 B. Today's
budget at AGENT_SLOT_MAX=4:

```
agent_state_t          ≈   4 410 B  (4 slots)
dash_state_t (scene)   ≈   1 200 B  (2 cards, sparkline buffer, labels)
sessions_state_t       ≈   2 800 B  (one card + 5 entries × ~256 B for labels)
prompt_state_t         ≈     800 B
LVGL overhead          ≈  20 000 B  (object tree, draw buffers)
                       ─────────
                       ≈  29 000 B  of "scene-side"

Free heap (idle)       ≈  84 000 B
Total internal SRAM    ≈ ~330 000 B (after IDF)
```

At AGENT_SLOT_MAX=8 with the PERFORMANCE.md §3 fixes applied:

```
agent_state_t          ≈   5 220 B  (8 × 124*3 entries + the rest)
                                   ↑ entries 5→3 saves ~ 2 KB net
dash_state_t (scene)   ≈   1 500 B  (8 thumb cards = 8 × small,
                                     sparkline buffer collapses to 32)
                       ─────────
                       Δ ≈ +2 100 B vs today
```

**Net heap impact of going to 8 agents with the proposed fixes: roughly
+2 KB.** That leaves the idle heap at ~82 KB, comfortably above the
40 KB v0.7.0 floor. Without the fixes (naive 8 slots × 5 entries +
doubled sparkline buffer), the cost would be ~+6 KB — still under
the budget, but eats into headroom we'll want for v0.8.0+ scenes.

### 4.1 PSRAM is sitting there

The board has 8 MB octal PSRAM. Today nothing in `main/` uses it.
LVGL's draw buffers, the agent_state, and the scene state structs
all live in internal SRAM. **At AGENT_SLOT_MAX=8 we don't need to
spill into PSRAM**, but if v0.9.0 (observability) adds a 30 KB ring
buffer of recent events, or v1.5.0 adds on-device replay, PSRAM is
the obvious tier-2 home. The `tools/perf/bench_firmware.py` script
already logs `heap_min` (the all-time low watermark) so we'll see
the spill point coming.

## 5. Scene tick budget

Today: `scene_dashboard.c` ticks at 250 ms with two cards. The hot
work is:

```
agent_state_lock           ~ 1 µs
memcpy(slots[4])           ~ 4 µs  (4 × 1040 B at SRAM bandwidth)
agent_state_unlock         ~ 1 µs
paint_card × 2             ~ 200 µs  (LVGL property mutations + strcmp)
render_sparkline           ~ 50 µs
                          ──────
                          ~ 260 µs of CPU per tick = 1 ms/s of LVGL time
```

At 8 slots:

```
memcpy(slots[8])           ~ 8 µs   (still trivial; CPU is fast at memcpy)
paint_card × 8             ~ 200 µs (mode 5-8 cards are SIMPLER —
                                     no tokens, no transcript, no spark.
                                     Per-card cost actually drops.)
                          ──────
                          ~ 230 µs of CPU per tick
```

The 8-agent path is **cheaper per tick** than today's 2-card path
because each thumb card has less content. The "8 agents = slower"
intuition is wrong; the cost scales with rendered information, not
with slot count.

## 6. Implementation checklist (for F2)

Suggested commit order for v0.7.0:

1. `main/agent_state.h`: `AGENT_SLOT_MAX 4 → 8`, `AGENT_ENTRY_COUNT
   5 → 3`. Rebuild. `tools/perf/bench_bridge.py compare` confirms
   host-side untouched. `tools/stress.py --all` confirms wire intact.
2. `main/theme.{h,c}`: add `theme_accent_generate()` + the
   `theme_accent_for_kind` rewrite. Unit test by reading the
   colour for a few synthetic kind strings; verify same input →
   same output.
3. `main/scenes/scene_dashboard.c`: extract `layout_for(slot_count)`,
   refactor `dash_tick()` to iterate any number of cards. Start with
   mode 1 and mode 2 working; add 3-4; add 5-8.
4. `main/scenes/scene_dashboard.c`: add `focus_idx` for the
   encoder-rotation focus ring (mode 5-8 only).
5. `main/agent_state.c`: switch `entries[]` to a head-indexed ring
   (PERFORMANCE.md §3.6). Same external API, the only caller is
   `agent_state_push_entry()`.
6. `main/scenes/scene_dashboard.c`: collapse `dash_state_t.spark_pts`
   to `AGENT_SPARK_SAMPLES` (PERFORMANCE.md §3.4).
7. `main/harness/agent_commands.c`: add `dash scene <id>` verb so
   `profile_scene.py` can deterministically switch scenes. Closes
   HARNESS_GAPS G-PERF1.
8. `tools/mock_device_v1.py`: vary `dash health` reply by scene
   (closes HARNESS_GAPS G-PERF2). Optional: gate behind
   `--health-vary` so existing stress tests stay deterministic.

After step 7, re-run `tools/perf/bench_firmware.py` against a real
device (or the enriched mock) and update PERFORMANCE.md §1.3 and
§1.4 with non-`N/A` numbers.

## 7. Implementation note

**PERF1 does NOT modify any source file** in `main/` or `tools/`
outside `tools/perf/`. Everything in this document is a spec for F2.
The scaling design has been deliberately calibrated to be a series of
small, independently-revertible changes — F2 can land step 1 alone
and the firmware boots, the bench scripts still work, the host
bridge still works. Each step incrementally improves the numbers
measured by `tools/perf/`. None of them is a "big rewrite" risk.

## 8. Open questions for the orchestrator

- **Touch.** The 1.85" panel is touch-capable. Should the 5-8 mode's
  focus ring also respond to touch (tap a thumb to focus it)? This
  is a UX call, not a perf call. Default proposal: no — the encoder
  is the canonical input; touch is reserved for v0.8.0+ "expert mode".
- **`scene_dashboard` vs new `scene_grid8`?** Should the 8-agent
  adaptive layout live in `scene_dashboard.c`, or split into a new
  scene? PERF1's vote: keep it in `scene_dashboard.c`. The whole
  point of an *adaptive* grid is that the user doesn't switch
  scenes — the grid adapts under them. Two scenes for "few agents"
  vs "many agents" would re-introduce the friction we're removing.
- **What's the upper bound?** The roadmap says 8. Could we go to 16
  on the same screen? At 16 thumbs the per-card area is 50 × 60 —
  smaller than a `lv_font_montserrat_12` glyph. We'd be reduced to
  a coloured-dot grid, which is *not nothing* (status-at-a-glance)
  but is a different design. Defer to v1.0+ if anyone asks.
