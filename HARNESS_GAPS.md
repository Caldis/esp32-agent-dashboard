# Harness gaps surfaced by esp32-agent-dashboard

A running list of places where the esp-harness framework fell short
during this project. **Agent G** (in `D:\Code\esp-harness\`) watches
this file and lifts each gap upstream — either as a code fix in
esp-harness master, or a documented-recipe so the next consumer
doesn't reinvent it.

The point isn't to complain — it's to make every real project that
consumes esp-harness improve the framework itself. By the end of
this project, this file should be empty OR every entry should have
an upstream commit / docs entry it points at.

## Format

```
### G-N — <one-line title>

**Context**: Which sub-agent hit it (F = firmware, H = host bridge).
**What I needed**: …
**What I got**: …
**Workaround used**: …
**Suggested upstream fix**: …
**Resolution**: link to commit hash or doc PR in esp-harness/.
```

## Open

### G-1 — `esp-harness console --cmd` startup cost dominates push latency

**Context**: H (host bridge).

**What I needed**: Sub-100ms snapshot push so the dashboard feels live.

**What I got**: 11 live pushes to COM9, end-to-end median **324.7 ms**
(min 307.3, max 329.3, stdev 7.4). The harness itself reported the
device round-trip as median **186 ms**, so **~140 ms per call is pure
Python interpreter spin-up + import graph + serial-port re-open**, fixed
overhead independent of the payload.

**Workaround used**: Bumped the snapshot throttle to 250 ms and added a
keepalive. For the permission round-trip (where we need a persistent EVT
reader anyway) the bridge sidesteps the subprocess entirely and imports
`esp_harness.core.console_session.ConsoleSession` directly.

**Suggested upstream fix**: Ship a `esp-harness console --daemon` mode
(named-pipe or TCP socket) that keeps the port open and processes
commands without re-importing/re-opening per call. A `--bench` flag on
console would also let us measure this regression as harness evolves.

**Resolution**: (pending)

### G-2 — `subprocess.run(..., text=True)` crashes on GBK Windows console

**Context**: H.

**What I needed**: Stable subprocess capture of `esp-harness console --json`.

**What I got**: On zh-CN Windows the subprocess `_readerthread` raised
```
UnicodeDecodeError: 'gbk' codec can't decode byte 0x82 ... in position 103
```
when a port descriptor came back as `"USB 串行设备 (COM9)"`. The parent
saw `proc.stdout is None` and the snapshot publisher thread crashed on
`proc.stdout[-200:]`.

**Workaround used**: Bridge now passes `capture_output=True` (bytes) and
decodes with `errors="replace"`.

**Suggested upstream fix**: Force UTF-8 stdout in the esp-harness CLI
entrypoint (`sys.stdout.reconfigure(encoding="utf-8")` and/or
`os.environ.setdefault("PYTHONIOENCODING", "utf-8")` at startup) so
`--json` output is locale-independent.

**Resolution**: (pending)

### G-3 — Can't share an open serial port between snapshot push and EVT listen

**Context**: H.

**What I needed**: Push `dash prompt` AND keep a long-lived reader on
the same port to catch the matching `EVT: permission id=... decision=...`
when the user taps a button. Today `console --cmd ... --wait-evt REGEX`
opens-uses-closes per call, so it can't be combined with a high-rate
snapshot stream.

**What I got**: Each `console --cmd` is fire-and-forget on its own
opened port.

**Workaround used**: The bridge imports `ConsoleSession` directly and
runs a private background thread (`DevicePusher._evt_reader_loop`) that
pulls EVT lines off the wire and dispatches by `req_id`.

**Suggested upstream fix**: Expose a public, supported persistent-session
API — either `esp-harness console --listen` that streams JSONL of every
device line to stdout, or `from esp_harness.client import
open_persistent_session(port) -> SessionHandle` that the user can share
across snapshot/EVT consumers.

**Resolution**: (pending)

### G-4 — `?help json` payload tag is undocumented (`HELP`)

**Context**: H.

**What I needed**: Programmatic way to know which `?cmd`s the device
exposes. The console returns `OK: manifest follows` then a payload block
framed by `HELP_BEGIN / HELP_END`, but the tag name isn't in any user-
visible doc — had to grep firmware source.

**Workaround used**: Pass `--payload HELP` to `esp-harness console`.

**Suggested upstream fix**: Make the OK line self-describing, e.g.
`OK: manifest follows tag=HELP`, and/or add a one-liner to the harness
README's "discoverable commands" section.

**Resolution**: (pending)

### G-5 — Codex CLI has no native hook system (host-side gap, not harness)

**Context**: H.

**What I needed**: A `~/.codex/hooks.json` analogue of Claude Code so
the bridge could ingest Codex events without wrapping.

**What I got**: Nothing — `codex --help` mentions no hook config, and
`~/.codex/config.toml` has no `[hooks]` section.

**Workaround used**: Shipped `codex_wrapper.py` that the user must
invoke as `python codex_wrapper.py -- codex exec ...`. Wrapper parses
codex's JSONL stdout and forwards normalized events to the bridge over
TCP. The exact codex event-type strings are inferred from public
behavior and may drift between codex releases.

**Suggested upstream fix**: Lobby for codex hooks (out of esp-harness
scope, noted here for awareness). Until then, this gap is structural
and the wrapper approach is the only path.

**Resolution**: (pending — upstream, not harness)

### G-6 — smoke gate is Aurora-coupled; consumer projects drag in irrelevant tests

**Context**: orchestrator (this multi-agent run, surfaced by user
question).

**What I needed**: A `smoke.ps1` that, when run from THIS project's
context, tests THIS project's invariants — `dash snapshot` is parsed
correctly, 5 dashboard scenes exist (not 20 Aurora scenes), permission
round-trip latency is reasonable.

**What I got**: `D:\Code\esp-harness\tools\smoke.ps1` mixes
HARNESS-quality gates (doctor / pytest / sim-diff against goldens /
manifest count / version triangulation / MSys trap) WITH Aurora-as-
consumer gates (`?stat scene_count == 20`, audio tone bytes, audio
mic peak, `?ota info running ota_0`, tap `--wait-evt tap_hit`,
`?keys press boot/user/pwr`, bench --compare, `scene system`,
`scene halo`). When the dashboard project's smoke runs, it currently
either: (a) inherits the Aurora gates and fails because the device is
flashed with dashboard firmware (no audio scene, no scene halo,
scene count is 5 not 20), or (b) skips device gates and loses any
project-specific verification.

**Root cause**: Lesson 7 (`every test that has ever caught a real
bug runs every release`) was implemented as one global smoke. Aurora's
scene_count / audio / OTA / keys / scene-halo cases are CONSUMER
regression tests, not FRAMEWORK quality gates. They should live in
the consumer's tree.

**Workaround used**: nothing yet — the dashboard runs its own
ad-hoc verification.

**Suggested upstream fix**: Split the smoke gate into two layers:

- `D:\Code\esp-harness\tools\smoke.ps1` — **framework-only** gates:
  doctor, pytest, sim-diff against Aurora golden (Aurora is the
  reference firmware so its goldens stay), manifest, version
  triangulation, MSys trap, README drift. These prove the framework
  itself works.
- `D:\Code\esp-harness\examples\aurora\tools\smoke.ps1` — Aurora's
  **consumer** smoke: device-side scene_count == 20, audio tone bytes,
  audio mic peak, ?ota info, ?keys press, scene switches, bench compare,
  L1/L2/L9/R2/R4-bug regression. These prove Aurora hasn't regressed.
- `D:\Code\esp32-agent-dashboard\tools\smoke.ps1` — Dashboard's
  consumer smoke: 5 scenes registered, dash protocol parses,
  permission round-trip < 1 s, oversize line drops cleanly (G-7 + this).

A new `esp-harness smoke` toolkit command would run the framework
gates and then chain into the current project's smoke if one exists
(`./tools/smoke.ps1`).

**Resolution**: (pending)

## Resolved

(none yet)
