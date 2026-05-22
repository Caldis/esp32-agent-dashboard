# tools/tauri/ — desktop client build prerequisites (v2.2.0 scaffold)

The native desktop client lives in the sibling repo
`Caldis/esp32-agent-dashboard-desktop`. This directory holds the
host-side build prerequisites + the contract between THIS firmware
repo's wire protocol and the desktop's consumer.

## Prereqs

1. Rust toolchain (stable, 1.74+).
2. Tauri 2.x CLI: `cargo install tauri-cli --version "^2"`.
3. Same `emsdk` as v2.1.0 (for the LVGL-WASM bundle).
4. (macOS) Apple Developer ID for code-signing the .app.
5. (Windows) `signtool` from the Windows SDK.

## Build

```bash
# In the sibling repo
git clone https://github.com/Caldis/esp32-agent-dashboard-desktop
cd esp32-agent-dashboard-desktop
# Shares tools/web/build_wasm.sh from this firmware repo
bash ../esp32-agent-dashboard/tools/web/build_wasm.sh \
    --out-dir ./src-tauri/static/
cargo tauri build
```

Output: `.app` (macOS), `.msi` (Windows), `.deb`/`.AppImage` (Linux).

## Wire contract this repo commits to

The desktop app talks to the local bridge over TCP using the
**unchanged** `dash *` verbs. Specifically:

- `dash snapshot`, `dash prompt`, `dash event`, `dash tokens`,
  `dash idle`, `dash config`, `dash time`, `dash health` —
  identical to PROTOCOL.md v1.
- No special "desktop-only" verbs. The desktop client is just
  another consumer of the same wire.

This means v2.2.0 of THIS firmware repo is a "wire-compat keeper"
release — we commit not to break the desktop client without a major
bump.

## Scaffold status

The Tauri repo exists as a stub at this version: README + Cargo.toml
+ tauri.conf.json with the expected capability config. Functional
client lands in v2.2.x.
