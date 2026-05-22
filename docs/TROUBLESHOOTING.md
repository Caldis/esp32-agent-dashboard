# Troubleshooting

Common failure modes, ordered by where they show up in the get-started
flow. Each entry: what you see, why, what to do.

## Quick index

| Symptom | Section |
|---|---|
| no COM port when I plug in | [#com-port-not-found](#com-port-not-found) |
| `esp-harness build` fails — `cJSON: no such file` | [#build-fails-with-cjson](#build-fails-with-cjson) |
| device stays on **idle** even with CC running | [#device-stays-on-idle](#device-stays-on-idle) |
| prompt scene shows, BOOT doesn't approve | [#permission-button-does-not-approve](#permission-button-does-not-approve) |
| every snapshot → `ERR: dash snapshot: malformed JSON` | [#all-snapshots-err](#all-snapshots-err) |
| stuck on Waveshare factory demo | [#stuck-on-factory-demo](#stuck-on-factory-demo) |
| AMOLED black or scrambled | [#blank-amoled](#blank-amoled) |
| bridge logs "throttled" / "snapshot dropped" | [#bridge-throttle-warnings](#bridge-throttle-warnings) |
| Linux: `Permission denied: /dev/ttyACM0` | [#serial-permission-denied](#serial-permission-denied) |

---

## COM port not found

Plugged in, no new port in Device Manager / `/dev/tty*`.

**Likely cause, in order:** charge-only USB cable; no driver (Win 10+
has it; older Windows needs the Espressif `usb_serial_jtag` driver);
board in download mode (hold **RST** 1 s, release).

**Verify.** `esp-harness list-ports` — if it's empty, the OS doesn't
see the board. Fix that before anything else.

## Build fails with cJSON

```
fatal error: cJSON.h: No such file or directory
```

**Cause.** ESP-IDF v6 dropped cJSON. The dashboard migrated to
in-repo `main/tiny_json.c`.

**Fix.** Make sure you're on v0.7.0+; `idf.py fullclean` and rebuild;
`grep -r cJSON main/` should be empty — any leftover include is a bug.

## Device stays on idle

Bridge and CC are running but the AMOLED never advances from idle.

**Likely cause:**

1. Hooks not wired (`~/.claude/settings.json` doesn't reference
   `hook_dispatch.py`, or the path is wrong, or CC is using a
   project-local override).
2. Bridge isn't serving HTTP (crashed, or bound to a different port —
   hooks POST to `127.0.0.1:7321`; CC does not error on hook failure).
3. Bridge is in `--dry-run` — it accepts hooks but logs "would send"
   instead of serialising.

**Diagnose, 4 commands:**

```bash
# bridge listening?
curl -s http://127.0.0.1:7321/healthz
# {"ok":true,"uptime_s":...}

# hook actually firing? — check ~/.claude/logs/

# bridge sending to device?
python tools/claude_buddy_bridge.py serve --serial-port COM9 --log-level debug

# bypass CC entirely
python examples/01_minimal/run.py
```

The first command that returns the wrong thing is where the chain
broke.

## Permission button does not approve

Prompt scene rendering. You press **BOOT**. CC stays blocked until the
60 s timeout.

**Likely cause:**

1. Bridge's serial-reader thread crashed (happens on Windows if
   another process claimed the COM port). EVT line vanishes.
   Restart the bridge.
2. Some clone boards have the silkscreen swapped — try the other
   button.
3. `id` mismatch (two bridge instances racing for one device). `ps |
   grep claude_buddy_bridge` should show exactly one.

**Diagnose.** Bridge at `--log-level debug`. Expect three lines:

```
[bridge] prompt sent id=req_abc tool=Bash
[bridge] EVT permission id=req_abc decision=once    <-- proof
[bridge] unblocking hook for id=req_abc
```

Missing line 2 → device → bridge path broken. Missing line 3 → bridge
→ hook path broken (hook timed out before the decision; CC's hook
timeout default is 60 s).

## All snapshots ERR

Every snapshot gets `ERR: dash snapshot: malformed JSON (...)`.

**Cause.** Almost always the **G-8 class**: consumer tokeniser doesn't
match the framework's. Device-side tokeniser uses G-7 semantics —
quote-leading tokens close at the *last* `"` followed by whitespace/
EOL, not the *next* `"`. Hand-rolled tokenisers that close at the
next `"` mangle every snapshot whose `msg` contains a space.

**Fix.**

1. Import `esp_harness.core.parser.tokenise_console_line` — canonical
   Python port of the firmware tokeniser, kept in lockstep by the
   framework's parity tests.
2. If you can't import it (older esp-harness), copy `_tokenise`
   verbatim from `tools/mock_device_v1.py` — field-proven G-7 logic.
3. Add a parity test against `mock_device_v1.py` with `{"msg":"hi
   there"}` — if the mock rejects, your bridge would too.

Full post-mortem: [`HARNESS_GAPS.md`](../HARNESS_GAPS.md) entry G-8.
Upstream fix: `esp-harness@fb5a549`.

## Stuck on factory demo

AMOLED shows the Waveshare demo (rotating sphere); flashing seems to
do nothing.

**Cause.** Flash succeeded but board didn't reset into the new
firmware (USB-C splitters can drop the reset signal).

**Fix.** Hold **RST** 1 s, release. If still stuck: hold **BOOT**, tap
**RST**, release **BOOT** → forces download mode → re-flash.

## Blank AMOLED

Board enumerates fine, can flash, COM port prints logs — but the AMOLED
is black or shows colour bars.

**Likely cause:**

1. First flash after `erase_flash` — power-cycle (unplug, replug).
2. Reversed LiPo polarity browning the panel out. Disconnect cell,
   USB-only, fix polarity per
   [`HARDWARE_GUIDE.md`](./HARDWARE_GUIDE.md).
3. Wrong sdkconfig (copied from a similar Waveshare SKU). Delete
   `sdkconfig`, rebuild — `sdkconfig.defaults` is the source of truth.

## Bridge throttle warnings

`[bridge] snapshot dropped (throttled)` periodically.

**Expected.** Protocol caps snapshots at 1 per 250 ms. Bridge
coalesces rapid bursts and sends the latest — nothing visible is lost.
Override with `--throttle-ms 0` only if you want to stress-test (the
device serial buffer will then fill and ERR, which puts you in the
[#all-snapshots-err](#all-snapshots-err) bucket).

## Serial permission denied

Linux:

```
OSError: [Errno 13] Permission denied: '/dev/ttyACM0'
```

Your user isn't in `dialout` (or `uucp` on Arch).

```bash
sudo usermod -aG dialout $USER
# log out / log back in
```

One-off: `sudo chmod a+rw /dev/ttyACM0` — resets on re-plug. Group
fix is the durable one.

---

If your symptom isn't here, open a GitHub issue with: exact command,
exact output (copy/paste, don't summarise), OS + board, and the bridge
log at `--log-level debug`.
