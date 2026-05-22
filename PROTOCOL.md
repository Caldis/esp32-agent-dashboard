# esp32-agent-dashboard wire protocol (v0)

The contract between the host bridge and the device. Both sides
must follow this exactly. Versioned `v0` because BLE NUS in `v1`
will replace the transport but keep the schema.

## Transport (v0)

USB-Serial JTAG over the device's USB-C, 115200 baud, accessed via
the esp-harness `console_protocol` (which sits inside the standard
ESP-IDF console interface). One JSON object per host→device command,
host accumulates device replies until it sees `OK:`/`ERR:`/`EVT:`.

The console protocol handles the framing — the host bridge uses
`esp-harness console --cmd 'dash <verb> <json>'` per snapshot (or
`pyserial` directly for the persistent prompt/decision round-trip).

## Commands (host → device)

Each is one console line: `dash <verb> '<json>'` where the JSON is a
single argv-token (outer double-quotes around the JSON; inner
escaped). The console tokenizer's quote support (added in
esp-harness v1.7.1) handles spaces inside the JSON payload.

### `dash snapshot`

Periodic state push. Sent every ~250 ms when something changes, plus
a 10 s keepalive.

```json
{
  "total":       3,
  "running":     1,
  "waiting":     1,
  "msg":         "approve: Bash",
  "entries":     ["10:42 git push", "10:41 yarn test", "10:39 reading file"],
  "tokens":      184502,
  "tokens_today":31200,
  "prompt":      null
}
```

Field rules:
- `total` ≥ 0, count of all sessions.
- `running` ≤ `total`, sessions actively generating.
- `waiting` ≤ `total`, sessions blocked on a permission prompt.
- `msg` is a single-line summary suitable for a 466-pixel display.
  Max 64 UTF-8 bytes on the wire; device may truncate further for
  display.
- `entries` is the last N transcript lines (newest first), max 8
  on the wire. Device renders 4 — overflow is dropped.
- `tokens` and `tokens_today` are cumulative output tokens
  (non-decreasing; `tokens_today` resets at host's local midnight).
- `prompt` is `null` unless a session is blocked on permission, in
  which case it's the same shape as `dash prompt`'s body (see below).

### `dash prompt`

Switches device to the `prompt` scene immediately and waits for a
user button press.

```json
{
  "id":   "req_abc123",
  "tool": "Bash",
  "hint": "rm -rf /tmp/foo"
}
```

- `id` is whatever string the host wants to correlate decision back.
  Device echoes it verbatim in the EVT.
- `tool` is the tool name (any string; e.g. `Bash`, `Edit`, `Read`).
  Device may abbreviate for display.
- `hint` is the call-site detail (command for Bash, target path for
  Edit, etc.). Max 256 UTF-8 bytes; device wraps to fit.

Device fires `EVT: permission id=<id> decision=<once|deny>` when the
user presses **BOOT** (once) or **USER** (deny). After 60 s with no
press, device emits `decision=deny` and switches back to whatever
scene it was on before.

### `dash event`

A one-shot completed-turn transcript line. Appended to the rolling
`entries` buffer that `sessions` scene displays. Drops anything >2 KB.

```json
{
  "role":    "assistant",
  "content": [{"type":"text","text":"writing the patch now"}]
}
```

### `dash tokens`

Pushes a token-count sample to the sparkline shown on `tokens` scene.

```json
{
  "cumulative":    184502,
  "today":         31200,
  "latest_sample": 1240
}
```

- `latest_sample` is the count delta for the most recent turn.

### `dash idle`

No payload. Switches device back to `idle` scene immediately. Used
when all sessions end.

## Replies (device → host)

### `OK:` body

JSON object containing what was applied:

```json
OK: {"scene":"sessions","total":3,"running":1}
```

### `ERR:` body

A short reason string. JSON-parseable as a string is fine, prose is
fine too:

```
ERR: missing required field 'total' in snapshot
```

### `EVT: permission`

Fired by the prompt scene on physical button press OR on 60 s timeout.

```
EVT: permission id=<id> decision=once
EVT: permission id=<id> decision=deny
```

The host bridge MUST forward this back to the originating CLI (CC
or Codex) so the blocked session can proceed.

## Wire size

`CONSOLE_MAX_LINE = 1024` bytes in esp-harness v1.7.5. A `dash
snapshot` with 8 entries + a prompt is comfortably under that. The
host bridge SHOULD reject snapshots that serialize >1000 bytes after
JSON-compaction — drop the oldest entries first, keep `prompt` /
`msg` / counters intact.

If the line is too long, the device-side tokenizer silently
truncates (known esp-harness limitation; see HARNESS_GAPS.md).

## Versioning

This file is `v0`. Any breaking change bumps to `v1`. Additive
fields (new optional keys) don't bump the version — both sides
ignore unknown fields.
