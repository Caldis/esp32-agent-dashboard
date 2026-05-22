# Power management (v1.7.0)

The device should sleep when nothing's happening, wake instantly on
agent activity, and gracefully degrade when running on battery.

## Sleep policy

| Trigger | Action | Wake source |
|---|---|---|
| Idle scene + 5 min | dim display 50% | any BLE / serial activity |
| Idle scene + 15 min | display off | any BLE / serial activity |
| Idle scene + 30 min | light-sleep (CPU 40MHz) | BLE adv interrupt |
| Idle scene + 60 min | deep-sleep | BLE NUS notify or button press |

Wake-from-deep takes ~600ms; we hide that with a "waking..." LVGL
fade-in. From light-sleep wake is < 80ms — invisible to the user.

## Battery + USB-C PD

Reads battery state via the onboard fuel gauge (board datasheet TBD,
hardware guide will get the part number). `scene_status` shows
charge %, time-to-full / time-to-empty, and PD negotiated voltage
(5V / 9V / 12V / 15V / 20V).

Low-battery thresholds:
- < 20%: warning EVT, dim display 30%
- < 10%: prompt scene auto-defaults to "deny" on timeout (safer)
- < 5%: persist agent state to NVS, force deep-sleep

## Wire commands

- `dash power_set { "policy": "...", "screen_off_after_s": 300 }` —
  user overrides.
- `dash power_status` — current state, battery %, last-wake reason.
- `dash power_sleep_now` — immediate light-sleep (testing).

## Scaffold status

Spec + `main/power/sleep.{h}` + `battery.{h}` land in v1.7.0. Real
sleep state machine + fuel-gauge driver land in v1.7.x once we
identify the exact gauge IC on the Waveshare board.
