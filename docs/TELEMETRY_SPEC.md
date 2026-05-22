# Telemetry envelope specification (v0.9.0)

**Default state: OFF.** No data leaves the device or the host bridge
until the user explicitly enables telemetry. This document specifies
exactly what is collected, what is *never* collected, the wire
format, the storage layout, and the deletion recipe.

If anything in this document contradicts the actual behaviour of the
firmware or the bridge, the document is wrong — file an issue.

---

## 1. Goals

We want to know two things, and only two things:

1. **Does the firmware run reliably on real hardware?** — uptime
   distribution, heap headroom, frame-rate health, scene utilisation,
   agent-count distribution.
2. **Does the bridge round-trip stay snappy?** — prompt-latency
   histogram, error rate (`ERR:` / 1000 `OK:` replies).

Anything that doesn't directly answer one of those two questions is
out of scope and must not appear in a telemetry record.

## 2. What's collected (the entire list)

Every field is either a fixed enum, a small integer, or a bucketed
histogram. There are **no free-text fields**.

| Field | Type | Source | Why |
|---|---|---|---|
| `schema` | string literal `"dash.telemetry/v1"` | constant | version pin |
| `ts_unix` | uint32 | host clock at envelope flush | bucketing |
| `device_id` | string (16-hex) | sha256(MAC)[:16] one-way hash | aggregate by device, can't reverse |
| `fw_version` | string (e.g. `"0.9.0"`) | git tag at build time | crash-correlate |
| `idf_version` | string (e.g. `"v6.0.1"`) | `IDF_VER` macro | toolchain skew |
| `uptime_s_min` | uint32 | min over window | reboots show up |
| `uptime_s_max` | uint32 | max over window | longest run |
| `heap_free_min` | uint32 | min over window, bytes | OOM proximity |
| `heap_free_p50` | uint32 | median over window | steady-state |
| `fps_p50` | uint16 | median frame rate | render health |
| `fps_p95` | uint16 | p95 frame rate | jitter |
| `scene_time_pct` | object `{<scene_id>:0–100,...}` | per-scene % of window | usage mix |
| `agent_count_p50` | uint8 | typical concurrent agents | scale signal |
| `agent_count_p95` | uint8 | peak concurrent agents | scale signal |
| `prompt_latency_buckets_ms` | object `{"<50":n,"<100":n,"<250":n,"<500":n,"<1000":n,">=1000":n}` | bridge bench data | snappiness |
| `err_per_1k_ok` | uint16 | bridge counters | error rate |
| `crash_count` | uint8 | count of `crash_dump_available` since last envelope | reliability |

Total envelope size: ~800 bytes worst case. One envelope per 6 hours.

## 3. What's NEVER collected

The collector must refuse to encode any of the following. The
schema validator (see §7) **rejects records that contain a field
outside the §2 table**.

- **No cwd**. Working directories are PII (project names, usernames).
- **No agent message text**. The `msg`, `entries[].text`, and
  `entries[].summary` fields stay on the device.
- **No tool arguments**. The `entries[].tool` *name* is already not
  exfiltrated even at the per-scene aggregate; tool *args* are
  doubly-not-collected.
- **No prompt content**. `prompt.id`, `prompt.tool`, and `prompt.hint`
  never appear in telemetry.
- **No session IDs**. `agents[].session_id` is hashed *then dropped*
  before the envelope is built — we only emit a *count*.
- **No hostnames**. Neither the device's BSSID, nor the host bridge's
  computer name, nor any network identifier.
- **No IP addresses**. The transport may know them (it's TCP); the
  payload does not.
- **No timestamps fine-grained enough to leak working hours**. The
  envelope ts is bucketed to the 6-hour window boundary.
- **No file paths**. Anywhere.
- **No tracebacks or panic messages**. (Those go into the *local-only*
  crash dump — see §6.)

## 4. Envelope shape (canonical example)

One full record, encoded as a single JSON object, ASCII-only,
no trailing newline inside the object:

```json
{
  "schema": "dash.telemetry/v1",
  "ts_unix": 1779638400,
  "device_id": "9f1c8e6b07a34a12",
  "fw_version": "0.9.0",
  "idf_version": "v6.0.1",
  "uptime_s_min": 21600,
  "uptime_s_max": 43200,
  "heap_free_min": 142336,
  "heap_free_p50": 178944,
  "fps_p50": 30,
  "fps_p95": 24,
  "scene_time_pct": {
    "dashboard": 71,
    "sessions": 12,
    "prompt": 4,
    "idle": 13
  },
  "agent_count_p50": 2,
  "agent_count_p95": 3,
  "prompt_latency_buckets_ms": {
    "<50": 4,
    "<100": 18,
    "<250": 211,
    "<500": 34,
    "<1000": 6,
    ">=1000": 1
  },
  "err_per_1k_ok": 2,
  "crash_count": 0
}
```

On the wire this is one line of JSONL terminated by `\n`. Multiple
records concatenated form a valid `.jsonl` file.

## 5. Endpoints

### 5.1 Local (always)

Path: `~/.claude-buddy/telemetry/YYYY-MM.jsonl` (rolling, monthly).

The bridge appends one record per device per 6 hours. The file is
plain JSONL — `tail -f` works, `jq -c` works, Grafana's
JSON-API datasource works (see `tools/grafana/`).

**The local file is written even when `telemetry=off`.** Why: so the
operator can `dash health` their own setup without uploading anything.
The default-off switch only gates the *remote* endpoint.

(Rationale: storing rolling stats on your own disk is not telemetry,
it's logging. Telemetry == "data sent to a third party.")

### 5.2 Remote (opt-in)

The user must run **both** of:

```bash
dash config telemetry=on
dash config telemetry-url=https://your.collector.example/v1/ingest
```

or set them in `~/.claude-buddy/config.toml`:

```toml
[telemetry]
enabled = true
url = "https://your.collector.example/v1/ingest"
```

There is **no default URL** shipped in the firmware or bridge. The
project does not run a collector. The user points at their own
ingestion endpoint, or at a friend's, or at nothing.

Transport: HTTPS POST, body = a single envelope, `Content-Type:
application/json`. Failed POST → record stays in the local file,
retried at the next 6-hour boundary.

## 6. Crash-dump telemetry interaction

Crash dumps are written to NVS and surfaced as `EVT:
crash_dump_available bytes=N` on the next boot. They contain the
panic reason, stack pointer, and the last few `EVT:` lines for
context — i.e. *they may contain useful debug info that we
deliberately keep out of regular telemetry*.

**Therefore crash dumps are local-only.** They never go to the
remote endpoint, ever, regardless of `telemetry=on`. To share a
crash dump, the user has to do it explicitly:

```bash
dash dump crash > crash.json
gh gist create -p crash.json     # or drag-drop to a GitHub gist
```

The only crash-related field in the remote envelope is the
opaque count `crash_count`. Whether the crash was OOM, watchdog,
or a stack overflow is *not* exfiltrated.

## 7. Schema validation

A copy of the JSON schema is checked in at
`tools/grafana/telemetry.schema.json` (planned, post-v0.9.0). Until
that lands, the validator is the §2 table + §3 deny-list as a hard
constraint inside the bridge: any unknown key in an envelope being
about to leave the bridge causes a refuse-to-send.

## 8. "Delete my data" recipe

To wipe **everything locally** (only thing under your control;
remote retention depends on whichever endpoint you opted into):

```bash
# Stop any running bridge first.
dash config telemetry=off

# Remove local rolling stats.
rm -rf ~/.claude-buddy/telemetry/

# Clear on-device crash dumps.
dash dump crash > /dev/null   # one read clears the NVS slot
                              # (firmware behaviour, see crash_dump.c)
```

To wipe data already at a remote endpoint: that's between you and
the endpoint operator. The bridge cannot recall a record that has
already been POSTed; the bridge does not store the records it has
already sent.

## 9. Audit checklist for reviewers

Anyone reviewing a telemetry change must verify:

- [ ] Every new field is in §2.
- [ ] No new field is in §3.
- [ ] Default-off invariant is preserved (`telemetry=off` →
      zero outbound bytes).
- [ ] Envelope still fits in ~800 bytes.
- [ ] Schema version is bumped if any field changed.
- [ ] The §8 recipe still works end-to-end.
