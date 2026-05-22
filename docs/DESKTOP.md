# Native desktop client (v2.2.0)

macOS + Windows + Linux tray app that runs the full scene set in a
small floating window — the dashboard without the hardware. Useful
for users who want the dashboard's value but don't have a board on
their desk yet.

## Stack

- **Framework**: Tauri 2.x (Rust + system webview).
- **UI**: same LVGL-WASM bundle as v2.1.0's web mirror, hosted in
  the Tauri webview.
- **Bridge link**: in-process — Tauri's Rust side calls the local
  bridge over the same TCP socket the device uses (so the desktop
  client is just another `dash` consumer).
- **Permissions**: Tauri capability config restricts the app to
  127.0.0.1 + the user's home dir (settings.toml).

## Bundle size target

- macOS / Linux: < 8 MB.
- Windows: < 12 MB (system webview is shared).

## Why not Electron

Tauri is ~10x smaller than Electron for the same functional surface.
The point of "dashboard without hardware" is low friction; a 200 MB
desktop client violates that. Tauri also gets us code-signing parity
with the OTA flow SEC1 already designed (ed25519, same tooling).

## Repo layout

Lives in sibling repo `esp32-agent-dashboard-desktop`:
- `src-tauri/` — Rust side, capability config, bridge link.
- `src/` — TypeScript bootstrap (just the WASM loader).
- Shares `tools/web/build_wasm.sh` with the v2.1.0 mirror.

## Scaffold status

Spec + `tools/tauri/README.md` (build prerequisites) land in v2.2.0.
The actual Tauri app is the sibling repo; v2.2.x of THIS repo just
keeps the wire compatibility stable.
