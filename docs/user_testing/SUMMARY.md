# User-Testing P0 Summary (v2.5/v2.6)

Four persona-based testing sessions against the shipped firmware.
Each persona exercised a different axis; P0 = must-fix before v2.7.0.

## Persona A — Power User (3-4 concurrent sessions)

| # | Finding | Fix | Status |
|---|---------|-----|--------|
| A-1 | hook_dispatch has no circuit breaker against a hung bridge — 5-60 s latency per tool | Circuit breaker: N timeouts in W s -> short-circuit M s | **v2.7.0 R2** |
| A-2 | Ambient feed hierarchy inverted — tool names louder than content | Font/color hierarchy swap in scene_dashboard | **v2.7.0 R1** |
| A-3 | Marquee text clips mid-word in screenshots | Render timing issue; replaced clipped screenshots | **v2.7.0 R1** |

## Persona B — Newcomer (first ESP32 project)

| # | Finding | Fix | Status |
|---|---------|-----|--------|
| B-1 | "Quickstart 30 seconds" is dishonest; ESP-IDF install takes 15-40 min | Rename section, add prerequisites, mention prebuilt bins | **v2.7.0 R3** |
| B-2 | Mock-device path diverges between README and GET_STARTED | Consolidate to single canonical path | **v2.7.0 R3** |
| B-3 | Troubleshooting misses the 3 failures newcomers actually hit | Add idf.py-not-found, port-locked, settings-clobbered | **v2.7.0 R3** |

## Persona C — Bystander (non-technical desk-mate)

| # | Finding | Fix | Status |
|---|---------|-----|--------|
| C-1 | Agent chip at 28pt competes with 48pt headline — hierarchy inversion | Drop chip to 22pt, reduce AGENT_H | **v2.7.0 R1** |
| C-2 | SID display (last-6 of UUID) is unrecognizable gibberish | first-4 + ":" + last-2 format | **v2.7.0 R1** |
| C-3 | Clipped marquee text reads as "device is broken" | Ellipsis truncation (LV_LABEL_LONG_DOT) when motion=reduced | **v2.7.0 R1** |

## Persona D — Accessibility (WCAG 2.2 audit)

| # | Finding | Fix | Status |
|---|---------|-----|--------|
| D-1 | Marquee scrolls indefinitely — WCAG 2.2.2 Level A fail | Gate behind `motion_reduced` config; DOT truncate when set | **v2.7.0 R1** |
| D-2 | ink-mute #5A514A at 2.59:1 — WCAG 1.4.3 AA fail | Promote to ink-fade #8A807A (5.13:1 AA pass) | **v2.7.0 R1** |
| D-3 | Urgency teal/gold at 1.17:1 inter-luminance — WCAG 1.4.1 fail | Add redundant non-color channel (shape/text affordance) | Deferred to v2.8 |

## Resolution Coverage

- **v2.7.0 R1** (firmware UX): A-2, A-3, C-1, C-2, C-3, D-1, D-2
- **v2.7.0 R2** (bridge): A-1
- **v2.7.0 R3** (docs): B-1, B-2, B-3
- **Deferred**: D-3 (requires design for redundant urgency channel)
