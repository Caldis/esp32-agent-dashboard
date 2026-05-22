# Mobile companion (v1.6.0)

Thin iOS / Android app that mirrors the dashboard scenes, sends BLE
provisioning, and lets the user decide-from-phone on permission
prompts.

## Why

Three use-cases:
1. **First-boot provisioning** (v1.3.0) — phone is already on wifi,
   device isn't. Phone is the natural bootstrap pivot.
2. **Across-the-room decisions** — user is in a meeting, device is
   on the desk asking "approve `git push`?" The phone gets a push
   notification, user taps Approve.
3. **Spectator mode** — friends/colleagues can watch the dashboard
   on their phone (read-only, via mDNS) without crowding around the
   physical device.

## Architecture

```
device  <-- BLE NUS -->  phone  <-- HTTPS -->  bridge (laptop)
                          |
                          v
                       APNs / FCM push
```

The phone is the BLE peer (device adverts as Nordic UART). When
backgrounded, the app maintains a low-power BLE listener; the device
sends a BLE notify for `dash prompt`, which translates to an OS-level
push notification.

## Stack choice

- **Tech**: Flutter (single codebase iOS + Android). Native BLE via
  flutter_blue_plus.
- **Bundle size target**: < 8 MB.
- **No backend**: all comms are device ↔ phone direct.

## Repo layout

Mobile app lives in a sibling repo `esp32-agent-dashboard-mobile`
when v1.6.0 actually ships. v1.6.0 of THIS repo just lands the
design doc + the wire-protocol additions the mobile app will use:

- `dash mobile_subscribe <topic>` — phone asks to be pinged on a
  topic (`prompt`, `scene_changed`, `low_heap`, ...).
- `dash mobile_decide id=<req_id> decision=<once|deny>` — phone
  replies to a permission prompt.

## Scaffold status

This version of THIS repo ships the spec + wire additions only.
The mobile app codebase + Flutter build are deferred to a sibling
repo to keep this firmware repo focused.
