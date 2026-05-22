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

**Resolution**: `esp-harness@ba44c06` — `esp_harness.client.open_persistent_session(port)`
returns a `SessionHandle` that holds ONE connection open for the
lifetime of the bridge. The ~140 ms startup cost amortises to zero
across N pushes; bench numbers stay at v0.1.1 levels (~310 events/s
in dry-run, median 5 µs) while gaining concurrent read+write on the
same wire. Bridge adoption: `esp32-agent-dashboard@G2` (this cycle).

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

**Resolution**: `esp-harness@ba44c06` — `SessionHandle` supports
concurrent `write_line()` (from any thread) AND `iter_events()` /
`on_event` callbacks (reader thread). Tested with a writer thread
pushing 20 snapshots while iter_events() awaits an EVT — both
arrive on the same wire. Bridge now uses one open handle for
snapshots + permission EVTs (replaces the pre-v0.2 bespoke
`_evt_reader_loop`).

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

**Resolution**: `esp-harness@335d435` — the firmware was already
emitting `tag=HELP` / `tag=SCENES` / `tag=DUMP` / `tag=HEALTH` since
v1.7.5; v0.2.0 documents the convention as the explicit-tag contract
in `console_protocol.h`, the aurora-harness README, and the toolkit
README. The host-side helper `PayloadFollowsReader` (shipped this
cycle as G-H1) detects the tag via the regex
`\btag=([A-Z][A-Z0-9_]*)\b` on every OK body and routes the
following block accordingly; legacy `OK: payload follows` without
a tag is still accepted for backward compatibility.

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

### G-7 — tokeniser strips ALL double quotes, collapses nested JSON

**Context**: F (firmware) + orchestrator (live device test).

**What I needed**: `dash prompt "{\"id\":\"req_001\",\"tool\":\"Bash\","
`\"hint\":\"rm -rf /tmp/foo\"}"` to reach the firmware parser as the
verbatim JSON inside the outer quotes.

**What I got**: The v1.7.1 tokeniser had two behaviours for a quote:
"toggle in_quote, drop the char." So ALL `"` got stripped from any
token containing them — outer delimiters AND inner JSON keys/values
alike. The cJSON parser on the device got `{id:req_001,tool:Bash,...}`
which is invalid syntax. `dash prompt` returned `ERR: prompt id
required` because cJSON couldn't find any string keys.

**Workaround used**: live-patched `console_protocol.c` to a two-mode
tokeniser:
- Tokens starting with `"` — only the matching close-quote at end-of-
  token terminates the token. Inner `"` pass through verbatim.
- Tokens NOT starting with `"` — legacy toggle-on-any-quote behaviour
  preserved (don't break `wifi connect ssid="My Wi-Fi"`).

**Suggested upstream fix**: Same shape as the workaround. Smoke case:
`dash prompt` with a nested JSON payload survives the tokeniser →
parses with valid `id`/`tool`/`hint` keys.

**Resolution**: `D:\Code\esp-harness` commit `664b14e` —
`fix(G-7): tokeniser strips all double quotes, collapses nested JSON`.
Verified live: prompt scene renders correctly on device with
permission button hints visible. See `docs/img/dash-prompt.png`.

### G-8 — consumer mocks re-implement the framework tokeniser and drift silently

**Context**: orchestrator / V1-E (stress.py + docs/mock_device.py).

**What I needed**: A way for any project consuming esp-harness to test
its own host-side bridge against an honest device stand-in *without
maintaining a copy of the parser*. The dashboard's `mock_device.py` had
its own `_tokenise()` copy of the firmware's logic. When G-7 changed
the firmware tokeniser, the mock was not updated, and the V1-E stress
suite produced 0/1000 OK on the flood test + 0/5 prompt latencies —
not because the bridge was broken, but because the mock's stale parser
made the firmware-shaped JSON look invalid to it. Drift is silent: the
mock and the device disagree, the bridge tests "pass" against the mock
while a real device would have caught it.

**What I got**: a one-shot fix to mock_device.py mirroring console_protocol.c.
Works *now*, breaks the next time someone touches the C tokeniser.

**Workaround used**: (no longer needed — see Resolution.)

**Upstream fix**: shipped `esp_harness.core.parser.tokenise_console_line`
as the canonical Python port of the C tokeniser, with 25 parity tests
in `tools/esp-harness/tests/test_parser.py`. The dashboard's
`mock_device.py` now imports it instead of carrying its own copy. Next
time the C tokeniser changes, both sides move together or the parity
test fails the build.

**Resolution**: `esp-harness/tools/esp-harness/src/esp_harness/core/parser.py`
+ `tests/test_parser.py` (this commit). End-to-end re-verified: stress
suite 5/5 passes against the import-based mock.

### G-D1 — `docs/demo_inputs.jsonl` ≠ bridge event schema

**Context**: V1-D (publish).

**What I needed**: `docs/E2E_DEMO.md`'s replay step to actually feed
the bridge — i.e. `cat demo_inputs.jsonl | bridge replay -` should
produce the expected `dash snapshot` / `dash prompt` chatter.

**What I got**: `demo_inputs.jsonl` uses Claude-Code-hook PascalCase
event names (`{"event":"SessionStart",…}`) while the bridge expects
snake_case `type:` keys (`{"type":"user_prompt_submit",…}`). Replaying
the file currently yields 13 unrecognized events. CI works around this
by using `tools/sample_session.jsonl` which is in the bridge's shape.

**Workaround used**: CI uses the bridge-shape file; docs still call
out the original.

**Suggested fix**: `hook_dispatch.py` should normalize PascalCase →
snake_case at the ingress, and `demo_inputs.jsonl` should keep the
PascalCase form (it's documenting what Claude Code sends) — then a
single replay reaches both audiences. Alternatively, document the
two schemas as deliberately distinct (raw hook vs bridge wire) so
nobody confuses them.

**Resolution**: (pending — flagged in `CHANGELOG.md` known limitations.)

### G-D2 — bridge has no `--port-kind tcp` for the mock device

**Context**: V1-D (publish, CI design).

**What I needed**: CI to exercise a true mock round-trip
(bridge → TCP mock → bridge sees reply) rather than dry-run replay.

**What I got**: `claude_buddy_bridge.py serve` only opens via
`esp_harness.core.console_session.ConsoleSession` (a real serial
port). There's no flag to route the push to a TCP socket. CI
sidesteps with `--dry-run` (which prints `[DRY] dash ...` without
opening any device).

**Workaround used**: dry-run replay in CI; full mock loop is local-
only via `docs/mock_device.py` listening on a port + manual eyeballs.

**Suggested fix**: `claude_buddy_bridge.py serve --port-kind {serial,tcp}`
and a parallel `host:port` arg shape. CI then runs the bridge against
the in-process mock and asserts the mock observed each expected dash
verb (much stronger contract than dry-run printing).

**Resolution**: (pending — non-blocking for v0.1.0.)

### G-H1 — bridge needs payload-follows reply framing helper

**Context**: H1 (host bridge v1 upgrade).

**What I needed**: A documented, library-supplied way to consume the
new `OK: payload follows tag=<TAG>` / `<TAG>_BEGIN ... <TAG>_END`
multi-line reply shape introduced in PROTOCOL.md v1 (currently used
by `dash health`, future `dash screenshot`, etc.). The host bridge
has to roll its own state machine to (a) recognise the trigger line,
(b) skip the `<TAG>_BEGIN fmt=json bytes=N` framing line, (c) collect
inner lines, (d) flush on `<TAG>_END`. Each consumer will re-invent
this and inevitably differ in edge cases (mine missed the BEGIN-skip
on first try and got "Expecting value: char 0" because the framing
line went into the JSON buffer).

**What I got**: protocol spec mentions the convention but no helper.
The pyserial line iterator gives us raw lines and we glue the state
machine ourselves.

**Workaround used**: hand-rolled `_process_line()` + `_await_tag` /
`_await_buf` in `tools/claude_buddy_bridge.py:DevicePusher`.

**Suggested upstream fix**: ship `esp_harness.core.parser.PayloadFollowsReader`
or similar — give it an iterator of OK/EVT/ERR lines and it yields
either a single-line OK/ERR/EVT or a parsed `(tag, blob)` tuple. Then
both `claude_buddy_bridge` and any other consumer reuse identical logic.

**Resolution**: `esp-harness@39018e2` — shipped
`esp_harness.core.parser.PayloadFollowsReader`. Feed it the device's
line stream (per-line via `feed()` or iterable via `feed_lines()`)
and it yields `ReplyEvent(kind=..., text=..., tag=..., meta=...,
blob=...)` for ok / err / evt / payload / log events. 15 contract
tests cover single-line replies, multi-line payloads, payload+EVT
interleave, partial line-by-line feed, reset, orphan BEGIN, and
the self-describing-OK + legacy `payload follows` paths. The new
`open_persistent_session` API uses it internally; the dashboard
bridge dropped the hand-rolled state machine.

### G-H2 — no canonical Python G-7 tokeniser yet for *quote-leading mode*

**Context**: H1 (had to extend mock_device for v1 verbs).

**What I needed**: When `mock_device.py` (which is V1-C's domain) does
not yet exist for v1, agents writing their own v1 mock (`tools/mock_device_v1.py`)
need to import the exact G-7 quote-leading tokeniser used by the device.
G-8 surfaced this and the resolution claims `esp_harness.core.parser.tokenise_console_line`
ships it. But during the H1 cycle that module wasn't importable from
this repo (path-injection ergonomics), so I re-implemented it by hand —
and got it WRONG on the first try (my version terminated quote-leading
tokens at the next whitespace rather than at the next `"`-followed-by-whitespace).
That broke every snapshot whose `msg` field contained a space (which is
basically every realistic prompt: `"> refactor the auth flow"`).

**What I got**: silent failure mode — bridge sent 10 snapshots, mock
received them, mock's stale parser returned a half-token, json.loads
raised `Unterminated string starting at char 113`. Mock send `ERR: ...`
back to the bridge but the bridge ignores ERR lines. Mock log showed
LINE_IN entries but no RX entries for ~30 seconds before I added
MALFORMED logging.

**Workaround used**: corrected my tokeniser in `tools/mock_device_v1.py`
to find the closing `"` followed by whitespace/EOL, not the next whitespace.

**Suggested upstream fix**: same as G-8 — make the canonical Python
tokeniser trivially importable from a consumer's `tools/` dir. Also
add G-7 parity test cases that include spaces inside the quoted JSON
payload (e.g. `dash snapshot "{"msg":"hi there"}"`); the existing
parity suite apparently didn't cover this since the agents that wrote
it never had a `msg` with whitespace in the test corpus.

**Resolution**: (pending — would close out G-8 properly.)

### G-H3 — bridge should log ERR replies from device

**Context**: H1 (debugging session loss).

**What I needed**: When the device (or mock) returns
`ERR: dash <verb>: malformed JSON (...)`, the bridge currently swallows
it silently in `_process_line()` — only OK / EVT lines are processed.
That made the "snapshot doesn't appear on device" failure mode invisible
until I instrumented the mock side. The bridge log just showed timing
stats "10 pushes successful" because `sendall()` returned cleanly, but
the device side had errored out on every single one.

**What I got**: bridge logs nothing about ERR lines.

**Workaround used**: noted; will add an ERR-line warning in a follow-up
patch (would change observable behaviour mid-cycle, so leaving for next
iteration).

**Suggested upstream fix**: `esp_harness.core.console_session` should
surface ERR lines via a dedicated callback (or expose them through the
iter_lines stream with a tag so consumers can choose to log/raise).
Bridges that ignore them are bridges that pretend to work.

**Resolution**: `esp-harness@6084c1e` + `esp-harness@ba44c06` —
`ConsoleSession` gains `on_err` callback parameter AND collects
every observed ERR in `Response.errs: list[str]` (in addition to
the first one populating `Response.text`). The new
`SessionHandle` yields ERRs through `iter_events()` as
`ReplyEvent(kind="err")` and offers a convenience `on_err(cb)`
hook. Verified live: bridge now logs `[bridge] device ERR: dash:
unknown verb 'config'` for the three pre-v1 verbs the mock
device rejects (previously these were silently dropped).

### G-F1a — `tiny_json.skip_value` mis-balances nested `{}` inside `[]`

**Context**: F1 (firmware v1 upgrade).

**What I needed**: `tj_object_find(json, end, "totals", ...)` to succeed
even when the preceding `"agents":[{...},{...}]` value contains nested
objects. The skip_value walker must balance brackets across mixed `{`
and `[`.

**What I got**: The original v0 walker only decremented depth when the
closing char matched the OUTER kind (object vs array). For an array
`[{...}]` it incremented on inner `{` but never decremented on inner
`}`, leaving depth ≥ 1 forever and the walker returned false. The
caller silently fell into the v0 single-agent fallback and only
registered ONE agent regardless of the array length. Symptom: a
two-agent snapshot replied `{"applied":true,"agents":1}` plus
`EVT: agent_added kind=claude-code session_id=` (empty session_id —
v0 path doesn't read kind/sid). v0 only ever had `entries[]` at top
level so the bug never manifested before.

**Workaround used**: Patched `main/tiny_json.c::skip_value` to count
BOTH `{}` and `[]` toward the same depth counter (one-line change).
Outer bracket kind is no longer used for matching. Verified live:
two-agent snapshot now replies `agents:2` and emits two `agent_added`
EVTs with correct session_ids.

**Suggested upstream fix**: This is a consumer-tree bug (tiny_json
lives per-project). Worth blessing a small `aurora-harness/json.h`
helper so each new consumer doesn't re-introduce the same class of
walker bug.

### G-F1b — `?dump` is hard-capped at 128×128, masking right-pane content

**Context**: F1 — surfaced during v1 acceptance shots.

**What I needed**: A 466×466 (full panel) screenshot of the v1 dual-pane
sessions scene so both panes appear in the capture. The right pane sits
at x ≥ 233 on the panel; a 128×128 dump shows only the top-left and the
codex pane never appears in any capture.

**What I got**: `?dump w=466` silently downgrades to `w=128` on the
device side (`OK: dump start ... w=128 h=128 fmt=RGB565LE bytes=32768`)
regardless of the requested dimension. The harness `console --payload
DUMP` correctly forwards the request; the cap is firmware-side in
`components/aurora-harness/src/screenshot.c`.

**Workaround used**: Acceptance screenshots show the LEFT pane only;
visual eyeball of the on-device sessions scene confirms the right
pane renders symmetrically with the codex accent.

**Suggested upstream fix**: Let `?dump w=N` accept any N up to panel
width. At 466×466×2 ≈ 434 KB raw → ~579 KB base64. Streaming the
payload via `console_write_raw` in chunks keeps memory steady; the
harness `--payload` reader already handles multi-line bodies.

**Resolution**: `esp-harness@cb72e87` —
`components/aurora-harness/src/screenshot.c::cmd_dump` now accepts
`w` in [32, 2048] and clamps to the active panel width. The OK
line carries `w_requested=N w_actual=M reason=<below_min|above_max|
panel_cap|default|ok>` so host parsers detect silent downgrades.
PSRAM impact is bounded by the existing free-size check; realistic
captures (≤ panel size 466×466) stay well inside budget. NOTE:
this is a firmware change — the dashboard board must re-flash to
pick it up. Until then, screenshots remain capped at 128×128 on
the live device.

## Resolved

| Gap | Resolution commit |
|---|---|
| G-7 (tokeniser collapse) | `esp-harness@664b14e` |
| (console-overflow drain) | `esp-harness@98affb0` (Agent G) |
| G-8 (consumer-mock parser drift) | `esp-harness@fb5a549` — `esp_harness.core.parser` + 25 parity tests |
| G-D2 (bridge has no TCP port-kind) | `esp32-agent-dashboard@H1` — bridge v1 adds `--port-kind {serial,tcp}` and `--port HOST:PORT`. |
| G-F1a (tiny_json depth) | `esp32-agent-dashboard@F1` — fixed in `main/tiny_json.c::skip_value`. |
| G-H1 (payload-follows helper) | `esp-harness@39018e2` — `esp_harness.core.parser.PayloadFollowsReader` + 15 tests. |
| G-1 + G-3 (persistent session) | `esp-harness@ba44c06` — `esp_harness.client.open_persistent_session(port)` + 12 tests; bridge adopted in `esp32-agent-dashboard@G2`. |
| G-H3 (ERR surfacing) | `esp-harness@6084c1e` + `esp-harness@ba44c06` — `ConsoleSession.on_err` + `SessionHandle.on_err` + ERR yielded through iter_events. |
| G-4 (explicit-tag convention) | `esp-harness@335d435` — convention documented in `console_protocol.h`, both READMEs; reader parses any `tag=NAME` body. |
| G-F1b (?dump w cap) | `esp-harness@cb72e87` — cap raised to 2048 / panel width; OK line emits `w_requested=` / `w_actual=` / `reason=`. Live device requires re-flash to take effect. |

(G-1..G-6, G-H1..G-H3, G-F1b CLOSED this cycle (v0.2.0). G-2 / G-5 / G-6
remain as docs/out-of-scope/north-star items. G-D1 surfaced by V1-D
remains non-blocking. G-H2 was about consumer-side mock parity and
was effectively absorbed by the G-8 resolution + G-H1's reader
helper — the mock no longer needs its own state machine.)
