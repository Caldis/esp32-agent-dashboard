# Multi-device fleet (v1.2.0)

One bridge ↔ N devices. The "wall of dashboards" use case.

## Motivation

A power user with several agents (Claude Code in two terminals, Codex
in a third, an experimental cursor session) may want one device per
agent — each its own AMOLED panel — instead of N agents crammed onto
one 466×466 screen. v1.2.0 makes the bridge a hub that fans events
to multiple devices, with deterministic routing.

## Wire protocol changes

`dash hello` reply gains a `device_id` field (the ed25519 public-key
fingerprint baked at sign-time, see SEC1's `main/secure/ota_pubkey.h`).
Bridge tracks `SessionRegistry → device_id` mapping via the user's
`~/.claude-buddy/config.toml`:

```toml
[fleet.device.clawd-1]
serial_port = "COM9"
agents      = ["claude-code"]      # only push these kinds

[fleet.device.clawd-2]
port_kind   = "tcp"
port        = "192.168.1.42:7321"
agents      = ["codex", "cursor"]
```

## Discovery

`tools/fleet/discover.py` enumerates devices via:
1. local COM ports speaking the dash protocol (existing
   `esp_harness.core.ports`)
2. `_aagentdash._tcp` mDNS browse (TRANS1's `tools/transport/discover.py`)
3. cached `~/.claude-buddy/known_devices.json` for offline boot

Returns a list of `{device_id, transport, address, last_seen_unix}`.

## Routing

The bridge's `SessionRegistry` is extended with a per-agent
`target_device_id` field. Snapshot pushes filter by `agents` allow-list
per device. If no match, push goes to the default fallback device
(first registered, or `agents = ["*"]`).

## Failure modes

- One device offline: bridge buffers its targeted events (bounded
  queue, drops oldest on overflow) and replays on reconnect. Other
  devices continue normally.
- Two devices claim same `device_id` (cloned firmware): bridge emits
  `EVT: fleet_conflict device_id=...` and refuses to push to either
  until user resolves via `dash config '{"force_device_id":"..."}'`.

## Scaffold status (this cycle)

Spec + `tools/fleet/discover.py` stub land in v1.2.0. Routing
implementation in the bridge lands in v1.2.1 once we have two real
devices on the bench. The wire protocol is forward-compatible — old
single-device bridges keep working against v1.2.0 devices.
