# palette

Exact colour values for `esp32-agent-dashboard`. The firmware's
`main/theme.c` pulls these numbers verbatim; docs, homepage and device
must read as one system.

## The contract (before any token)

**Colour follows STATE, never the page.** The device has exactly three
stateful colours and one glyph (the breathing ring) that wears them:

| State | Colour | Meaning |
|---|---|---|
| **gold** | `#B89020` (bright `#E0B43C` on dark) | your move — an agent handed the turn back |
| **teal** | `#2BB3B1` (light-bg `#0E7C7B`) | an agent is thinking |
| **dim** | `#7D746C` | nothing needs you |

Everything else on the panel is achromatic. The decoration layer
(`ui_deco`) is **never** allowed a colour of its own — it expresses
state through tempo, not hue. Introducing a fourth signal colour means
trading away the state contract; don't.

## Core neutrals (shared with esp-harness)

| Token | Hex | Use |
|---|---|---|
| `bg` / noir | `#0B0A09` | Device background, homepage background. Warm near-black — never pure `#000`. |
| `surface` / ink | `#1C1814` | Cards, fleet rows, code blocks on dark |
| `paper` | `#F3EEE2` | Primary text on dark; light-theme background |
| `ink-fade` | `#8A807A` | Secondary text on dark |
| `ink-mute` | `#5A514A` | Tertiary text, decoration strokes, hairlines |

## Accents

| Token | Hex | Use |
|---|---|---|
| `gold` | `#B89020` | The product moment. Ring, greeting, project chip, waiting rows. |
| `gold-bright` | `#E0B43C` | Gold emphasis on dark (waiting-row meta, urgent) |
| `teal-bright` | `#2BB3B1` | Running/thinking on dark; `codex` agent accent |
| `teal` | `#0E7C7B` | Same on light backgrounds |
| `rust` | `#B8431A` | `claude-code` agent accent; the nav-dot marker; the family tie to esp-harness. A dot, not a paint bucket. |
| `moss` | `#344A36` | ok / done / passing (badges, docs) |

Agent kinds beyond claude-code/codex get deterministic hue-allocated
accents at runtime (`theme.c`, djb2 + golden-angle); they are data
colours, not brand colours.

## Device themes (`dash config theme=…`)

| Theme | Background | Foreground | Notes |
|---|---|---|---|
| `noir` (default) | `#0B0A09` | `#F3EEE2` | The brand look; AMOLED black = pixels off |
| `lab` | `#F3EEE2` | `#1C1814` | Light bench variant |
| `mono` | `#080808` | `#EDEDED` | Single-hue accessibility variant |

## Rules of thumb

- Dark surfaces are the native medium (the panel is an AMOLED). Docs
  and homepage commit to noir; light contexts (print, light-theme
  README views) use paper + ink with the same accents.
- Gold is *earned*: it appears only when something actually awaits the
  user. Never use it as decorative emphasis in UI mockups.
- Decoration/structure strokes: `ink-mute`, hairline-thin is fine
  (lines may be thin; blocks must not be small — see the ui_deco notes
  in `CLAUDE.md`).
- Body text on noir → `paper`; secondary → `ink-fade`; never grey-on-
  colour.
