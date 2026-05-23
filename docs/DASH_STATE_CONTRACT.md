# dash-state contract (v2.4.0)

The contract between the AI agent (Claude Code, Codex, …) and the
physical dashboard. The agent emits a **machine-readable suffix block**
at the end of every meaningful turn. The host bridge extracts it and
pushes a `summary` (marquee text) plus 2–4 `options` (short
executable phrases the user can paste verbatim as their next prompt)
to the device's AWAITING takeover.

Result: the device becomes a **pre-rendered conversation wall**. The
user glances at it from across the desk, picks a number, switches to
the terminal, and types just that number (or just sends the option
text). No composing required. Multiple-choice not fill-in-the-blank.

## Sentinel block

The agent appends, AS THE VERY LAST THING in its assistant message,
a fenced block of this exact shape:

```
<dash-state>
summary: <one-line text, 60-200 chars, recap of what the agent just did>
options:
  - <executable short phrase, 8-32 chars>
  - <another>
  - <another>
  - <up to 4 total>
</dash-state>
```

Rules:

- Block must be at the END of the message — anything after it is
  ignored.
- `summary` is ONE LINE only. Newlines inside summary are illegal;
  the parser uses the first `\n` after `summary:` as the terminator.
- `options` are bullet-prefixed (`-` or `*`), 2 to 4 entries. Empty
  options are dropped. Trailing whitespace stripped.
- The block is optional. Turns without one fall back to the
  classifier's default ("continue" with the assistant's last
  sentence as context).

## Wire shape (snapshot per-agent additions, v2.4.0)

```json
{
  "kind": "claude-code",
  "session_id": "...",
  "awaiting_kind":    "continue",
  "awaiting_summary": "Built and shipped v2.3.0 AWAITING takeover...",
  "awaiting_options": [
    "ship v2.4.0 too",
    "polish marquee animation",
    "rewrite scene_dashboard",
    "take a break"
  ],
  "awaiting_since":   1779600000
}
```

Wire-size cap: summary ≤ 200 chars on the wire (bridge truncates);
each option ≤ 32 chars; max 4 options. With AGENT_AWAITING_CONTEXT
the snapshot fits comfortably in CONSOLE_MAX_LINE=1024.

## Device rendering

For the `continue` kind, scene_awaiting renders:

```
   ┌──────────────────────────────┐
   │       14:32 · Clawd          │   ← eyebrow
   │           [pulse]            │   ← breathing glyph
   │                              │
   │         your turn            │   ← 48pt headline
   │          cc · sx4            │   ← 28pt agent chip
   │                              │
   │ ◀ Built and shipped v2.3.0 ▶ │   ← marquee summary, scrolls if > width
   │                              │
   │  1.  ship v2.4.0 too         │   ← numbered options (max 4)
   │  2.  polish marquee          │   ← 22pt, TEXT_DIM
   │  3.  rewrite scene_dashboard │
   │  4.  take a break            │
   │                              │
   │      waiting 38s · +N        │   ← footer
   └──────────────────────────────┘
```

The user types `1` or `ship v2.4.0 too` in the terminal — both
work because Claude Code accepts the option as a prompt verbatim.

For `approve` / `pick` / `type` / `clarify` kinds, options are
shown if present (e.g. `approve` can show "once / no / explain"
options), otherwise the existing kind-specific context applies.

## Extraction (hook_dispatch.py)

On `Stop` event, hook_dispatch reads `transcript_path` from the CC
hook payload, finds the LAST assistant message, runs a regex to
extract the `<dash-state>` block. If absent, hook_dispatch attaches
`last_assistant_text` only (existing v2.3.0 path) and lets the
bridge's classifier do its job.

Extraction regex:

```regex
<dash-state>\s*(.*?)\s*</dash-state>\s*$
```

with DOTALL flag, anchored to the END of the message text.

## Why a markdown-style block (not JSON / TOML)

- Plain-text appears in the conversation transcript without
  cluttering the user-visible reply when CC renders the message in
  the terminal (the block is just trailing fluff at the bottom).
- No parser ambiguity around quotes / nesting.
- Easy for the agent to compose — no escaping discipline.
- Trivial regex extraction host-side.

## Versioning

`v2.4.0` ships this contract with summary + options. Future
revisions might add:

- `urgency: low | normal | high` (overrides accent color)
- `until: "2026-05-23T14:35Z"` (deadline; device shows countdown)
- `auto_select: 2` (if user doesn't engage, default option 2 fires)
- `kind: pick / type / clarify` (agent classifies itself, bridge
  doesn't need text heuristic)

These are additive — older bridges ignore unknown keys.

## Author guidance (for Claude, Codex, and future agents)

When writing `options`:

- **Be verb-first**: "ship it" / "add tests" / "wait for CI" / "revert"
- **Short and specific**: "ship v2.4.0" beats "go ahead and ship"
- **No options if no real choice**: skip the block entirely for
  acknowledgements ("OK, done.") — the bridge's default continue
  context handles those gracefully.
- **2-4 options** is the sweet spot; 5+ is a list, not a choice.
- **Cover the obvious branches**: ship / polish / defer / reverse.
  Whatever the natural next-step decisions are.
