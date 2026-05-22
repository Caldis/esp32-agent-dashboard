# Agent kinds

The wire protocol's `agents[].kind` field. v1 ships with two
first-class kinds (`claude-code`, `codex`); v0.5.0 expands to an
open registry with deterministic accent allocation for unknown kinds.

## v0.1.x — first-class set

| Kind | Accent (noir) | Accent (lab) | Source |
|---|---|---|---|
| `claude-code` | `#B8431A` rust | `#B8431A` rust | Claude Code CLI hooks |
| `codex` | `#2BB3B1` teal-bright | `#0E7C7B` teal | Codex CLI wrapper |
| `other` | `#5A514A` ink-mute | `#5A514A` ink-mute | fallback |

These three are hard-coded in `theme.c::theme_accent_for_kind`.

## v0.5.0 — open registry

The bridge gains an `[agent_kinds.<name>]` section in
`~/.claude-buddy/config.toml`:

```toml
[agent_kinds.cursor]
display_name = "Cursor"
icon_glyph   = ""   # LV_SYMBOL_DRIVE
accent_hue   = 215        # degrees on the colour wheel — bridge passes to device

[agent_kinds.aider]
display_name = "Aider"
icon_glyph   = ""
accent_hue   = 35

[agent_kinds.qwen-code]
display_name = "Qwen-Code"
icon_glyph   = ""
accent_hue   = 305
```

Bridge sends `dash config '{...,"agent_kinds":[{"name":"cursor","accent_hue":215,...}],...}'`
on connect. Device stores per-kind accents in NVS and uses them
instead of the named palette entries when matching.

## Accent allocation for unknown kinds

When the device receives an `agents[].kind` that's neither in the
hard-coded set nor in the config-supplied registry, it falls back to
deterministic hue allocation:

```c
/* Per-kind hue picker, deterministic so the same kind always renders
 * the same colour across reboots. Uses golden-ratio rotation so any
 * N kinds get maximally-spread hues with no clustering. */
static uint16_t hue_for_kind(const char *kind) {
    uint32_t h = 5381;                         /* djb2 */
    for (const char *p = kind; *p; ++p) h = h * 33 + (uint32_t)*p;
    /* Golden-ratio multiplier in degree space. */
    return (uint16_t)((h * 137u) % 360u);
}
```

The hue → RGB conversion uses HSL with fixed saturation/lightness
chosen per theme:

| Theme | S | L |
|---|---|---|
| noir | 0.55 | 0.55 |
| lab  | 0.65 | 0.40 |
| mono | (no accent — drops to ink-mute) |

## Wire format

`dash snapshot` payload includes a per-agent kind:

```json
{
  "agents": [
    { "kind": "cursor", "session_id": "cu_42", ... },
    { "kind": "qwen-code", "session_id": "qc_88", ... }
  ]
}
```

No allow-list. The device renders whatever kind string arrives. The
bridge SHOULD send `dash config` with the kind metadata so the user-
visible display name and icon look intentional, but the device
degrades gracefully if it doesn't.

## v0.5.0 implementation footprint

Estimated:

- `main/theme.h` — add `theme_accent_for_kind_hue(uint16_t hue)`.
- `main/theme.c` — implement the HSL → RGB conversion + djb2 hash.
- `main/agent_state.h` — bump `AGENT_KIND_MAX` from 16 to 24 to fit
  longer kind names (`"qwen-code"` is 9; want headroom).
- `main/harness/agent_commands.c::cmd_dash_config` — parse the
  `agent_kinds` table from the config payload, store in NVS.
- `tools/claude_buddy_bridge.py` — `[agent_kinds.<name>]` config
  loader, append to `dash config` payload on connect.

Total: ~150 LOC. Doable in one PR.

## Why not just accept any string?

We DO accept any string. The registry is a HINT for display names +
icons. The minimal contract is: kind string → deterministic hue.
Names + icons are optional ergonomics.
