# Session replay / timeline scrubbing (v1.5.0)

Per-agent on-device ring buffer (last 30 minutes), rotary-encoder
scrubbing to walk back through transcript history, export the
selected window to the bridge as JSONL.

## Storage

Per-agent ring at 4096 bytes (covers ~30 min @ 1 entry/15s). Stored
in PSRAM (cheap there). When PSRAM is tight (8-agent scenarios), the
ring shrinks to 1024 bytes per agent (~7 min).

## UX

In `scene_sessions` or `scene_dashboard`, rotating the encoder
backwards walks the active agent's transcript pointer through the
ring. Press to "anchor here" — scene shows the snapshot AS IT WAS
at that moment, with a "(replaying)" badge. Rotate forward to
return to live.

## Wire commands

- `dash replay_dump <agent_kind> <session_id>` — emit the ring as
  a single `REPLAY_BEGIN/END` payload block (JSONL). Bridge saves
  to `~/.claude-buddy/replays/`.
- `dash replay_clear <agent_kind> <session_id>` — wipe the ring for
  one agent (privacy hygiene before sharing the device).

## Scaffold status

Spec + `main/replay/ring_buffer.{h}` stub land in v1.5.0. Rotary-
encoder wire-up + scene integration land in v1.5.x once we settle
on the encoder driver path.
