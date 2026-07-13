# esp32-agent-dashboard wire protocol

`v1` (current). `v0` was USB-Serial-only single-agent. `v1` adds
per-agent sessions (Claude Code + Codex side-by-side), richer scene
data, device-side configuration. BLE NUS arrives in `v2`.

## Transport (v1)

USB-Serial JTAG / 115200 baud / esp-harness `console_protocol`
line framing. One JSON object per `dash <verb> "<json>"` console
line. Replies are `OK:` / `ERR:` / `EVT:` prefixed lines.

The G-7 fix (commit `esp-harness@664b14e`) made the tokeniser
preserve inner quotes on quote-leading tokens, so nested JSON
arrives at the device parser intact.

## Commands (host → device)

### `dash snapshot`

Periodic state push. Throttled by the bridge to ≤ 1 per 250 ms
(configurable via `--throttle-ms`). Plus a 10 s keepalive.

```json
{
  "agents": [
    {
      "kind": "claude-code",
      "session_id": "cc_abc",
      "status": "running",
      "cwd": "D:\\Code\\my-firmware",
      "msg": "editing main.c",
      "entries": [
        {"t":"10:42","tool":"Bash","summary":"git push"},
        {"t":"10:41","tool":"Edit","summary":"main.c (+8 -2)"},
        {"t":"10:39","tool":"Read","summary":"main.c (120 lines)"}
      ],
      "tokens": 84502,
      "tokens_today": 21200,
      "last_active_unix": 1779600000,
      "prompt": null
    },
    {
      "kind": "codex",
      "session_id": "cx_xyz",
      "status": "idle",
      "cwd": "D:\\Code\\other",
      "msg": "(stop)",
      "entries": [
        {"t":"10:30","tool":"Grep","summary":"login (42 hits)"}
      ],
      "tokens": 12300,
      "tokens_today": 12300,
      "last_active_unix": 1779599800,
      "prompt": null
    }
  ],
  "totals": {
    "total":2, "running":1, "waiting":0,
    "tokens": 96802, "tokens_today": 33500
  }
}
```

Field rules:
- `agents[].kind` ∈ {`claude-code`, `codex`, `other`}. Device picks
  a colour per kind from its palette.
- `agents[].status` ∈ {`running`, `waiting`, `idle`}.
- `agents[].entries[].tool` is the canonical tool name. Device may
  render an icon based on it (Bash/Edit/Read/Grep/Write/...).
- `last_active_unix` lets the device show "active 7 s ago" without
  needing its own clock sync (still useful — see `dash time` below).

Backwards compat: the flat v0 shape (`total`/`running`/`waiting`/
`msg`/`entries[]`/`tokens`/`tokens_today`/`prompt`) is still
accepted. The device treats a flat snapshot as a single-agent
implicit-kind `claude-code`. Bridge >= v1 emits the v1 shape.

### `dash prompt`

Unchanged from v0. The prompt scene now also shows which agent
submitted the prompt if `agent_kind` is provided:

```json
{
  "id": "req_abc123",
  "tool": "Bash",
  "hint": "rm -rf /tmp/foo",
  "agent_kind": "claude-code",
  "session_id": "cc_abc"
}
```

Device fires `EVT: permission id=<id> decision=<once|deny>` on
button press, plus `session_id=<id>` so the bridge can route the
decision back to the right agent.

### `dash event`

A one-shot transcript line (kept in v0 form for bridges that
haven't migrated):

```json
{
  "agent_kind": "claude-code",
  "session_id": "cc_abc",
  "role": "assistant",
  "content": [{"type":"text","text":"writing patch"}]
}
```

### `dash tokens`

Per-agent token sample (sparkline accumulates per agent):

```json
{
  "agent_kind": "claude-code",
  "session_id": "cc_abc",
  "cumulative": 84502,
  "today": 21200,
  "latest_sample": 1240
}
```

### `dash idle`

No payload. Switches to the overview scene (wire id `idle` — legacy
alias kept for stress/profile tooling and old NVS `default_scene`
values). Prefer `dash scene` for new code.

### `dash scene` (new in v4)

```
dash scene clock
```

Payload is a raw scene id, not JSON. Switches to any registered scene
(`dashboard` / `idle` / `clock` / `prompt` / `awaiting`); replies
`ERR: unknown scene: <id>` otherwise.

v4.2 scene contract: dashboard and overview are the two mutually
exclusive ambient views — the device BOOT key toggles between them
(`dash scene` / `dash idle` can still reach anything). Snapshots never
move them (the pre-v4 0-agents→idle / N-agents→dashboard auto-switch is
gone). clock is the screensaver: it enters on its own after
`screensaver_min` minutes of no activity — or, v4.7, after the host
link has been silent for `offline_clock_min` minutes (stale agent data
is history, so the device retreats to being a clock; a small red dot
marks the lost link) — leaves on new activity or any
key, and stays reachable manually via `dash scene clock`. The two
takeovers stay state-driven: a prompt (or `agents[].awaiting`) enters
its scene and, on clear, restores whatever the user was viewing.

### `dash config` (new in v1)

Set device-side parameters. Persisted to NVS so they survive reboot.

```json
{
  "device_name": "Clawd",
  "owner": "Felix",
  "theme": "noir",
  "default_scene": "dashboard",
  "screensaver_min": 10,
  "offline_clock_min": 5
}
```

All fields optional; only present fields are updated. Theme values:
`noir` (current default — dark with rust accent), `lab` (light
clinical), `mono` (single-colour minimal).

`screensaver_min` (v4.2): minutes without activity (key press, `dash
prompt`/`dash event`, or a snapshot that actually changes agent state —
keepalives don't count) before the clock scene takes over as a
screensaver. New activity restores the covered view; any key exits.
0 disables. Default 10, clamped to 0-255, persisted to NVS.

`offline_clock_min` (v4.7): minutes without ANY snapshot (keepalives
included) before the device treats the host as gone and retreats to the
clock scene, suppressing a stale awaiting takeover if one is up. The
connection dot (red) stays visible on the clock. Any key exits and buys
another `offline_clock_min` of browsing the stale views. 0 disables.
Default 5, clamped to 0-255, persisted to NVS.

### `dash time` (new in v1)

```json
{
  "epoch_unix": 1779600000,
  "tz_offset_seconds": -25200
}
```

Lets device compute "active N s ago" + "today" rollover precisely.

### `dash health` (new in v1)

Query device status. Device replies with one snapshot of its own
internals:

```json
OK: {
  "device_name": "Clawd",
  "owner": "Felix",
  "scene": "dashboard",
  "uptime_s": 8412,
  "heap_free": 84200,
  "heap_min": 78400,
  "fps": 33.4,
  "battery_pct": 87,
  "snapshots_received": 4612,
  "prompts_received": 14,
  "decisions_sent": 14,
  "connection_age_s": 1820,
  "agent_count": 2
}
```

Bridge calls this every ~5 s for the connection-health indicator and
to populate its own status display.

### `dash push` (v2.7 – v3.0, removed)

The per-tool banner overlay was removed in v3.1: it flashed Read/Edit
noise on every `PostToolUse` while the fleet activity line already
carries the same information. Devices reply `ERR: unknown dash
subcommand` — hosts must not send it.

## Reply tag convention (post G-4 fix)

The `OK:` line for a payload-followed command now embeds the tag
so the host parser doesn't have to know names by convention:

```
OK: payload follows tag=HEALTH
HEALTH_BEGIN fmt=json bytes=420
{...}
HEALTH_END
```

Tags used by dashboard firmware: `HELP` (built-in), `SCENES`
(built-in), `DUMP` (screenshot), `HEALTH` (new in v1).

## EVT lines

| EVT body | Source | When |
|---|---|---|
| `permission id=<id> decision=<once\|deny> [session_id=<id>]` | prompt scene button press / 60s timeout | host bridge correlates to its pending request |
| `scene_changed idx=<n> id=<id>` | every scene_fw_show | bridge can log |
| `agent_added kind=<kind> session_id=<id>` | snapshot adds new agent | bridge can log |
| `agent_removed kind=<kind> session_id=<id>` | snapshot drops an agent | bridge can log |
| `low_heap free=<bytes>` | heap watchdog (v1) | bridge can warn user |

## Wire size

CONSOLE_MAX_LINE = 1024 bytes. v1 snapshots with two agents at full
detail are typically 400-700 bytes. The bridge MUST truncate
`entries[]` (oldest first) if a snapshot serialises >900 bytes.

## Versioning

Bridge sends `{"protocol":"v1"}` in the first command after connect.
Device replies `OK: {"protocol":"v1","compat":["v0","v1"]}` to
indicate it accepts both. Old bridges sending raw v0 still work.
