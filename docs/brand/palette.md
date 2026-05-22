# palette

Exact colour hex codes for `esp32-agent-dashboard`. These are sibling-
compatible with `esp-harness`'s palette (`paper`, `ink`, `rust`, …)
and add a teal accent that signals the dashboard / agent side of the
family. The firmware `theme.h` (when Agent F1 writes it) should pull
these exact values so docs + device read as one design.

## Core (shared with esp-harness)

| Token | Hex | Use |
|---|---|---|
| `paper` | `#f3eee2` | Light background, centre dot, light-variant text on dark |
| `ink` | `#1c1814` | Body text, screen ring, dark-variant background |
| `ink-mute` | `#5a514a` | Secondary text, captions on paper |
| `ink-fade` | `#8a807a` | Tertiary text, URL footers |

## Accents

| Token | Hex | Use |
|---|---|---|
| `teal` | `#0E7C7B` | **Primary accent.** Pulse line, hyphens in wordmark, the dashboard's signature colour. Codex-side cool counterpart to harness's rust. |
| `teal-bright` | `#2BB3B1` | Teal on dark backgrounds (`logo-dark.svg`, device noir theme). |
| `rust` | `#b8431a` | Tertiary accent. Kept as a single quiet dot in the mark for cross-family unity with esp-harness. Do not expand this. |
| `dusk` | `#6B7AA8` | Idle-scene indigo on device (matches `scene_idle.c`). Use sparingly in docs. |
| `moss` | `#344a36` | "Done / passing" status. Approve button on prompt scene. |
| `gold` | `#b89020` | "Warning / blocked" status. |

## Agent kind colours

Picked by the device based on `agents[].kind` in `dash snapshot`.

| Kind | Hex | Notes |
|---|---|---|
| `claude-code` | `#b8431a` (rust) | Anthropic/harness side — warm |
| `codex` | `#0E7C7B` (teal) | Codex side — cool. Matches brand accent. |
| `other` | `#5a514a` (ink-mute) | Generic / unknown agent |

## Theme variants (device-side, set via `dash config theme=…`)

| Theme | Background | Foreground | Accent |
|---|---|---|---|
| `noir` (default) | `#0b0a09` near-black | `#f3eee2` paper | `#2BB3B1` teal-bright |
| `lab` | `#f3eee2` paper | `#1c1814` ink | `#0E7C7B` teal |
| `mono` | `#0b0a09` | `#f3eee2` | (none — no accent) |

## When to reach for each

- **Need a link / emphasis on a paper background** → `teal #0E7C7B`.
- **Same on dark / device** → `teal-bright #2BB3B1`.
- **Sibling-marker / "this lives in the esp-harness world"** → a
  single quiet `rust #b8431a` dot. Don't paint things rust just
  because esp-harness does; that's the harness's territory.
- **Body text** → `ink #1c1814` on paper; `paper #f3eee2` on ink.
- **Secondary text / captions** → `ink-mute #5a514a` / `ink-fade
  #8a807a`.
- **Status: ok / done** → `moss #344a36`. **Status: blocked /
  warning** → `gold #b89020`. **Status: idle / sleepy** → `dusk
  #6B7AA8`.
