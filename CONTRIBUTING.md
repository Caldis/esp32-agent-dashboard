# Contributing to esp32-agent-dashboard

Thanks for considering a contribution. This doc covers the practical
mechanics; the **why** behind the project lives in
[`README.md`](./README.md) and the wire-format design in
[`PROTOCOL.md`](./PROTOCOL.md).

This project is a **consumer** of [esp-harness](https://github.com/Caldis/esp-harness) — the framework that
provides the structured console protocol, LVGL scene framework, and
host CLI. If your contribution is really an esp-harness improvement
(a missing toolkit command, a console-protocol gap), file it there
and reference it here.

## Before you start

Please skim, in this order:

1. [`README.md`](./README.md) — what this project is, hardware, scenes
2. [`PROTOCOL.md`](./PROTOCOL.md) — the host ↔ device wire format
3. [`AGENT.md`](./AGENT.md) — where things go, the file-placement table
4. [`HARNESS_GAPS.md`](./HARNESS_GAPS.md) — known places where esp-harness
   itself needs work; if your issue belongs here, log it as a gap.

## Setup

```bash
git clone https://github.com/Caldis/esp32-agent-dashboard
cd esp32-agent-dashboard

# 1. esp-harness toolkit (needed to build + flash + push to device)
git clone https://github.com/Caldis/esp-harness ../esp-harness
pip install -e ../esp-harness/tools/esp-harness/[test]
esp-harness doctor

# 2. The bridge has zero third-party deps beyond what esp-harness
#    pulls in (pyserial). No extra pip install needed.
```

## Development loop

The shortest cycle when changing a **dashboard scene** (firmware):

```bash
# 1. Edit
vim main/scenes/scene_sessions.c

# 2. Iterate against the sim if the scene is sim-buildable
cd ../esp-harness/examples/aurora/sim
cmake --build build -j
esp-harness sim diff --scenes sessions   # if a golden exists

# 3. Build + flash to device
cd D:/Code/esp32-agent-dashboard
esp-harness build --project .
esp-harness flash --project . --port COM9
```

When changing the **host bridge** (`tools/claude_buddy_bridge.py`):

```bash
# Replay the canned session — no device needed
python docs/mock_device.py --port 9876 &
python tools/claude_buddy_bridge.py replay docs/demo_inputs.jsonl \
    --port-kind tcp --port 127.0.0.1:9876

# Assert: every `dash <verb>` line emitted reached the mock,
# the mock state matches the expected snapshot/prompt/event sequence.
```

The CI workflow (`.github/workflows/ci.yml`) runs exactly this
roundtrip on every push / PR.

## How the bridge talks to the device

```
Claude Code hook ──► hook_dispatch.py ──► TCP 7321 ──► claude_buddy_bridge.py
Codex JSONL      ──► codex_wrapper.py  ──► TCP 7321 ──┘
                                                     │
                                                     ▼
                                          SessionRegistry (per-agent state)
                                                     │
                                                     ▼   (throttled, ≤ 1 / 250 ms
                                                     │   + 10 s keepalive)
                                                     ▼
                                          esp-harness console_session
                                                     │
                                                     ▼
                                              USB-Serial COM9
                                                     │
                                                     ▼
                                          ESP32-S3 firmware
                                          (`dash` verb handler)
                                                     │
                                                     ▼
                                              5 LVGL scenes
                                                     │
                                                     ▼   (BOOT / USER buttons
                                                     │    on prompt scene)
                                                     ▼
                                          `EVT: permission id=... decision=...`
                                                     │
                                                     ▼
                                          hook_dispatch.py exit code
                                                     │
                                                     ▼
                                          Claude Code blocks/allows
```

The full schema for each verb is in [`PROTOCOL.md`](./PROTOCOL.md). Both sides
treat `PROTOCOL.md` as the spec of record; firmware and bridge must
match. If you change the wire format, bump the `version` field in
the first `OK:` line on connect and update both ends in the same PR.

## Adding a scene

A "scene" is a screen the dashboard can show — `idle`, `sessions`,
`prompt`, `tokens`, `status`. Each lives in `main/scenes/scene_<name>.c`
and is registered through esp-harness's scene framework.

1. **Decide what the scene needs.** Does it consume an existing
   `dash` verb's payload, or does it need a new verb? If new, update
   [`PROTOCOL.md`](./PROTOCOL.md) first — that's the contract the bridge
   and firmware both code against.
2. **Implement it.** Mirror the structure of `main/scenes/scene_idle.c`.
   Allocate widgets in `scene_<name>_enter`, free them in
   `scene_<name>_exit`, update them in a `dash` verb handler.
3. **Register it.** Add a `harness_scene_register(...)` call in
   `main/app_main.c` next to the existing five.
4. **Trigger it.** Either auto (state-driven from a snapshot field,
   like prompt activation) or manual (a `dash scene <name>` verb).
5. **Capture an image.** Boot the device, get the scene live, save a
   PNG to `docs/img/<name>.png` (or `docs/<name>.png` for transient).
6. **Smoke-test it.** Add a case to `tools/smoke.ps1` that pushes the
   shape of payload that activates the scene and asserts no `ERR:`
   comes back. If the scene reacts to a button, add a tap simulation
   via `esp-harness console --cmd 'tap 233 233'`.

## Adding a bridge hook

The bridge funnels hook events from Claude Code and Codex into a
single event stream. To support a new hook type or a new agent:

1. **Document the event shape.** Add it to `docs/demo_inputs.jsonl` —
   a single canned line of the JSON the new event looks like. The CI
   roundtrip replays this file, so anything captured here is regression-
   tested.
2. **Add the handler in `SessionRegistry.apply()`** (`tools/claude_buddy_bridge.py`).
   Decide whether it mutates the session, opens a prompt, or just
   shows a transient toast.
3. **Decide if it triggers a snapshot push.** Most events do.
   `PreToolUse` events that need approval trigger a `dash prompt`
   AND block on the device's `EVT: permission` reply.
4. **Test via replay.** Add the new event line to
   `docs/demo_inputs.jsonl` and run the CI roundtrip locally:
   ```bash
   python docs/mock_device.py --port 9876 &
   python tools/claude_buddy_bridge.py replay docs/demo_inputs.jsonl \
       --port-kind tcp --port 127.0.0.1:9876 --assert-dash-commands
   ```

## Branching + PR

```bash
git checkout -b feat/<short-name>      # or fix/<...>, docs/<...>
# ... commits ...
git push origin feat/<short-name>
gh pr create --fill                    # the PR template will load
```

PR title format: `<type>: <imperative summary>`

Examples:
- `feat(scene): add scene_tokens with 24h sparkline`
- `fix(bridge): codex_wrapper assumes tool_input is dict`
- `docs(protocol): clarify v1 prompt response shape`
- `chore(ci): pin pyserial == 3.5`

## CI / verification

Every PR runs `.github/workflows/ci.yml`:

1. `pip install` the bridge's deps
2. Boot `docs/mock_device.py` on a free TCP port
3. Replay `docs/demo_inputs.jsonl` through the bridge in dry-run + live mode
4. Assert the expected `dash` verbs were emitted in the expected order
5. Assert no `ERR:` came back from the mock

Firmware build is **not** in CI — cross-compiling ESP-IDF is slow and
runner-image-fragile. We exercise the firmware-side parser via the
mock (which mirrors `console_protocol.c`'s tokeniser exactly) and
defer real-hardware verification to release-time `tools/smoke.ps1`.

## Code style

- **C**: 4-space indent, no tabs, K&R braces. Match the surrounding
  file. We don't run a formatter — manual care.
- **Python**: PEP 8 with 4-space indent. Type hints on public APIs.
- **Markdown**: prose, not bullet-grids when you can help it. Aim for
  80-char lines.
- **Commit messages**: imperative subject (≤ 70 chars), blank line,
  body explaining *why*.

## What good looks like in a PR

A PR is "ready to review" when it has:

- [ ] One conceptual change. Bundle related cleanups, but a PR fixing
      a bug *and* adding a feature is two PRs.
- [ ] A description explaining the **why**, not just the what.
- [ ] CI green (bridge roundtrip passes locally).
- [ ] If touching the wire format: [`PROTOCOL.md`](./PROTOCOL.md) updated
      in the same PR. Both firmware and bridge sides updated.
- [ ] If adding a scene: image at `docs/img/<name>.png` + smoke case.
- [ ] If you discovered an esp-harness gap: a row in
      [`HARNESS_GAPS.md`](./HARNESS_GAPS.md) with reproducer.

## Reporting bugs

Use [Issues → Bug Report](https://github.com/Caldis/esp32-agent-dashboard/issues/new?template=bug.md).
Always include:

- esp-harness version (`esp-harness --version`)
- ESP-IDF version (`idf.py --version`)
- The exact `dash <verb> ...` payload that failed (if reproducible)
- The device's exact `OK:` / `ERR:` reply (if reachable)

## Asking questions

If it's about "how do I integrate X with the dashboard?", use
[Issues → Question](https://github.com/Caldis/esp32-agent-dashboard/issues/new?template=question.md).

If it's "should we do X?", use a Discussion (or open a draft PR with a
sketch).

## License

By contributing, you agree your contributions are licensed under MIT
(matching the repo). No CLA required.
