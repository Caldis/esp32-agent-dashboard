# BLE WiFi provisioning (v1.3.0)

First-boot pairing UX: BLE pushes wifi credentials to the device so
users never edit `.env` again.

## Flow

1. **Out-of-box**: device boots into provisioning mode (no wifi creds
   in NVS). Scene shows "Tap to pair" + QR code with BLE pairing URI.
2. **Pair**: user opens the mobile companion (v1.6.0) or the
   bridge's `claude_buddy_bridge provision --device-id <id>` CLI.
3. **Transfer**: companion sends `dash provision '{"ssid":"...","psk":"...","tz":"..."}'`
   over BLE NUS (TRANS1's transport).
4. **Verify**: device tries to connect; on success writes creds to
   encrypted NVS (SEC1's `nvs_crypto.c`) and reboots into normal mode.
   On failure emits `EVT: provision_failed reason=...` and retries
   pairing.

## Wire commands

- `dash provision <json>` — accepts `ssid`, `psk`, `tz`,
  `device_name`, `owner`. All optional except `ssid` + `psk`.
- `dash provision_status` — replies `OK: {"state":"paired|unpaired|trying|failed", ...}`.
- `dash provision_reset` — wipes wifi creds, reboots into provisioning.

## QR code shape

```
buddy:provision/v1?id=<device_id>&ble_name=Clawd-9F1C8E
```

Mobile companion parses this, derives BLE service UUID, and connects.

## Scaffold status

Spec + `main/provisioning/ble_provision.h` + `provision.c` stubs land
in v1.3.0. Real GATT registration depends on v0.4.0's BLE transport
stack maturing. Mobile-side handled by v1.6.0.
