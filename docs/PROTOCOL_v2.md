# esp32-agent-dashboard wire protocol — v2 DRAFT

Status: **DRAFT — staged for v0.4.0 cycle**. `PROTOCOL.md` (v1) remains
canonical until v2 ships. The device accepts v1 indefinitely; this doc
just adds new verbs + a negotiation handshake so the bridge can opt in.

What v2 changes:

1. Explicit `dash hello` handshake — transport + version + capabilities
   exchanged in a single round-trip on connect.
2. `dash farewell` for clean disconnect (bridge tells device "I'm going
   away, expect link loss").
3. `dash mtu` — transport-specific buffer hint so the device can size
   its output more efficiently on BLE.
4. Two new `EVT:` lines for transport state (`transport_changed`,
   `link_quality`).
5. Version negotiation **on the first command after connect**, not at
   the framing layer. v1 bridges that don't send `hello` are still
   accepted exactly as before — backwards-compatible by silence.

Everything in `PROTOCOL.md` (v1 verbs, EVT lines, reply tag convention,
wire-size limits) stays unchanged. v2 is additive.

---

## 0. Version negotiation

Old (v1) negotiation:

```
bridge → device: dash snapshot '{"protocol":"v1", "agents":[...]}'
device → bridge: OK: {"protocol":"v1","compat":["v0","v1"]}
```

v1 piggy-backed protocol detection onto the first snapshot. v2
separates the concerns:

```
bridge → device: dash hello '{"transport":"wifi_tls","protocol_version":"v2","capabilities":["LINES","EVT_STREAM","CONFIG_NVS"]}'
device → bridge: OK: {"protocol_version":"v2","compat":["v1","v2"],"transport":"wifi_tls","device_capabilities":["LINES","PAYLOAD_FOLLOWS","EVT_STREAM","BIN_FRAMES","CONFIG_NVS","FAILOVER","MTU_HINT","FARE_WELL"],"capability_bitmap":"0x01DF","device_name":"Clawd","fw":"v0.4.0"}
```

Rules:

- Bridge SHOULD send `dash hello` as its very first command after the
  link is READY. If it doesn't, the device behaves exactly as v1: any
  v1-shape verb is accepted, no capability gating.
- Device replies `OK:` with its own capability set + `compat` array. The
  bridge picks the highest version in `compat` that it also supports.
- `compat` will always include `v1` for the foreseeable future. v0 is
  also accepted by the device parser but is no longer advertised.
- If the bridge sends an unrecognised verb at a v2 capability the
  device didn't advertise, the device replies `ERR: unsupported cap=<X>`.
  The bridge MUST degrade gracefully.

---

## 1. New verbs

### `dash hello` (new in v2)

Handshake. Bridge → device.

```json
{
  "transport": "serial",
  "protocol_version": "v2",
  "capabilities": ["LINES","EVT_STREAM","CONFIG_NVS","MTU_HINT","FARE_WELL"],
  "bridge": "claude-buddy-bridge/v0.4.0",
  "host_os": "Windows_NT 10.0.26100"
}
```

Fields:

| Field | Required | Notes |
|---|---|---|
| `transport` | yes | one of `serial` / `ble_nus` / `wifi_tls` — tells the device which path it's actually on, useful for failover diagnostics |
| `protocol_version` | yes | `v1` or `v2`. Device picks `min(bridge, device)` from compat |
| `capabilities` | yes | string array of capability symbols the bridge supports (see `TRANSPORTS.md` §7) |
| `bridge` | no | free-form bridge identity for logs |
| `host_os` | no | free-form host OS identity for logs |

Device reply:

```
OK: {"protocol_version":"v2","compat":["v1","v2"],"transport":"serial","device_capabilities":["LINES","PAYLOAD_FOLLOWS","EVT_STREAM","BIN_FRAMES","CONFIG_NVS","FAILOVER","MTU_HINT","FARE_WELL"],"capability_bitmap":"0x01DF","device_name":"Clawd","fw":"v0.4.0","negotiated":"v2"}
```

`negotiated` is the version both sides agreed on. If the bridge sent
`v2` but the device only has `v1`, `negotiated` is `v1` and the bridge
must stick to v1 verbs.

### `dash farewell` (new in v2)

Clean disconnect. Bridge → device.

```json
{
  "reason": "host_shutdown"
}
```

Acceptable reasons: `host_shutdown`, `bridge_restart`, `user_quit`,
`failover_starting`, `unknown`. Device replies `OK: bye` and tears
down per-bridge state (clears the snapshot, drops to idle scene after
1 s, stops health polling expectation). The link itself is NOT torn
down — that's the bridge's job (close the socket / disconnect BLE).

If the device gets no `farewell` and the link drops, it shows the
"connection lost" indicator on the status scene after `connection_age_s`
of silence > 30 s. With `farewell`, the indicator is suppressed.

### `dash mtu` (new in v2)

Buffer-size hint. Either side may send it; typically bridge → device on
connect (after `hello`) to inform the device how big the bridge's
receive buffer is, and device → bridge in the `hello` reply if the
transport has a hard MTU (BLE NUS).

Bridge sends:

```json
{
  "bytes": 4096,
  "direction": "host_to_device"
}
```

`direction` ∈ {`host_to_device`, `device_to_host`, `both`}. Default
`both`. The device clamps the value against its own
`CONSOLE_MAX_LINE = 1024` and replies with the effective value:

```
OK: {"bytes":1024,"direction":"both","clamped":true}
```

For BLE NUS the device sends an unsolicited `dash mtu` to the bridge in
the **same line as the hello reply** when MTU < 1024:

```
OK: {"protocol_version":"v2",...,"mtu_bytes":244,"mtu_direction":"both"}
```

so the bridge can immediately truncate its `entries[]` to fit.

---

## 2. New EVT lines

| EVT body | Source | When |
|---|---|---|
| `transport_changed from=<x> to=<y>` | transport failover layer | failover chain switched the active transport (e.g. `from=serial to=ble_nus` when USB unplugged) |
| `link_quality rssi=<N> tx_err=<N> rx_err=<N>` | BLE / WiFi transport | periodic, every 30 s while link is READY. Skipped on serial (no RSSI). `rssi` in dBm (negative). `tx_err` / `rx_err` are cumulative since boot. |
| `pairing_started transport=<x>` | BLE transport | a new BLE pair attempt began (v1.3.0 will hook a scene to this) |
| `pairing_completed transport=<x> bonded=<true\|false>` | BLE transport | pair finished |

All existing v1 EVTs (`permission`, `scene_changed`, `agent_added`,
`agent_removed`, `low_heap`) remain unchanged.

---

## 3. Wire example — full v2 connect sequence

Bridge connects via WiFi+TLS, says hello, pushes a snapshot, then
the link drops and BLE takes over.

```
bridge -> device: dash hello '{"transport":"wifi_tls","protocol_version":"v2","capabilities":["LINES","EVT_STREAM","CONFIG_NVS","MTU_HINT","FARE_WELL","LINK_QUALITY"]}'
device -> bridge: OK: {"protocol_version":"v2","compat":["v1","v2"],"transport":"wifi_tls","device_capabilities":["LINES","PAYLOAD_FOLLOWS","EVT_STREAM","BIN_FRAMES","CONFIG_NVS","LINK_QUALITY","FAILOVER","MTU_HINT","FARE_WELL"],"capability_bitmap":"0x01FF","device_name":"Clawd","fw":"v0.4.0","negotiated":"v2"}

bridge -> device: dash mtu '{"bytes":4096,"direction":"both"}'
device -> bridge: OK: {"bytes":1024,"direction":"both","clamped":true}

bridge -> device: dash snapshot '{"agents":[...], "totals":{...}}'
device -> bridge: OK:

device -> bridge: EVT: link_quality rssi=-58 tx_err=0 rx_err=0
device -> bridge: EVT: link_quality rssi=-61 tx_err=0 rx_err=2
... (link drops) ...
device -> bridge: EVT: transport_changed from=wifi_tls to=ble_nus

bridge -> device: dash hello '{"transport":"ble_nus","protocol_version":"v2","capabilities":["LINES","EVT_STREAM"]}'
device -> bridge: OK: {"protocol_version":"v2","compat":["v1","v2"],"transport":"ble_nus","device_capabilities":[...],"capability_bitmap":"0x01F7","negotiated":"v2","mtu_bytes":244}

bridge -> device: dash farewell '{"reason":"user_quit"}'
device -> bridge: OK: bye
```

The bridge re-sends `dash hello` on every transport change because the
capability set differs per transport (e.g. BLE drops `BIN_FRAMES`).
This is cheap (one round-trip) and keeps the bridge from guessing.

---

## 4. Backward compatibility

- A v1 bridge that never sends `dash hello` works **unchanged**. The
  device behaves as v1 throughout the session.
- A v2 bridge talking to a v1 device gets `ERR: unknown command hello`
  on the first send. The bridge MUST detect this and fall back to v1
  shape (no hello, send snapshots directly).
- The wire grammar (`dash <verb> '<json>'` / `OK:` / `ERR:` / `EVT:`)
  is unchanged. v2 adds verbs only.

The only thing that's strictly v2-only is the
`EVT: transport_changed` / `EVT: link_quality` lines. Pre-v2 bridges
just see them as unrecognised EVTs and (per `PROTOCOL.md` §EVT lines)
"bridge can log" — i.e. they don't crash the parser.

---

## 5. Open questions (for the v0.4.0 review)

1. Should `dash hello` reply include `last_known_snapshot_age_s` so the
   bridge knows whether to replay or just push fresh? Currently the
   bridge always pushes a fresh snapshot post-`hello`.
2. Should `transport_changed` carry the reason (`link_lost`,
   `user_override`, `prefer_higher`)? Probably yes; add as optional
   `reason=` kvp.
3. For multi-device fleet (v1.2.0), should `hello` include a
   `device_id` UUID so the bridge can disambiguate two devices on the
   same LAN with the same `device_name`? Yes — but defer to v1.2.0
   when TRANS1+H3 ship the fleet support.
