# Persona D — Accessibility / Inclusive-Design Audit

Reviewer: **D** (accessibility-leaning UX). Subject: v2.5.1 firmware
UX. Sources: `docs/brand/palette.md`, `main/theme.c`,
`main/scenes/scene_dashboard.c` (v2.5.1 ambient feed),
`main/scenes/scene_awaiting.c` (takeover, marquee + breathing dot),
live captures `docs/img/live-ambient-v251.png`,
`docs/img/live-ambient-v25.png` (the pre-fix wrap),
`docs/img/live-awaiting-options.png`,
`docs/img/live-awaiting-strip.png`. Heuristics:
WCAG 2.2 (1.4.3, 1.4.4, 1.4.6, 1.4.11, 2.3.3) plus standard
arc-min legibility math at the manufacturer-quoted viewing
distance of ~500 mm.

All contrast ratios below are computed from the WCAG relative-
luminance formula (sRGB → linear → 0.2126·R + 0.7152·G +
0.0722·B → `(L_light + 0.05) / (L_dark + 0.05)`), not estimated.

---

## 1. Contrast ratios — palette.md vs noir bg

Background `#0B0A09` has linear luminance `L_bg ≈ 0.00311` (effectively
zero — this is one of the darkest backgrounds you can ship without
going to pure `#000000`). All ratios below are *foreground on this
background* unless noted.

| Token | Hex | L_fg | Ratio vs `#0B0A09` | WCAG verdict |
|---|---|---|---|---|
| `paper` (body text) | `#F3EEE2` | 0.8555 | **17.05 : 1** | AAA (≥7) for normal & large |
| `ink-fade` (dim text) | `#8A807A` | 0.2225 | **5.13 : 1** | AA normal (≥4.5) pass; **AAA fail** (≥7) |
| `ink-mute` (caption) | `#5A514A` | 0.0877 | **2.59 : 1** | **AA fail** (normal & large both) — decorative only |
| `teal-bright` (accent) | `#2BB3B1` | 0.3638 | **7.79 : 1** | AAA pass — safe for text + UI components |
| `gold` (warning) | `#B89020` | 0.3048 | **6.68 : 1** | AA pass; AAA fail (just under 7) |
| `moss` brand (`#344A36`, original) | `#344A36` | 0.0605 | **2.08 : 1** | **FAIL** — unusable as status on dark |
| `moss-ok` firmware (`#588A5C`, COL_MOSS_OK) | `#588A5C` | 0.2110 | **4.92 : 1** | AA pass (normal & large); AAA fail |
| `rust` accent_claude | `#B8431A` | 0.1599 | **3.94 : 1** | AA large pass only (≥3); **fail normal text** (needs 4.5) |
| `dusk` idle indigo | `#6B7AA8` | 0.1916 | **4.55 : 1** | AA normal pass (barely); AAA fail |

### Findings

- **Body paper-on-noir is genuinely excellent.** 17 : 1 is at the
  ceiling of WCAG; this clears AAA with margin. The headline
  "your turn" `#F3EEE2` 48 px in
  `scene_awaiting.c:433` is the gold-standard slot in the whole UI.
- **`ink-mute #5A514A` is used for body text** in
  `scene_awaiting.c:417` (eyebrow) and `:492` (footer). At 2.59 : 1
  this **violates WCAG 1.4.3 (Contrast Minimum)** for both normal
  and large text. The eyebrow string is `"HH:MM device_name"` — load-
  bearing for "which device am I looking at" — yet it is below the
  AA large-text floor of 3.0. **P0.**
- **Brand `moss #344A36` for "success" is unshippable on noir**
  (2.08 : 1). The firmware already silently substitutes
  `#588A5C` (`scene_dashboard.c:179`) which passes AA — but this is
  off-palette, undocumented in `palette.md`, and theme switching to
  `lab` will re-introduce the readable variant or, worse, break
  parity. **palette.md should be amended** with an `on-dark` moss.
- **`rust #B8431A` as accent_claude on noir is 3.94 : 1.** Used for
  the "Claude" agent identity color on dark surfaces. AA large only,
  fails for any small label (e.g. the `cc` chip if it is 14 px or
  below). The 28 px `cc 0_demo` chip in `live-awaiting-options.png`
  is a "large text" candidate under WCAG (≥18 pt ≈ 24 px), so it
  passes for that specific use — but at 14 px (eyebrow scale) it
  would fail. Document the size floor explicitly.

---

## 2. Font sizes vs viewing distance — WCAG 1.4.4 + readability

Display: 2.16" round AMOLED, 466 × 466 px, physical Ø ≈ 36.9 mm →
**pixel pitch ≈ 0.079 mm/px**. Distance: 500 mm (the
manufacturer-quoted desk-glance distance, also in `UX_REVIEW.md §3`).

LVGL's `lv_font_montserrat_N` is rasterised at **N px em-height**
(not pt). I report visual angle in arc-min (1° = 60 arc-min). The
common readability threshold for at-a-glance reading by people with
20/20 vision is ~16 arc-min of *cap-height* (≈ 0.27°). The "AA-pass
minimum of ~0.5° / 30 arc-min" threshold the brief cites is the
*conservative* low-vision target (roughly equivalent to 20/40 acuity
+ desk distance) — I will mark every size against both.

Cap-height for Montserrat ≈ 0.70 × em.

| `lv_font_montserrat_*` | Cap-height (mm) | Visual angle | 20/20 pass (≥16′)? | Low-vision (≥30′)? | Used where |
|---|---|---|---|---|---|
| 12 | 0.66 | **4.57 ′** | **FAIL** | FAIL | `scene_dashboard.c:325` "active" / "tokens today" footer captions |
| 14 | 0.77 | **5.30 ′** | **FAIL** | FAIL | `scene_awaiting.c:418` eyebrow; `scene_dashboard.c:312` row rest column |
| 16 | 0.88 | 6.07 ′ | FAIL | FAIL | `scene_awaiting.c:493` footer "waiting Xs" |
| 20 | 1.10 | 7.59 ′ | FAIL | FAIL | `scene_awaiting.c:468` marquee summary |
| 22 | 1.21 | 8.34 ′ | FAIL | FAIL | `scene_awaiting.c:452` context lines; `scene_dashboard.c:309` verb column |
| 28 | 1.54 | **10.61 ′** | FAIL | FAIL | `scene_awaiting.c:441` agent chip; `scene_dashboard.c:319` footer numbers |
| 48 | 2.64 | **18.18 ′** | **PASS** | FAIL | `scene_awaiting.c:434` "your turn" headline; `scene_dashboard.c:291` clock |

### Findings

- **Only the 48 px display sizes clear the conservative-vision 20/20
  threshold.** Every body, caption, and chip size in the v2.5 UX is
  below 16 arc-min of cap-height. This is the **single biggest
  accessibility liability** in the project.
- Concretely in `live-ambient-v251.png`: the row-rest column
  (`14 px / 5.3 arc-min`) shows the strings `00:11 ok cc x_a3 src/auth.py +8 -2`.
  Each character is ~5 arc-min wide; this is at the edge of
  letter-recognition (Snellen 20/20 = 5 arc-min per stroke). A
  20/40 user (which WCAG 2.5.5 / 1.4.4 implicitly accommodates)
  cannot read this row at 50 cm.
- **WCAG 1.4.4 (Resize text)** is a structural concern: the device
  has **no** text-resize control. There is no `dash config
  text=large` knob. To comply, ship either:
  1. a `text=large` theme that scales the base font set
     (montserrat_14 → montserrat_20, etc.), or
  2. a "lean closer / read mode" gesture that re-renders the
     ambient feed with 22 px + 3 rows instead of 14 px + 6 rows.
- The strip capture (`live-awaiting-strip.png`) shows the worst
  case: the context line "All set. Refactor of src/auth.py is
  committed" rendered at 22 px (8.34 arc-min cap) sits *next to* the
  48 px headline — the contrast in size correctly signals
  hierarchy, but the smaller line is at the limit of foveal
  recognition for an unimpaired reader and beyond it for anyone
  with even minor refractive error.

---

## 3. Color blindness — teal vs gold urgency coding

The whole urgency system pivots on `is_urgent()` in
`scene_awaiting.c:79`, which picks **gold `#B89020`** (warning) vs
**teal-bright `#2BB3B1`** (relaxed). Both colors are also used as
the agent-chip color (`scene_awaiting.c:266`). This is the *only*
signal distinguishing "your turn" (continue, teal) from
"approve?" (gold) when the user glances peripherally.

Luminance: teal = 0.3638, gold = 0.3048. **Inter-color contrast
ratio = 1.17 : 1.** Almost identical brightness. Hue is the only
discriminator.

Simulated under each color-vision deficiency (I follow the
Brettel/Viénot/Mollon 1997 simulation):

| CVD type | Teal `#2BB3B1` appears as | Gold `#B89020` appears as | Distinguishable? |
|---|---|---|---|
| Protanopia (no L-cones; ~1% males) | `~#9BAEA6` muted greyish-teal | `~#A29F47` dull olive | **Yes** — hue still diverges (cool vs warm-olive) |
| Deuteranopia (no M-cones; ~6% males) | `~#A6AFA3` desaturated blue-grey | `~#A2A042` mustard | **Marginal** — both desaturate; only saturation cue separates them. Luminance 1.17 : 1 gives no help. |
| Tritanopia (no S-cones; ~0.03%, rare) | `~#3FB6BC` shifts toward magenta/pink-cyan | `~#B89F22` reads near-yellow | Distinguishable, but both fall in the warm half of the wheel; teal loses its "cool" character entirely. |
| Achromatopsia (full monochromacy; ~1 in 30 k) | mid-grey ~`#737373` | mid-grey ~`#6F6F6F` | **FAIL** — 1.17 : 1 luminance gap is invisible. |

### Findings

- **Deuteranopia + achromatopsia are the failure modes.** 6 % of
  males cannot reliably distinguish the two urgency states by hue
  alone, and the near-equal luminance gives them no fallback. This
  **violates WCAG 1.4.1 (Use of Colour)**: information conveyed by
  color must also be available another way.
- The codebase has the redundant signals it needs but does not use
  them consistently:
  - The **glyph** already varies (`LV_SYMBOL_WARNING` for approve
    vs `glyph_pulse` for continue — `scene_awaiting.c:159-170`).
    That's one redundant channel.
  - The **headline** varies ("approve?" vs "your turn"). Another.
- But the **agent-chip color**, the **marquee text color**, and the
  **footer color** all collapse onto the single
  teal/gold channel. A deuteranope at 50 cm sees the glyph (small,
  6.5 arc-min wide) and the headline, but the peripheral cue
  ("this row is gold = urgent") is lost.
- **Recommendation:** add a second redundant signal that is
  luminance-based, e.g. a `>` chevron prefix on urgent kinds, or
  bold-weight headline only for urgent. The free fix is in
  `scene_awaiting.c:248` — instead of `is_urgent(k) ? gold : teal`,
  also set `LV_STATE_BOLD` or a left-edge accent bar when urgent.

---

## 4. Motion — WCAG 2.3.3 (Animation from Interactions)

Two animations in v2.5:

| Source | Animation | Duration | Trigger | Pause control? |
|---|---|---|---|---|
| `scene_awaiting.c:188-198` | Breathing dot (14 → 28 px, sin-ish via `apple_ease_out`, repeat infinite) | 2 s in + 2 s out = **4 s cycle** | Auto on AWAITING_CONTINUE scene | **No** |
| `scene_awaiting.c:466` | Marquee summary (`LV_LABEL_LONG_SCROLL_CIRCULAR`, ~30 px/s) | Indefinite while text > 380 px | Auto when any summary present | **No** |
| `scene_idle.c` (carry-over from v1) | zZz fade + breathing dot | 2.4 s cycle | Idle scene | No |

### WCAG mapping

- **WCAG 2.3.3 (Animation from Interactions, AAA)** — motion
  animation triggered by interaction can be disabled unless essential
  to functionality.
- **WCAG 2.2.2 (Pause, Stop, Hide, A)** — for any moving content
  that auto-starts, lasts >5 s, and is presented in parallel with
  other content, users **must** be able to pause / stop / hide it.
  The marquee on `LV_LABEL_LONG_SCROLL_CIRCULAR` **runs
  indefinitely** while AWAITING is active. It is parallel with
  static content (headline, options list). **This is a 2.2.2
  Level A failure.** Not AAA-aspirational — Level A. P0.
- **`prefers-reduced-motion` analog** — the
  Vestibular-Disorders subgroup of the W3C cite horizontal-scrolling
  text and rapid sine-pulsing as common trigger patterns for
  vestibular distress. The 4-second breathing cycle is gentle
  (well below the >3 Hz flash threshold of WCAG 2.3.1) but the
  marquee scrolls continuously and crosses the user's foveal axis
  every 12 s or so. A vestibular-sensitive user reading the
  device at 50 cm has nowhere to look that is motion-free.

### Findings

- **The marquee is the riskiest single behaviour in the v2.5 UX.**
  It auto-starts, never stops, has no user control, and is on the
  takeover scene that demands the user's attention. Vestibular
  users will avert their gaze, which defeats the takeover's
  purpose.
- The breathing dot is *probably* fine for most vestibular users
  (slow, predictable, single-axis-of-scale not translation) but
  combined with the marquee on the same screen, the cumulative
  motion load is non-trivial.

---

## 5. Glyph clarity at small sizes

The five awaiting kinds use:

| Kind | Glyph | Render in `scene_awaiting.c` |
|---|---|---|
| continue | filled dot inside 72 px ring (pulse) | `:158` `glyph_pulse` |
| approve | `LV_SYMBOL_WARNING` (triangle ⚠) | `:160` |
| pick | `LV_SYMBOL_LIST` (lines/grid icon ☰) | `:163` |
| type | `LV_SYMBOL_EDIT` (pencil ✎) | `:166` |
| clarify | `LV_SYMBOL_BELL` (🔔) | `:169` |

At the takeover scene's chosen size (`montserrat_36` glyph font,
`:147`), cap ~25 px ≈ 13.5 arc-min of visual angle — borderline
for confident shape recognition at 50 cm.

In the **strip view** (`live-awaiting-strip.png`) all five glyphs
render side-by-side at the same size. From the strip:

- The **pulse-dot** vs **bell** vs **warning-triangle** are
  unambiguous — they have distinctive outline silhouettes (circle
  vs domed-with-stem vs triangle).
- **`LV_SYMBOL_LIST`** ☰ and **`LV_SYMBOL_EDIT`** ✎ — both are
  small-stroke icons inside a similar bounding box. At the strip
  scale, they remain distinguishable, but **if these glyphs were
  shrunk into the ambient feed at 14 px** (e.g. as per-row icons),
  the LIST horizontal lines would collapse into a single thick
  blur and the EDIT pencil's diagonal would be similarly
  unresolvable.
- The ambient feed currently does **not** include glyphs (the
  status column is the literal string `ok` / `..` / `·` —
  `scene_dashboard.c:215-218`). This is the *right* call for that
  scale.
- **Risk:** if a future version pulls these `LV_SYMBOL_*` icons
  into the 14 px feed rows for visual punch, LIST/EDIT will
  collide with EDIT/BELL at that pixel density. Document the
  **22 px floor** for LV_SYMBOL glyphs.

---

## 6. Text wrap behaviour

Comparing `live-ambient-v25.png` (pre-fix) against
`live-ambient-v251.png` (post-fix):

- **v2.5.0** has the multi-column overlap bug: row 2 shows
  `login (42 hits)` from the rest-column visibly overlapping the
  *next row's* "Edit" verb (~y=260). Row 4 shows
  `login (42 hits)` overlapping. This is a text-bleed: multi-
  column absolute layout with no clipping.
- **v2.5.1** collapses both into single labels with
  `LV_LABEL_LONG_DOT` (`scene_dashboard.c:271`). Wraps now
  truncate cleanly with the `…` ellipsis on overflow, never bleed.

### Findings

- The v2.5.1 fix is structurally the right approach: **bound the
  overflow inside the label**, don't trust column gutters to
  contain it.
- But truncation has its own accessibility cost: the row label
  `00:11 ok cc x_a3 src/auth.py +8 -2` is 30 chars at 14 px. The
  container is 268 px (`scene_dashboard.c:171`). At ~7 px/char
  average for Montserrat 14, that's ~210 px → just fits. Add a
  longer file path and the `+8 -2` is silently dropped, breaking
  the user's ability to see the diff size. **Dropping content
  via ellipsis is an accessibility regression vs full wrap**, and
  there is no affordance for the user to see what was elided
  (no tooltip, no scroll, no expand). WCAG 1.4.10 (Reflow) is the
  closest applicable criterion — content shouldn't require
  scrolling in two dimensions, but the device's response of
  "silently truncate" is arguably worse for understanding.
- **Recommendation:** at least bold the verb (already is) and
  *right-truncate* the rest-column with ellipsis at start (`… +8 -2`)
  instead of end, so the load-bearing diff metrics stay visible.

---

## 7. Reduced-motion mode

**Yes, ship `dash config motion=reduced`.** Concrete spec:

| Knob | Default | `motion=reduced` |
|---|---|---|
| awaiting breathing dot | 2 s/2 s breath cycle | static dot, no anim |
| awaiting marquee | `LV_LABEL_LONG_SCROLL_CIRCULAR` | `LV_LABEL_LONG_DOT` (truncate with ellipsis) |
| idle zZz fade | 200 ms-stagger fade cycle | static "z" glyph |
| scene transitions (future) | 200 ms cross-fade | 0 ms hard cut |

This addresses **WCAG 2.3.3 (AAA)** and is also the right design
default for users with vestibular disorders, attention disorders, or
who simply find continuous motion distracting in a peripheral-glance
device. Implementation cost: ~20 LOC in
`scene_awaiting.c:arm_breath()` and the label long-mode setters.

Bonus: also expose `dash config text=large` (see §2) so the two
knobs together form a documented "accessibility profile".

---

## 8. Top 3 P0 findings

These are accessibility **blockers** that must land before v2.5
can be marketed as suitable for production / general consumer
release (rather than enthusiast / developer audience).

### P0-1 — Marquee scrolls indefinitely with no pause control (WCAG 2.2.2 Level A fail)

`scene_awaiting.c:466` sets `LV_LABEL_LONG_SCROLL_CIRCULAR` on the
summary line whenever the text exceeds 380 px. It auto-starts,
parallels static content, lasts >5 s, and has no
pause/stop/hide. This is a **Level A** WCAG conformance failure —
not aspirational, blocking. Fix: gate behind a `motion=reduced`
config (default OFF for vestibular safety on a takeover scene), or
replace circular-scroll with a one-shot scroll + DOT truncate that
stops after one pass.

### P0-2 — `ink-mute #5A514A` body text on noir at 2.59 : 1 (WCAG 1.4.3 fail)

`scene_awaiting.c:417` eyebrow and `:492` footer use `ink-mute` for
text that is not decorative — it carries the current time, device
name, and elapsed-wait duration. 2.59 : 1 fails both AA normal
(4.5) and AA large (3.0). Fix: route all caption-class text through
`ink-fade #8A807A` (5.13 : 1, AA pass) and reserve `ink-mute` for
decorative borders/dividers only. Cost: one-line color change in
each scene file.

### P0-3 — Urgency channel collapses onto luminance-tied hue pair (WCAG 1.4.1 fail)

Teal `#2BB3B1` (relaxed) vs gold `#B89020` (urgent) sit at
1.17 : 1 inter-luminance contrast. Deuteranopes and anyone with
achromatopsia cannot distinguish them. The glyph + headline carry
the same information redundantly *at the focal element*, but the
**agent chip**, **marquee tint**, and **footer color** also key off
teal/gold and lose all signal for these users. Fix: add a second
redundant non-color channel for urgency — either a leading
`> ` chevron on urgent headlines or a 4 px left-edge accent bar
on the urgent scene's panel. Cost: ~10 LOC in `scene_awaiting.c`.

---

## Appendix — methodology

- Contrast: WCAG 2.2 relative luminance formula, computed
  channel-by-channel in linear-sRGB. Numbers cross-checked by
  re-deriving `#F3EEE2`-on-`#0B0A09` against published WebAIM
  calculator (17.05 : 1).
- Visual angle: pixel pitch derived from manufacturer's quoted
  2.16" diagonal and 466 px resolution. arc-min = `60 × atan(size
  / distance)` in degrees, with size in mm and distance = 500 mm.
- CVD simulation: descriptive (Brettel/Viénot/Mollon 1997 model);
  hex shifts are approximate, used to qualify hue-direction not
  to compute precise post-simulation appearance.
- LVGL font sizing assumption: `lv_font_montserrat_N` rasterised
  at N-px em-height (verified against LVGL upstream docs). If the
  project rebuilds fonts at a different point/px mapping the
  cap-height numbers will rescale linearly.
