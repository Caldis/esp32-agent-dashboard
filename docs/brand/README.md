# brand

Visual identity assets for `esp32-agent-dashboard`. Use these instead
of recreating the mark in slides / blog posts / talks.

The identity **is the device**: everything here is drawn in the
firmware's own design language — noir AMOLED field, the breathing ring
in its gold pose, ui_deco's functional-ornament vocabulary (corner
brackets, clamp brackets, segment baselines), and the state-colour
contract. If an asset introduces a visual idea the panel itself doesn't
have, it doesn't belong here.

## Mark concept

The logo is the **panel's gold pose reduced to geometry**:

- A **noir rounded square** — the panel itself. Corner radius is
  76/480 of the edge, the *measured* radius of the physical panel
  (`?vis`, v7.3). Always dark: on the device, black pixels are off.
- A **gold ring with a centre dot** — the breathing ring in the
  "your turn" state. The one status glyph the device has.
- **Clamp brackets** either side of the ring and **corner brackets** —
  the ui_deco decoration layer, achromatic as it is on the device.
- A **segment baseline** — the deco strip under the content band.
- A single **rust nav dot** at 25% width — the dashboard key's
  position dot, and the quiet family tie to esp-harness. One dot;
  don't expand it.

Read literally: *the moment the machine hands you the turn.*

## Files

| File | Use |
|---|---|
| `logo.svg` | 120×120 mark. Works on light and dark (the panel carries its own field). |
| `logo-dark.svg` | Same mark, stronger edge hairline for near-black backgrounds. |
| `wordmark.svg` | Mark + `esp32-agent-dashboard` in Archivo; hyphens in gold. |
| `favicon.svg` | 32×32 reduction: panel + gold ring + baseline. |
| `social-card.svg` | 1280×640 Open Graph card. Rasterise to PNG before uploading to GitHub. |
| `palette.md` | Exact hex codes + the state-colour contract. `main/theme.c` pulls these values. |

Legacy assets (the teal pulse-line mark, paper-field social card,
Fraunces wordmark) live in git history alongside the archived v0.1
homepage (`docs/archive/`).

## Colours

See [`palette.md`](./palette.md) — the contract matters more than the
swatches. Gold appears only where something genuinely awaits the user;
decoration is achromatic; rust is a one-dot family marker, not an
accent.

## Typography

| Role | Family | Notes |
|---|---|---|
| Display | **Archivo** (variable, wide wdth) | Homepage headings, wordmark |
| Body | **Instrument Sans** | Homepage body text |
| Mono | **IBM Plex Mono** | Data readouts, code, panel-voice labels |
| CJK | **Noto Sans SC** (subset) | The greeting words (该你了…) |

The device itself renders Consolas-flavoured mono + a GB2312 SimHei
subset; docs mono text is the same voice one register up.

## Don'ts

- **Don't stretch the mark.** Always 1:1.
- **Don't recolour the ring.** Gold is the product moment; a teal or
  rust ring is a different (wrong) claim about state.
- **Don't add colour to the decoration geometry.** Brackets, ticks and
  baselines are achromatic on the device and stay achromatic here.
- **Don't expand the rust dot.** One dot is the whole family marker.
- **Don't put the mark on a light square.** The panel is its own
  field; placing it directly on paper backgrounds is fine, but never
  re-field it in white.
