# OTA design — esp32-agent-dashboard

> **Status:** v0.6.0 draft. Owner: SEC1 (signature + verify),
> F2 (partition layout + apply wiring).
>
> Goal: ship firmware updates over WiFi without ever trusting the
> transport. The signature is the trust root; the transport is just
> a byte pipe. A malicious CDN, a corporate MITM proxy, even an
> attacker who owns the user's router cannot push a payload the
> device will install.

## Why

The dashboard sits on a developer's desk forever, and "forever" is
exactly how long an attacker needs to ship a malicious update.
Plain HTTPS is not enough — corporate MITM is a real shipping
condition. Hence: ed25519 signature over the firmware bytes, public
key baked into the running image, refuse to apply if the signature
doesn't match. Dual-partition rollback so a signed-but-broken build
is recoverable. NVS-stored *version* for soft anti-rollback before
the eFuse counter advances.

## Threat model link

See [`THREAT_MODEL.md`](THREAT_MODEL.md) §A4 for the full attacker
model. This document is the design — that one is the *why we built
it this way*.

## Wire format

A signed firmware payload is laid out as:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     magic = "DASH" (0x44 0x41 0x53 0x48), 4 bytes             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   version (u16 LE)            |        size (u32 LE)          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
|        size (continued)       |                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
|                       ed25519 sig (64 bytes)                  |
+                                                               +
|                                                               |
+                                                               +
|                                                               |
+                                                               +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                  firmware bytes (size bytes)                  |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Total wire size: `4 + 2 + 4 + 64 + size = 74 + size` bytes.

Field semantics:

| Offset | Field   | Width | Endian | Notes |
|---|---|---|---|---|
| 0  | `magic`     | 4 B   | —      | ASCII `"DASH"`. Wrong magic = `EVT: ota_rejected reason=magic`. |
| 4  | `version`   | 2 B   | LE     | Encoded as `(major << 12) \| (minor << 6) \| patch`. v0.6.0 → `0x0186`. Must be **strictly greater** than the running image's encoded version. |
| 6  | `size`      | 4 B   | LE     | Length of the trailing firmware blob. Must be ≤ partition size – 1 KB (we leave room for the app descriptor at the end). |
| 10 | `signature` | 64 B  | —      | ed25519 signature over `magic \|\| version_le \|\| size_le \|\| firmware`. NOT over the signature bytes themselves (would be circular). |
| 74 | `firmware`  | N B   | —      | The raw ESP-IDF app image (the bytes that, pre-OTA, would have gone to `factory` slot). |

The signature is over `magic || version_le || size_le || firmware`
specifically — including the magic + version + size means an attacker
can't lift a signature from one payload and stick a different
firmware after it (the size mismatch invalidates the signature), and
can't downgrade the version field without invalidating the signature
either.

## Version-roll rules

Versions are encoded as a single u16 little-endian on the wire and
internally compared as that integer:

```
encoded = (major << 12) | (minor << 6) | patch
```

with `major ∈ [0..15]`, `minor ∈ [0..63]`, `patch ∈ [0..63]`. That
gives us 15 majors / 63 minors / 63 patches, more than enough.

Examples:

| Semantic | Encoded (u16) | Hex |
|---|---|---|
| v0.5.0 | `(0<<12)\|(5<<6)\|0` = 320 | `0x0140` |
| v0.6.0 | `(0<<12)\|(6<<6)\|0` = 384 | `0x0180` |
| v0.6.1 | `(0<<12)\|(6<<6)\|1` = 385 | `0x0181` |
| v1.0.0 | `(1<<12)\|(0<<6)\|0` = 4096 | `0x1000` |
| v2.2.0 | `(2<<12)\|(2<<6)\|0` = 8320 | `0x2080` |

Note: v0.6.0 encodes to **0x0180**, not `0x0186` as the diagram
example suggested above (that example was illustrative). The signer
computes this from the semver string at sign time.

**Rules enforced by the device:**

1. **Strict-increase**. The payload's `version` MUST be > the
   running image's encoded version. Equal = reject
   (`EVT: ota_rejected reason=rollback`). Lower = reject.
2. **One-major-at-a-time**. We refuse a payload whose `(version >> 12)`
   is more than one greater than the running image's major. You can
   go v0.6.0 → v1.0.0, but not v0.6.0 → v2.0.0 directly — that
   forces stepwise migration of any breaking config.
3. **Anti-replay via NVS**. After every successful flash we write
   the new encoded version to `nvs_crypto` namespace `ota`, key
   `last_version`. On boot we check the running image against that
   key — if the running image is older, we refuse to mark the slot
   valid (the bootloader will roll us back on next reset).
4. **Hard anti-rollback eFuse counter** (when
   `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y`) bumps with every
   accepted *major*. Patches and minors don't burn eFuses (we only
   have 16 anti-rollback counters in eFuses on the S3).

## Flow

```
                ┌────────────────────────────────────┐
                │   tools/sign/sign_firmware.py      │
                │   reads private.pem + firmware.bin │
                │   writes firmware.signed           │
                └──────────────┬─────────────────────┘
                               │
                               │  HTTP(S) / WiFi / SD-card
                               ▼
        ┌──────────────────────────────────────────────────┐
        │  bridge: POST /ota with body = firmware.signed   │
        └──────────────────────┬───────────────────────────┘
                               │  console line:
                               │  dash ota "{\"len\":N}"
                               │  followed by N bytes
                               ▼
        ┌──────────────────────────────────────────────────┐
        │  on-device                                       │
        │  1. parse header, validate magic + size          │
        │  2. esp_ota_begin on inactive slot               │
        │  3. stream `firmware` bytes into the slot        │
        │  4. on EOF, fetch full bytes back (esp_partition │
        │     _read) for ed25519 verify                    │
        │  5. ota_verify_payload(...) → mbedtls SHA-512 →  │
        │     ed25519_verify(sig, hash, pubkey)            │
        │  6. on OK: esp_ota_end + esp_ota_set_boot_       │
        │     partition + reboot                           │
        │     on FAIL: esp_ota_abort + emit                │
        │     EVT: ota_rejected reason=signature           │
        │  7. after reboot, run 30 s smoke;                │
        │     esp_ota_mark_app_valid_cancel_rollback()     │
        └──────────────────────────────────────────────────┘
```

Why hash-then-verify rather than streaming verify: ed25519 requires
the whole message to compute the challenge `r`. We can't verify
incrementally without breaking the algorithm. The flash slot
doubles as our "buffered the whole payload" storage so we don't
need 6 MB of RAM. Read-back after streaming is the price.

## Public key handling

- **Generated once** by `tools/sign/generate_keys.py`, output:
  - `tools/sign/private.pem` — PEM-encoded ed25519 private key.
    Chmodded 600. Never commit. `.gitignore`'d.
  - `main/secure/ota_pubkey.h` — C header with `static const
    uint8_t ota_pubkey[32] = { ... };`. Commit this.
- **Baked at build time** — `ota_verify.c` `#include`s
  `ota_pubkey.h`, the verifier reads the constant directly.
- **Rotation** for v0.7.0: a new payload type `magic="DASR"`
  (rotation) carries a new pubkey signed by the old one, plus the
  new pubkey itself. Out of scope here.

## NVS-stored auxiliary state

| Namespace | Key            | Value | Purpose |
|---|---|---|---|
| `ota`     | `last_version` | u16   | Encoded version of last successfully applied OTA. Used for soft anti-rollback. |
| `ota`     | `pending`      | bool  | Set when an OTA was just applied; cleared after the 30 s smoke succeeds. If still set on next boot we know we crashed mid-validation and the bootloader's hard rollback will trigger. |
| `ota`     | `attempts`     | u8    | Number of times this slot has been tried. Capped at 3; after that we refuse to retry and force a USB reflash to clear. |

All three live in the encrypted `dashcfg`-adjacent namespace via
`nvs_crypto_*` — same protection as the rest.

## Console verbs (host → device, v0.6.0)

### `dash ota begin`

```json
{ "size": 4321234, "version": 384 }
```

Reply `OK: ota begin slot=ota_1` or `ERR: ota begin: bad version`.
Subsequent raw bytes (NOT JSON, NOT line-framed — direct binary)
are read from the console until `size` is reached. Console
protocol enters raw mode for that duration.

### `dash ota verify`

No payload. Triggers the hash-then-verify step. Reply
`OK: ota verify` or `ERR: ota verify: signature` (plus the EVT).

### `dash ota commit`

No payload. Calls `esp_ota_set_boot_partition` and reboots. Only
valid after a successful `dash ota verify`.

### `dash ota abort`

No payload. Cancels an in-progress OTA, frees the slot.

### `dash ota info`

```json
OK: {
  "running": "ota_0",
  "running_version": 384,
  "running_state": "valid",
  "next": "ota_1",
  "last_attempted_version": 0,
  "pubkey_fingerprint": "5f0a..."
}
```

The fingerprint is sha256(pubkey)[:8] so the user can sanity-check
which key is baked in without exposing the key.

## EVT lines

| EVT body | When |
|---|---|
| `ota_begin slot=ota_1 size=N version=V` | After `dash ota begin` accepted |
| `ota_progress pct=N` | Every 5 % during streaming |
| `ota_verifying` | After `dash ota verify` start |
| `ota_rejected reason=<magic\|size\|signature\|rollback\|major_skip>` | Any failure |
| `ota_committed slot=ota_1 version=V` | After `dash ota commit`, just before reboot |
| `ota_validated` | After 30 s smoke calls `esp_ota_mark_app_valid_cancel_rollback` |

## What the host bridge sends (sketch)

```python
# pseudo
hdr = open("firmware.signed", "rb").read()
size = struct.unpack_from("<I", hdr, 6)[0]
version = struct.unpack_from("<H", hdr, 4)[0]
firmware = hdr[74:74 + size]
session.send(f'dash ota begin "{{\\"size\\":{size},\\"version\\":{version}}}"')
session.read_ok("ota begin")
session.send_raw(hdr[:74])           # header (incl signature) goes verbatim
session.send_raw(firmware)           # then firmware bytes
session.send("dash ota verify")
session.read_ok("ota verify")        # may take ~3 s on S3 for sha512 + verify
session.send("dash ota commit")
# device reboots; reconnect, expect EVT: ota_validated within 30 s
```

## Performance budget

Measured / estimated on ESP32-S3 @ 240 MHz with mbedTLS SHA-512:

| Step | Time |
|---|---|
| Stream 3 MB to flash | ~6 s @ 500 KB/s console throughput |
| Read 3 MB back from flash | ~1 s |
| SHA-512 over 3 MB | ~250 ms (HW accelerated via `CONFIG_MBEDTLS_HARDWARE_SHA=y`) |
| ed25519 verify (one scalarmult) | ~80 ms (pure SW; no HW ECC on S3) |
| Apply + reboot | <100 ms |
| Total wall clock | ~7.5 s |

If verify takes >5 s in practice we'll switch the ed25519 verify
core from the bundled portable C to the optimised Donna variant.
Tracked as a v0.6.1 perf task, not a correctness one.

## File index

| Path | Owner | Purpose |
|---|---|---|
| `main/secure/ota_verify.h` / `.c` | SEC1 | Header parse + hash + verify orchestration |
| `main/secure/ed25519_verify.c` | SEC1 | Bundled portable ed25519 verify (since IDF mbedtls v6.0.1 lacks `mbedtls/ed25519.h`) |
| `main/secure/ota_pubkey.h` | SEC1 (generated) | Baked public key |
| `main/secure/nvs_crypto.h` / `.c` | SEC1 | Encrypted-NVS wrapper |
| `tools/sign/sign_firmware.py` | SEC1 | Host signer |
| `tools/sign/generate_keys.py` | SEC1 | Keypair + header generator |
| `main/esp32_agent_dashboard_main.c::app_main` | F2 | Init wiring — see THREAT_MODEL.md §Build integration |
| `main/CMakeLists.txt` | F2 | Add secure/ sources + mbedtls + app_update requires |
| `partitions.csv` | F2 | ota_0 / ota_1 / otadata / nvs_keys |
| `sdkconfig.defaults` | F2 | Flash encryption + NVS encryption + anti-rollback kconfig |
