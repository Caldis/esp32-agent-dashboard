# Threat model — esp32-agent-dashboard v0.6.0

> **Status:** draft, staged for v0.6.0. Owner: SEC1.
>
> This document is meant to be concrete: every adversary listed below
> has a real attack path against the *current* (pre-v0.6.0) device,
> a mitigation we are landing, an estimated **severity** (CVSS-ish:
> low / med / high / critical), and the **residual risk** that the
> mitigation does NOT cover. If you read an entry and think "they
> forgot X", file a PR on this file — that's the point.

## Why

The dashboard sits next to a developer's keyboard, holds an LLM-host
↔ device transport that signs no traffic, persists WiFi credentials in
plain NVS, and is going to gain WiFi push (v0.4.0) and OTA (this
version). The attack surface is small but real, and getting larger.
We document the model **before** writing mitigations so the
mitigations target real threats, not "industry best practice"
checklists.

## Assets we protect

| Asset | Why it matters | Where it lives |
|---|---|---|
| WiFi SSID + PSK | Pivot into the user's LAN | NVS namespace `dashcfg`, key `wifi_psk` (v0.4.0+) |
| Bridge↔device shared secret (v0.4.0+) | Forging push events | NVS namespace `dashcfg`, key `bridge_token` |
| OTA public key | Anchor for every future firmware update | Baked into firmware image (`main/secure/ota_pubkey.h`) |
| Running firmware | Owns the screen + buttons + network | OTA slot, verified at boot via secure boot v2 (out of scope here) |
| Prompt-decision integrity | A spoofed "deny" loses a tool call; a spoofed "approve" runs `rm -rf` | In flight on USB/WiFi |

## Assets we do NOT protect (and why)

- **Snapshot contents over USB-Serial.** The device is physically
  attached to the same machine that produces the snapshots. If the
  attacker has the host, they already have the snapshot.
- **Side-channel resistance of the SoC.** A nation-state with
  decap-and-microprobe gear gets the eFuses regardless. Out of scope.
- **DoS by physical access.** Holding the BOOT button + reflashing
  factory firmware is a feature, not a bug. We don't try to brick
  a device when its owner wants to wipe it.

## Adversaries

### A1 — Shoulder-surfer who steals the device + reads NVS

**Capability**: physical possession of the device for 30+ minutes,
USB cable, esptool / OpenOCD, an ESP32-S3 dev host.

**Attack path (today)**:
1. Plug device into laptop.
2. `esptool.py read_flash 0x9000 0x6000 nvs.bin` — pulls the whole
   `nvs` partition out as clear bytes.
3. `nvs_partition_gen.py` or just `strings nvs.bin | grep -i ssid`.
4. Learns the user's home WiFi PSK (when v0.4.0 lands) and any other
   strings we've persisted (`device_name`, `owner` — minor PII).

**Mitigation (v0.6.0)**:
- Enable **NVS encryption** (`CONFIG_NVS_ENCRYPTION=y`) with the
  encryption key sourced from an `nvs_keys` partition encrypted by
  eFuse-burned **flash encryption** (`CONFIG_SECURE_FLASH_ENC_ENABLED=y`,
  release mode). On first boot, `nvs_crypto_init()` generates a fresh
  random NVS key and writes it to the `nvs_keys` partition (one-time).
- Use the `nvs_crypto_*` wrappers (`main/secure/nvs_crypto.[ch]`) for
  every read/write of the `dashcfg` namespace so we cannot accidentally
  leave a value in the clear partition.

**Severity**: HIGH (PSK leak == LAN pivot, harvest-now-decrypt-later
on captured traffic).

**Residual risk**:
- Pre-eFuse-burn boot is unencrypted. The very first firmware
  flash + boot must happen on a trusted machine. Document this as
  a one-line warning in `docs/SECURITY.md`.
- If flash encryption is in *development* mode the key is
  reproducible from eFuses and an attacker with physical access can
  decrypt. We require **release** mode for v0.6.0 release builds
  (see `## Build integration` below).
- An attacker who keeps the device for hours can attempt fault
  injection on the SoC. Mitigation is a hardware-only problem.

### A2 — Rogue process on the host that forges `dash` commands over USB

**Capability**: runs unprivileged on the same OS user as the legit
bridge (Claude Code, Codex, malicious npm postinstall, etc.).

**Attack path (today)**:
1. Enumerate serial ports — COM9 / `/dev/cu.usbserial-*`.
2. Open at 115200 baud.
3. Write `dash prompt "{\"id\":\"req_evil\",\"tool\":\"Bash\",`
   `\"hint\":\"approve any rm -rf?\"}"`.
4. Watch for `EVT: permission id=req_evil decision=once` and use it
   to silently launder dangerous tool approvals to the *real* bridge,
   which is listening on the same wire and will see the EVT.

**Mitigation (v0.6.0, partial)**:
- The bridge token (32 random bytes, generated on first run, stored
  encrypted in `dashcfg`) is required on the first command of every
  session: `dash auth "{\"token\":\"<hex>\"}"`. Until auth succeeds
  the device returns `ERR: dash: not authenticated` for any verb that
  changes state (snapshot/prompt/event/config). Read-only verbs
  (`?help`, `?stat`, `dash health`) stay open.
- The token is delivered to the bridge once, at provisioning time,
  via a one-shot `dash auth provision` flow that prints the new
  token to the device screen so the user can type it into the bridge
  config. This avoids ever putting the token in a config file the
  rogue process can read.

**Severity**: HIGH (silent permission spoof = arbitrary code exec).

**Residual risk**:
- USB itself is unauthenticated transport. A malicious USB device
  could replay an entire authenticated session if it captures one.
  Mitigation: per-command nonce (rolling 32-bit counter, rejected if
  it doesn't strictly increase). Punted to v0.6.1; flagged.
- A process running as the *same* OS user as the bridge can hijack
  the bridge's IPC and bypass all of this. The device cannot defend
  against host compromise; this is documented in `docs/SECURITY.md`
  under "what the user is responsible for".

### A3 — MITM on the WiFi push path (lands with v0.4.0)

**Capability**: on-path on the user's LAN (compromised router,
malicious ESP32 doing ARP spoof + Karma-style AP, a curious dorm
roommate with `bettercap`).

**Attack path (today, hypothetical v0.4.0 without v0.6.0)**:
1. WiFi push is documented as "TCP socket on port 47900, JSON lines."
2. MITM intercepts the socket, alters `dash snapshot` payloads
   (insert fake "permission granted" hint), or replays an old prompt.
3. Worse: MITM injects `dash config '{"theme":"noir"}'` then a
   `dash ota` (if the OTA URL is HTTP) and serves their own firmware
   blob — see A4.

**Mitigation (v0.6.0)**:
- Require **TLS** on the WiFi transport: device acts as TLS server,
  cert is generated on first boot and pinned by the bridge (TOFU,
  hash in `~/.claude_buddy_bridge/device_cert_pins.json`).
- Same bridge-token auth as A2 runs inside the TLS tunnel, so even
  a stolen cert can't get past auth.
- Snapshot envelopes carry an HMAC (key derived from the bridge
  token via HKDF) so even if the TLS tunnel is somehow broken the
  payload itself is integrity-checked.

**Severity**: HIGH (when v0.4.0 ships without this).

**Residual risk**:
- TOFU is TOFU — the very first pin is unauthenticated. We display
  the cert fingerprint on the device screen so the user can compare
  with the bridge's prompt. Documented as a deliberate trade-off.
- TLS 1.3 with a 2048-bit RSA cert is heavy on the S3. We use
  ed25519 server keys (mbedTLS in IDF v6 supports this via
  `MBEDTLS_KEY_EXCHANGE_*PSK*` or via raw ed25519 cert chains) to
  keep the handshake under 200 ms. If that turns out to break v0.4.0
  perf we fall back to PSK-TLS.

### A4 — Supply-chain: a compromised firmware OTA blob

**Capability**: attacker controls the OTA distribution channel
(could be the github release CDN, could be a MITM on the OTA HTTP
URL, could be a malicious maintainer of a managed-component that
ships its own update mechanism).

**Attack path (today, no OTA yet)**:
1. User opts in to OTA.
2. Device fetches `https://dashboard.example/firmware.bin` over
   plain HTTPS, trusts whatever cert it gets (or, worse, plain HTTP).
3. Attacker swaps the bin for one with `xxd` + the same size, OR
   sniffs the HTTPS via a corp MITM proxy and re-signs with their CA.
4. Device flashes, reboots, attacker now owns the screen + button +
   any future credentials (it's a persistent rootkit on a developer's
   desk).

**Mitigation (v0.6.0)**:
- Every OTA payload carries an **ed25519 signature over the binary**.
  The public key is baked into the firmware image at build time
  (`main/secure/ota_pubkey.h`, regenerated by `tools/sign/generate_keys.py`).
- `main/secure/ota_verify.[ch]` validates the signature **before**
  `esp_ota_set_boot_partition()` is called. A failed verification
  emits `EVT: ota_rejected reason=signature` and stays on the
  running slot.
- The on-wire OTA frame is `magic(4="DASH") | version(2 LE) |
  size(4 LE) | sig(64) | firmware(size)`. The device computes
  `sha512` (mbedTLS) of `magic||version||size||firmware` and feeds
  that to ed25519 verify with the embedded public key.
- The running version is stored in `esp_app_desc.version` and the
  device refuses to install a payload whose `version` field is
  numerically lower than the current running version (anti-rollback,
  soft). For *hard* anti-rollback (eFuse-burned, irreversible) we
  defer to ESP-IDF's anti-rollback feature, opt-in only — release
  builds get it, dev builds don't, to keep dev pivots possible.
- Dual-partition rollback: standard A/B OTA. New firmware boots
  into a probationary state; if it doesn't call
  `esp_ota_mark_app_valid_cancel_rollback()` within 30 s of boot the
  bootloader reverts to the previous slot on next reset.
  Rationale: a signed-but-buggy firmware is still a brick if the
  developer panics; the rollback gives them a way out.

**Severity**: CRITICAL.

**Residual risk**:
- We bake exactly **one** public key. If the private key leaks,
  every device in the field is compromised and the only fix is
  re-flashing via USB. Mitigation candidates for v0.7.0:
  multiple trusted keys + revocation list, or key-pinning by hash
  with a key-rotation transaction signed by the old key.
- The host-side signer (`tools/sign/sign_firmware.py`) reads the
  private key from disk. If a developer commits `private.pem` to
  git, the model collapses. We mitigate with: `generate_keys.py`
  chmod 600 on the file, a `.gitignore` that drops `*.pem` in
  `tools/sign/`, and a one-line warning in `docs/SECURITY.md`.
- If `MBEDTLS_KEY_EXCHANGE_*` is misconfigured by F2's CMake
  integration the SHA-512 backend used by `ota_verify.c` might not
  exist. The C side compiles only if `mbedtls/sha512.h` is reachable.
  We assert this at build time via `#if !defined(MBEDTLS_SHA512_C)`.

## Build integration

> This section is a note **for F2** when they wire v0.6.0 into the
> actual build. Do not modify `CMakeLists.txt` from this branch.

F2 must, at integration time:

1. Append to `main/CMakeLists.txt`:
   ```cmake
   list(APPEND SRCS
       "secure/nvs_crypto.c"
       "secure/ota_verify.c"
       "secure/ed25519_verify.c"
   )
   list(APPEND PRIV_REQUIRES mbedtls app_update spi_flash esp_partition)
   ```
2. Append to `partitions.csv`:
   ```
   nvs_keys, data, nvs_keys, 0x10000, 0x1000, encrypted
   ota_0,    app,  ota_0,    0x20000, 3M
   ota_1,    app,  ota_1,    ,        3M
   otadata,  data, ota,      ,        0x2000
   ```
   (drop the `factory` row; or keep factory + relocate ota partitions
   higher — F2's call.)
3. Append to `sdkconfig.defaults`:
   ```
   CONFIG_NVS_ENCRYPTION=y
   CONFIG_SECURE_FLASH_ENC_ENABLED=y
   CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
   CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y
   CONFIG_MBEDTLS_SHA512_C=y
   CONFIG_MBEDTLS_HARDWARE_SHA=y
   ```
4. In `esp32_agent_dashboard_main.c::app_main()`, call
   `nvs_crypto_init()` **immediately after** the existing
   `nvs_flash_init()` block, before any `nvs_open`. The
   `agent_commands_load_config()` call must use the new wrappers
   (`nvs_crypto_set_str` / `nvs_crypto_get_str`) for the `dashcfg`
   namespace.
5. Run `python tools/sign/generate_keys.py` once at integration
   time. Commit `main/secure/ota_pubkey.h` (the public key header
   is fine to commit; the build needs it). DO NOT commit
   `tools/sign/private.pem` — `.gitignore` it.

If any of those steps are skipped the build still compiles (the
secure dir has its own CMakeLists.txt fallback note), but the
v0.6.0 security guarantees do not hold and the device falls back
to v0.5.0-equivalent posture.

## Out of scope for v0.6.0

- Hardware secure-boot v2 chain (eFuse-burned bootloader signature).
  Strongly recommended but covered by ESP-IDF docs; we don't add
  anything project-specific. Add a one-line pointer in `SECURITY.md`.
- Anti-glitch / fault-injection countermeasures. Hardware problem.
- Cryptographic agility (algorithm negotiation). We commit to
  ed25519 + sha512 for v0.6.0. A future version can negotiate.
- Encrypted snapshot bodies. The bridge↔device shared secret +
  TLS is enough; encrypting the body twice is theatre.

## How a deviation gets back into this doc

If, during integration, F2 (or anyone) cannot honour one of the
mitigations above, they MUST add a row to `## Open mitigations
deferred` with:
- which adversary it weakens against;
- new severity (post-deferral);
- target version to close the gap.

## Open mitigations deferred

| Adversary | Mitigation deferred | New severity | Target |
|---|---|---|---|
| A2 (rogue host process) | Per-command nonce | MED | v0.6.1 |
| A4 (OTA supply chain) | Multi-key + revocation | MED | v0.7.0 |
