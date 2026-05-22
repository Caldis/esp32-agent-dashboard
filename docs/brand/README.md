# brand

Visual identity assets for `esp32-agent-dashboard`. Use these instead
of recreating the mark in slides / blog posts / talks.

This project is a **sibling** to [`esp-harness`](https://github.com/Caldis/esp-harness):
same paper / ink palette, same Fraunces + IBM Plex Mono typography,
same dashed-frame device boundary. The two marks should sit next to
each other on a docs site without arguing.

## Mark concept

The dashboard's logo is a **circular display split by a pulse line**:

- A central **circle** — the actual hardware: the 466 × 466 AMOLED
  panel on the Waveshare ESP32-S3-Touch-AMOLED-2.16 board. Where
  esp-harness's mark is a literal **H**, this one is a literal
  **screen**.
- A horizontal **teal pulse line** crossing through — the agent
  activity signal. Reads as a heartbeat / ECG trace at first glance,
  as a wire / serial line on second look.
- A small **paper-coloured dot** at the centre — the convergence
  point where two agent streams (Claude Code, Codex) meet. Echoes
  esp-harness's centre dot.
- A small **rust micro-dot** at the far right of the pulse — a
  single quiet nod to the esp-harness family. Don't expand this; one
  pixel is the whole point.
- A **dashed outer rectangle** — the device boundary, low-emphasis,
  identical to the esp-harness frame.

Read literally it's *a screen with a heartbeat*. The "two streams
converging at centre" reading is for anyone who pauses.

## Files

| File | Use |
|---|---|
| `logo.svg` | 120×120 mark, light-background variant. Default. |
| `logo-dark.svg` | Inverted for dark backgrounds; teal lifts to teal-bright. |
| `wordmark.svg` | Mark + `esp32-agent-dashboard` wordmark in Fraunces serif. Use in headers. |
| `favicon.svg` | 32×32 simplified mark for browser tabs. |
| `social-card.svg` | 1280×640 Open Graph / Twitter card. Attached to the GitHub repo. |
| `palette.md` | Exact hex codes + which token goes where. The firmware `theme.h` pulls these values. |

## Colours

See [`palette.md`](./palette.md) for the full table. Summary:

| Name | Hex | Use |
|---|---|---|
| Ink | `#1c1814` | Body text, screen ring |
| Paper | `#f3eee2` | Background, centre dot |
| Teal | `#0E7C7B` | **Primary accent.** Pulse line, hyphens, links |
| Teal-bright | `#2BB3B1` | Teal on dark backgrounds |
| Rust | `#b8431a` | Family-marker dot only — sibling tie to esp-harness |
| Dusk | `#6B7AA8` | Idle scene indigo (matches device `scene_idle`) |
| Moss | `#344a36` | "Approve / passing" status |
| Gold | `#b89020` | "Warning / blocked" status |

## Typography

Identical to esp-harness — the two projects share a docs voice.

| Role | Family | Notes |
|---|---|---|
| Display / wordmark | **Fraunces** | Variable serif. Italic hyphens read as "joining wires". |
| Body | **Geist** | Variable sans. Substitutes: system UI stack. |
| Mono | **IBM Plex Mono** | Code blocks, the monospace voice. |

## Don'ts

- **Don't stretch the mark.** Always 1:1 aspect.
- **Don't recolour the pulse line.** Teal is the load-bearing accent
  — the only place teal appears in the whole brand at full
  saturation.
- **Don't add a tagline next to the mark below 200 px wide.** Use
  the wordmark instead.
- **Don't put the light-background variant on photos / busy
  backgrounds.** Use the social-card layout, which has a deliberate
  clean field.
- **Don't expand the rust dot.** It's a one-pixel family-marker.
  Painting more of the mark rust collapses the sibling relationship
  with esp-harness into "we copied their colours".
- **Don't drop the dashed frame.** It's the visual handshake with
  esp-harness; without it the mark loses family membership.

If you need a variant that doesn't exist here (e.g. all-white for a
dark sticker, monochrome for a thermal printer), open an issue rather
than improvising — naming consistency matters at this layer.

## Sibling relationship

| Trait | esp-harness | esp32-agent-dashboard |
|---|---|---|
| Subject | device-facing (firmware scaffold) | agent-facing (dashboard for sessions) |
| Mark form | H letterform (vertical pair + crossbar) | Circular screen + horizontal pulse |
| Primary accent | rust `#b8431a` (warm) | teal `#0E7C7B` (cool) |
| Family marker | the rust crossbar itself | a single rust dot at end of pulse |
| Dashed frame | yes | yes (identical) |
| Centre dot | yes (paper) | yes (paper, with ink ring) |
| Type | Fraunces + Geist + IBM Plex Mono | identical |
