# Transports — v0.4.0 scaffold (TRANS1)

Status: **DRAFT — scaffold only**. Wire-format v1 stays canonical
(`PROTOCOL.md`). The v2 negotiation lives in `PROTOCOL_v2.md`. Source
files under `main/transport/` are intentionally not in
`main/CMakeLists.txt` — see **Build integration** at the bottom; F2
picks that up during the v0.4.0 cycle.

This doc covers the three transports the device speaks (or will speak)
to its host bridge:

| Transport | State | Used by |
|---|---|---|
| `serial` — USB-Serial JTAG | shipping since v0.1.0 (refactored to share the abstract iface) | every desktop/laptop user with the USB-C tether |
| `ble_nus` — BLE Nordic UART Service | scaffold (GATT registration TODO) | mobile companion app (v1.6.0), wireless desk setups, first-boot provisioning (v1.3.0) |
| `wifi_tls` — TCP + TLS to `dashboard.local:7321` | scaffold (esp_wifi + esp_tls TODO) | "device sits on a different desk" deployments, fleet/wall-of-dashboards (v1.2.0) |

The abstraction goal: **scenes and `dash *` handlers don't know which
transport delivered the byte stream. They just call
`transport_write_line(...)` and the active transport handles framing.**

---

## 1. The `transport_t` interface

See `main/transport/transport.h`. One vtable, three concretes:

```c
typedef struct transport_s transport_t;

struct transport_s {
    const char *name;                         /* "serial" / "ble_nus" / "wifi_tls" */
    transport_kind_t kind;                    /* enum mirror */
    esp_err_t (*open)(transport_t *self);
    esp_err_t (*close)(transport_t *self);
    /* writes one line (NL appended automatically if not present). Honours
     * the transport's MTU — fragments where needed (BLE NUS). */
    esp_err_t (*write_line)(transport_t *self, const char *line, size_t len);
    /* registers an inbound-line callback. Framing layer (line buffer +
     * overflow drain) lives in transport.c and is shared. */
    void (*set_line_callback)(transport_t *self,
                              transport_line_cb_t cb, void *user);
    /* optional — returns the negotiated MTU for write_line. Serial / TCP
     * return SIZE_MAX. BLE NUS returns ~244 after MTU negotiation. */
    size_t (*mtu)(transport_t *self);
    void *priv;                               /* per-impl state */
};
```

`transport_register_active(t)` swaps the global active transport (used
by `console_send_evt` and friends). The framing layer (`transport.c`)
keeps a single shared line buffer and feeds completed lines to the
registered callback. Per-transport `priv` holds connection state.

### Concrete: `transport_serial`

Wraps the existing `harness/console_protocol.h` USB-Serial JTAG path.
**No functional change** — `open()` is a no-op (the harness already
brought it up in `console_protocol_init()`), `write_line()` forwards to
`console_write_raw()`. The point is just to expose the same surface as
BLE/WiFi so callers can be transport-agnostic.

Lifecycle: opened at boot, never closes. `close()` is a no-op for
serial; the device stays receptive even with no host attached (lines
just queue in the JTAG ring buffer).

### Concrete: `transport_ble_nus` — STUB

Nordic UART Service over BLE (NimBLE host stack — ESP-IDF v6.0.1 ships
NimBLE as the default BLE host).

- **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` (Nordic NUS)
- **RX char**: `6E400002-...` — WRITE / WRITE-NO-RESP, host → device
- **TX char**: `6E400003-...` — NOTIFY, device → host
- **MTU**: ATT 247 by default, so usable payload per write/notify is
  **244 bytes** (247 - 3 byte ATT header). `transport_ble_nus.write_line()`
  fragments anything bigger across multiple notifies and re-assembles
  on the host side using `\n` as the frame delimiter (same as serial).
- **Connection lifecycle**: advertise → connect → MTU exchange → bond
  if first time → service-discover → enable notifications on TX char →
  ready. On disconnect: stop notifications, restart advertising.

Pairing UX is **owned by v1.3.0 (U2 + TRANS1)** — the v0.4.0 stub
just-works pairing (`BLE_SM_IO_CAP_NO_IO`), no passkey display. v1.3.0
will move to numeric-comparison + a passkey display scene.

**TODO(v0.4.0 F2)**: `nimble_port_init`, `ble_hs_cfg`, `gatt_svc_register`,
`gap_event_handler`, advertising payload, NUS RX/TX char registration.
File `main/transport/transport_ble_nus.c` has the entry points stubbed
and labelled.

### Concrete: `transport_wifi` — STUB

TCP-over-TLS to a host-side bridge socket on `dashboard.local:7321`
(same port the existing TCP mock listens on).

- **Wire**: identical to serial — line-stream, `\n` delimiters, same
  `dash <verb>` / `OK:` / `ERR:` / `EVT:` grammar. No reframing.
- **TLS**: `esp_tls` with a single pinned CA cert baked in for v0.4.0.
  Cert pinning will move to NVS-stored creds in v0.6.0 (SEC1).
- **Discovery**: device tries mDNS resolution of `dashboard.local`
  first, then falls back to a configurable static IP from NVS.
- **Connection lifecycle**: WiFi-STA up → mDNS resolve → TCP connect →
  TLS handshake → ready. On disconnect / TLS error: exponential
  backoff (1s, 2s, 4s, ..., cap 30s), then re-resolve.

**TODO(v0.4.0 F2)**: `esp_event_loop_create_default`, WiFi STA bring-up,
mDNS resolve, `esp_tls_conn_new`, RX task reading lines into the shared
framer.

---

## 2. Connection lifecycle (state machine, per transport)

All three transports share the same external state machine — only the
"link up" semantics differ:

```
   DOWN ──open()──▶ OPENING ──link-ready──▶ READY ──RX/TX──▶ READY
     ▲                 │                       │
     │                 │ open-fail             │ link-lost
     │                 ▼                       ▼
     └─────────── BACKOFF ◀──────────────── CLOSING
```

- **DOWN**: no resources held, no callback fired.
- **OPENING**: hardware bring-up. Serial = instant; BLE = advertising
  active; WiFi = scanning / associating / TLS handshake.
- **READY**: lines flow both ways. Active transport.
- **BACKOFF**: open() failed, sleeping before retry. Exponential.
- **CLOSING**: client called `close()`, draining writes, then DOWN.

The shared framing layer drops malformed bytes (anything before the
first `\n` after an overflow) so a half-line from a dropped BLE write
doesn't desync the parser. Same drain policy as `console_protocol`'s
overflow drain.

---

## 3. Failover policy

Default chain: **serial → BLE → WiFi**, in that order.

Why this order:

1. **Serial first**: lowest latency (median 186 ms device round-trip
   per `HARNESS_GAPS.md` G-1), no battery cost on host, zero pairing
   friction. If a user has the USB-C tether plugged in, they almost
   certainly want to use it.
2. **BLE second**: untethered but proximity-bound (~10 m). No router
   dependency, works on flaky-WiFi hotel rooms, works during WiFi
   provisioning itself (the bootstrap problem — v1.3.0 ships creds via
   BLE so this transport must come before WiFi in the chain).
3. **WiFi last**: highest latency + most failure modes (router, DHCP,
   DNS, TLS handshake, cert rotation) but the only transport that
   crosses rooms. Acceptable as a fallback or as the user's explicit
   choice via `dash config '{"transport":"wifi_tls"}'`.

A user override always wins: if config pins `transport=wifi_tls`,
serial and BLE are not attempted on boot. The chain is the **default**
when no override is set.

Switching emits `EVT: transport_changed from=<x> to=<y>` (see
`PROTOCOL_v2.md`). The bridge resyncs by replaying its last snapshot.

**Hold-down**: once switched, the new transport must stay READY for at
least 5 s before another switch is considered. Prevents flap if both
links are marginal.

---

## 4. Wire framing differences

| Transport | Framing | MTU | Encoding |
|---|---|---|---|
| `serial` | line-stream, `\n` delimited | unlimited (effective limit is `CONSOLE_MAX_LINE = 1024`) | UTF-8 |
| `ble_nus` | **packetised** — each write/notify is its own ATT frame; lines may span multiple frames | 244 bytes per frame after MTU negotiation | UTF-8 |
| `wifi_tls` | line-stream, `\n` delimited (same as serial) | TLS record limit (~16 KB) | UTF-8 over TLS |

**Key implication**: the BLE path must concatenate received frames
into the shared line buffer before delivering to `set_line_callback`.
On the write side, lines longer than 244 bytes are split across multiple
notifies — the receiver re-joins by waiting for `\n`. The existing
`WIRE_MAX_BYTES = 900` truncation rule (in
`tools/claude_buddy_bridge.py`) still holds; BLE adds fragmentation
**below** that, transparently.

There is **no separate BLE framing** (no length-prefix, no MQTT-SN,
nothing). One transport, one parser. The 244-byte chunks are an
implementation detail of `transport_ble_nus.write_line()` and the
matching host-side reassembler in `tools/transport/ble_smoke.py`.

---

## 5. Pairing UX (BLE) — v0.4.0 stub vs v1.3.0 final

v0.4.0 stub (what TRANS1 ships):
- `BLE_SM_IO_CAP_NO_IO` — just-works pairing, no passkey.
- Device name advertised: `agentdash-<6 hex chars of MAC>`.
- First connection → bond → stored in NimBLE's NVS namespace.
- No UI feedback on the device side (the prompt scene stays as-is).

v1.3.0 (U2 + TRANS1 will rebuild this):
- Numeric-comparison pairing (`BLE_SM_IO_CAP_DISP_YES_NO`).
- A new `scene_pairing` shows the 6-digit code; BOOT button = confirm,
  USER button = reject.
- Bridge / mobile-app side displays the same code; user verifies.
- Bond table viewable + clearable via `dash config '{"ble_unbond":true}'`.

This v0.4.0 scaffold leaves room for the upgrade — the GATT service
registration is callback-based, so v1.3.0 just swaps the IO cap and
adds a pairing-event handler that pushes to the new scene.

---

## 6. mDNS discovery

Once WiFi STA is up the device advertises itself via mDNS so the bridge
can find it without `--port` flags:

```
service type:  _aagentdash._tcp
port:          7321
instance name: <device_name from NVS>   (default "Clawd")
hostname:      <device_name>.local
txt records:
    proto=v1,v2
    fw=v0.4.0-scaffold
    transport=wifi_tls
    agents=2          # current count, updated on snapshot
```

See `main/transport/mdns_discovery.h` for the API surface and
`tools/transport/discover.py` for the host-side resolver.

The choice of `_aagentdash._tcp` (not `_agentdash._tcp`) is deliberate:
`_a` prefix puts us first in alphabetical listings of services on a LAN,
making it easy to spot in tools like `dns-sd -B`. We do not squat
`_agentdash._tcp` in case Apple or Espressif ever standardise it.

---

## 7. Capability bitmap (advertised on hello)

Each transport tells the bridge what it can do, so the bridge doesn't
have to feature-detect by probing. Sent in the `dash hello` reply, as
both a bitmap (for compactness) and a string array (for forward-compat
when bits run out).

| Bit | Symbol | Meaning |
|---|---|---|
| 0 | `LINES` | line-stream framing (always set) |
| 1 | `PAYLOAD_FOLLOWS` | supports `payload follows tag=...` blocks (HEALTH, DUMP, SCENES) |
| 2 | `EVT_STREAM` | unprompted `EVT:` lines (always set in v1+) |
| 3 | `BIN_FRAMES` | supports binary frames (future — screenshot push without base64) |
| 4 | `CONFIG_NVS` | accepts `dash config` and persists to NVS |
| 5 | `LINK_QUALITY` | emits periodic `EVT: link_quality` (BLE / WiFi only) |
| 6 | `FAILOVER` | participates in transport_changed events |
| 7 | `MTU_HINT` | accepts `dash mtu` |
| 8 | `FARE_WELL` | accepts `dash farewell` for clean disconnect |
| 9-15 | reserved | must be zero |

Per-transport defaults (what each transport advertises on hello):

| Transport | Bits set | Hex |
|---|---|---|
| `serial`   | 0,1,2,3,4,6,7,8         | `0x01DF` |
| `ble_nus`  | 0,1,2,4,5,6,7,8         | `0x01F7` (no BIN_FRAMES — too noisy over BLE) |
| `wifi_tls` | 0,1,2,3,4,5,6,7,8       | `0x01FF` |

The bitmap lives in `transport.h` as `TRANSPORT_CAP_*` macros.

---

## Build integration (NOTE FOR F2)

**Intentionally not wired into `main/CMakeLists.txt` yet.** Reason:
the BLE and WiFi paths require new component requires (`bt`, `esp_wifi`,
`esp_tls`, `mdns`) which would force a clean rebuild for everyone, and
the v0.4.0 cycle hasn't started. The TRANS1 scaffold compiles in
isolation (header-only abstractions + .c stubs that don't reference
ESP-IDF outside of `esp_err.h` and `esp_log.h`).

When F2 picks this up:

1. Add to `main/CMakeLists.txt` `SRCS`:
   ```
   "transport/transport.c"
   "transport/transport_serial.c"
   "transport/transport_ble_nus.c"
   "transport/transport_wifi.c"
   "transport/mdns_discovery.c"
   ```
2. Add `"transport"` to `INCLUDE_DIRS`.
3. Add to `REQUIRES`: `bt esp_wifi esp_event esp_netif esp_tls mdns`
   (BLE/WiFi/mDNS are managed components in ESP-IDF v6.0.1 — no
   separate `idf_component.yml` add needed).
4. Behind menuconfig kconfig flags (suggested namespace
   `CONFIG_TRANSPORT_BLE`, `CONFIG_TRANSPORT_WIFI`,
   `CONFIG_TRANSPORT_MDNS`) so users without BLE/WiFi can opt out and
   save flash. The stubs already `#ifdef`-guard their bodies.
5. Wire `transport_init_all()` into `app_main` after
   `console_protocol_init()` but before `agent_commands_register()` —
   so the active transport is set before any handler runs.

---

## Cross-agent notes

- **SEC1**: TLS cert pinning + NVS-stored creds are your territory in
  v0.6.0. The WiFi stub leaves `// TODO(SEC1 v0.6.0): NVS cred load`
  comments where the hard-coded creds currently sit.
- **OBS1**: `EVT: link_quality rssi=N tx_err=N rx_err=N` (v0.9.0) — the
  transport layer maintains the counters; OBS1 owns the emit cadence
  and the Grafana panel.
- **F2**: build integration above. Plus the actual NimBLE GATT
  registration and esp_wifi event handlers. Stubs label every TODO with
  `// TODO(v0.4.0 F2): ...`.
- **H4 (future)**: the bridge-side replay pipeline (v1.5.0) will need to
  know which transport sourced each line for accurate reconstruction.
  The host-side `discover.py` records `transport` in its JSON output;
  H4 should plumb that through.
