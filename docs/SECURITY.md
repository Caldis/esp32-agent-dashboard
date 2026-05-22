# Security posture — esp32-agent-dashboard

> **Audience**: users running the dashboard on their desk, plus
> packagers / corporate users who need to answer "is this safe?"
> before plugging it in. For the engineering rationale and threat
> tree see [`THREAT_MODEL.md`](THREAT_MODEL.md).
>
> **Status as of v0.6.0**: signed OTA + NVS encryption + bridge auth
> are in. WiFi-push TLS lands with v0.4.0's transport layer. Secure
> boot v2 is *recommended* but not enforced — see "what you are
> responsible for".

## What this project protects against

- **OTA supply-chain swap.** Every firmware update must be signed
  with the project's ed25519 private key. A swapped or tampered blob
  is rejected before flashing and the device emits
  `EVT: ota_rejected reason=signature`. The public key is baked into
  the running firmware; trust is pinned at build time, not at
  download time.
- **NVS readout from a stolen device.** WiFi PSK, bridge auth
  token, and any user-configured strings (`device_name`, `owner`)
  are stored in an encrypted NVS partition keyed by a per-device
  random secret that lives in an encrypted `nvs_keys` partition.
  Flash encryption protects that key. An attacker with esptool +
  the bare flash chip sees ciphertext.
- **Host-side process forging device input.** The first command of
  every console session must present a bridge auth token. A rogue
  process on the host that doesn't have the token sees
  `ERR: dash: not authenticated` for any state-changing verb.
- **Bug in a signed firmware that bricks the screen.** Dual-partition
  rollback. If new firmware doesn't call
  `esp_ota_mark_app_valid_cancel_rollback()` within 30 s of boot
  the bootloader reverts to the previous slot on the next reset.
- **MITM on the WiFi push path (v0.4.0+).** TLS with TOFU-pinned
  device certificate, plus HMAC inside the tunnel.

## What this project does NOT protect against

- **A host that is already compromised.** If a process running as
  your user has access to your bridge config, it can read the auth
  token, masquerade as the bridge, and send any `dash` command it
  wants. We can't help with that — defend the host with your normal
  OS-level controls. The dashboard is an output device, not an
  HSM.
- **The very first boot before flash encryption is enabled.** If
  you plug a brand-new board into an untrusted machine and flash it
  there, the secrets written on that boot are recoverable. Do the
  first flash on a machine you trust.
- **Side-channel attacks on the SoC.** Decapping, microprobing,
  laser fault injection — we use commodity silicon, so commodity
  attacks apply.
- **The user shipping their own private key into git.** If you
  commit `tools/sign/private.pem`, every "secure" OTA we sign with
  that key is forgeable by whoever clones your repo. Don't do that
  (the file is `.gitignore`d).
- **Physical denial-of-service.** Holding BOOT + plugging USB lets
  you reflash from scratch. This is a feature for legit owners and
  a vulnerability for paranoid ones; we lean owner-friendly.

## What you (the user) are responsible for

1. **Burn flash encryption + secure boot v2 eFuses on release
   hardware.** The project ships with `sdkconfig.defaults` that
   enable both in release mode. Once the eFuses are burned the
   keys cannot be re-read. Do this on hardware you intend to keep.
   On dev boards you flash twenty times a day, leave it off and
   accept the lower posture — `docs/SECURITY.md` calls this out at
   boot via a one-line console banner `BANNER: dev posture, NVS
   in clear` so you can't forget.
2. **Keep `tools/sign/private.pem` off git and off shared
   machines.** A 32-byte ed25519 private key compromises every
   device that ships with the matching public key. Treat it like
   you'd treat a code-signing cert. The signer script chmods it to
   600 when it writes the file; you still need not to commit it.
3. **Pin the device's TLS cert fingerprint on first connect** (when
   v0.4.0 transport lands). The fingerprint shows on the device
   screen during pairing — compare it visually before the bridge
   accepts the pin. After first pair every subsequent connect is
   automatic and silent.
4. **Don't accept a downgrade.** If your bridge offers to install a
   firmware older than what's running, the device will refuse and
   emit `EVT: ota_rejected reason=rollback`. Soft anti-rollback;
   for hard anti-rollback enable `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`
   (it's on in our defaults) which burns a one-way eFuse counter
   per release.
5. **Wipe the device before resale / disposal.** `dash factory
   reset` (TODO v0.6.1) clears NVS + erases OTA slots + resets
   flash-encryption state in software. Without it the next owner
   inherits your auth token + WiFi PSK.

## Reporting a vulnerability

If you find a security issue, please **do not** open a public GitHub
issue. Email instead:

- **Primary**: `security@<this-project's-domain>` (set up when v1.0.0
  ships; until then use the maintainer's email listed in the GitHub
  org).
- **PGP**: key fingerprint will be published alongside `v1.0.0`.
  Until then plain email is acceptable; please don't paste exploits
  inline if the bug looks remotely-exploitable — send a "I think I
  have something, here's a 1-line summary" first and we'll set up a
  proper channel.

We aim to respond within **7 days** and to ship a fix within **30
days** for high-severity issues, **90 days** otherwise. We will
credit you in the release notes unless you ask us not to.

### Scope

In scope:
- The firmware in `main/`.
- The host bridge in `tools/claude_buddy_bridge.py`.
- The OTA signer in `tools/sign/`.
- The protocol documented in `PROTOCOL.md`.

Out of scope (please report to the upstream project, not us):
- The esp-harness framework in `D:\Code\esp-harness\` (separate
  project, separate disclosure flow).
- ESP-IDF and managed components — report to Espressif.
- LVGL — report to LVGL upstream.

### Safe harbour

We will not pursue legal action against researchers who:
- Test only against their own hardware and their own bridge install.
- Do not exfiltrate data beyond what's needed to demonstrate the
  bug.
- Give us a reasonable window to fix before public disclosure
  (90 days is fine, less is negotiable).

## Cryptographic primitives in use

| Use | Primitive | Library | Notes |
|---|---|---|---|
| OTA signature | ed25519 | bundled `ed25519_verify.c` (verify only) + mbedTLS SHA-512 | One key, baked at build time, host signer uses `cryptography` library |
| NVS encryption | XTS-AES-128 | ESP-IDF `nvs_flash` | Per-device random key in `nvs_keys` partition |
| Flash encryption | XTS-AES-256 | ESP-IDF bootloader | eFuse-burned key, release mode |
| Bridge auth token | 32 random bytes | `esp_random()` | HKDF-SHA-256 for subkeys |
| WiFi push TLS (v0.4.0) | TLS 1.3 ed25519 cert | mbedTLS | TOFU pinning host-side |

## Version history of this document

- v0.6.0 (draft): initial publish, signed OTA + NVS crypto + bridge
  auth.
- v0.4.0 (deferred): TLS section will move from "future" to "shipped".
- v0.7.0 (planned): key rotation + multi-trust-anchor.
- v1.0.0 (planned): final shape of `security@` mailbox + PGP key
  published, scope finalised.
