# Wiring Claude Code + Codex into the dashboard

Reference for how the bridge daemon connects to the two CLIs. The
actual bridge code lives in `tools/claude_buddy_bridge.py` (agent H
is building it); this document is the conceptual map so the
firmware-side and bridge-side decisions stay consistent.

## Claude Code hooks

Claude Code emits structured events via a hook system configured in
`~/.claude/settings.json`. Each event runs an external command with
the event payload on stdin; the command's stdout can short-circuit
or modify the action (used here for permission decisions).

### Relevant events for the dashboard

| Event | When | What we extract |
|---|---|---|
| `SessionStart` | new claude-code session starts | bump `total`, append entry "session N started" |
| `UserPromptSubmit` | user submits a prompt | update `msg`, mark session as `running` |
| `PreToolUse` | model wants to call a tool | if `permission == "ask"`, push `dash prompt` and BLOCK on device decision |
| `PostToolUse` | tool call finished | append `entries` line; update `tokens` |
| `Stop` | turn finished | session goes `idle` (or `running` if more turns queued) |
| `SubagentStop` | subagent finished | append entries line |
| `Notification` | permission needed / system notice | update `msg`; potentially `dash prompt` |

### Permission decision round-trip

The interesting hook is `PreToolUse`. The bridge:

1. Receives the `PreToolUse` JSON on stdin.
2. Pushes `dash prompt {id, tool, hint}` to the device.
3. Reads from the device serial until it sees
   `EVT: permission id=<same id> decision=<once|deny>`,
   or times out at 60 s.
4. Returns to Claude Code via stdout:
   - `{"hookSpecificOutput": {"permissionDecision": "allow"}}` for `once`
   - `{"hookSpecificOutput": {"permissionDecision": "deny", "permissionDecisionReason": "..."}}` for `deny`

This works because `PreToolUse` is a **blocking** hook — Claude Code
waits for the command to exit before continuing.

## Codex CLI integration

Codex doesn't expose a hooks system the same way as Claude Code (as
of writing). Two viable approaches:

- **Tee wrapper (recommended for v0)**: rename `codex` → `codex.real`,
  drop a wrapper that tees stderr to `hook_dispatch.py codex_stream`.
  Captures the read-only side (running, entries, tokens). No
  veto/permission flow — Codex's prompts stay interactive.
- **Codex MCP server (v1)**: more complex; user has to opt-in per
  session. Deferred.

## Bridge daemon lifecycle

The bridge runs as a long-lived process. The `hook_dispatch.py`
stubs are short-lived (one per hook fire); they talk to the daemon
over a Unix-domain socket or Windows named pipe. The daemon owns
the serial port; the stubs are synchronous (they wait for the
daemon's response so `PreToolUse` can block on the device
decision).

## End-to-end sanity test (once H finishes)

```powershell
# 1. Start the bridge in one terminal
python D:\Code\esp32-agent-dashboard\tools\claude_buddy_bridge.py serve --port COM9

# 2. In a second terminal, run Claude Code
claude-code "build a tiny ESP32 app that blinks"

# 3. Watch the device:
#    - idle → sessions when CC starts
#    - entries shows recent tool calls
#    - prompt appears when CC wants to Bash something
#    - Press BOOT to approve → CC proceeds
#    - Press USER to deny → CC tries alternatives
#    - Back to idle when the session ends
```
