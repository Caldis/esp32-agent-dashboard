# Web dashboard mirror (v2.1.0)

The same five scenes that render on the device, mirrored in a browser.
Useful when the user is working remote (their device is on a desk
they're not at) or when they don't yet own a device but want to see
what the dashboard looks like with their real CC session.

## Architecture

```
bridge (laptop) ──┐
                  ├──> HTTP /scene/<id>           — current scene JSON
                  ├──> WebSocket /events          — push EVT + snapshot
                  └──> /static/* (lvgl-wasm bundle)

browser ──> /static/index.html ──┐
                                  ├──> instantiates lvgl-wasm
                                  ├──> connects /events WS
                                  └──> renders the same scenes
```

The browser runs the **exact same LVGL scene code** compiled to WASM
(via `emscripten`). No re-implementation of UI in JS. Snapshot
payloads from the bridge feed `agent_state.h` over a thin C shim;
LVGL paints into a canvas.

## Compatibility

This is read-only mirror. The browser cannot send `dash` commands
(no button taps from the web). For decisions, the user still uses
the device, the mobile companion (v1.6.0), or the bridge's CLI.

## Tooling

- `tools/web/serve.py` — `python tools/web/serve.py --bridge 127.0.0.1:7321`
  starts an HTTP+WS server bound to a local bridge.
- `tools/web/build_wasm.sh` — `emcmake` invocation that builds the
  LVGL-wasm bundle from `main/scenes/*.c` + `tools/web/lvgl_shim.c`.
  Output: `tools/web/static/lvgl.wasm` + companion JS loader.

## Scaffold status

Spec + `tools/web/{serve.py,build_wasm.sh}` stubs ship in v2.1.0.
Building the WASM bundle requires `emsdk` installed; the stub
documents the host setup. Real working mirror lands in v2.1.x.
