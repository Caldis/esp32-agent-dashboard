# Get started — 30 minutes from zero to dashboard

This is the **shortest credible path** from "just cloned the repo" to
"my Claude Code session is rendering on the AMOLED and the BOOT button
unblocks tool prompts". It assumes:

- You already have **Claude Code** working in your terminal.
- You already have **ESP-IDF v6+** working (`idf.py --version` returns
  something).
- You have one of the boards listed in [`HARDWARE_GUIDE.md`](./HARDWARE_GUIDE.md).
  Best results: Waveshare ESP32-S3-Touch-AMOLED-2.16.

No ESP-IDF? Install it first via Espressif's
[Get Started](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/index.html)
— ~20 minutes, not repeated here.

Wall-clock: ~30 minutes if it goes right, mostly waiting on the first build.

---

## Step 1 — Get esp-harness (1 min)

esp-harness is the toolkit that ships the build/flash/console CLI and
the LVGL scene framework. This repo *consumes* it.

```bash
git clone https://github.com/Caldis/esp-harness
pip install -e esp-harness/tools/esp-harness/
esp-harness --version    # should print v1.7.5 or newer
```

## Step 2 — Get this repo (1 min)

```bash
git clone https://github.com/Caldis/esp32-agent-dashboard
cd esp32-agent-dashboard
```

## Step 3 — Verify the round-trip works *without* hardware (3 min)

Before you risk a flash, prove the toolchain talks to itself. Open two
terminals:

```bash
# Terminal A — stand-in for the device
python tools/mock_device_v1.py --port 9876 -v
```

```bash
# Terminal B — the smallest possible consumer of the wire protocol
python examples/01_minimal/run.py
```

Expected final line: `OK — minimal round-trip complete.` and exit code 0.

If this works, the host side is healthy. If it doesn't, fix it here —
the rest of the steps will compound the error.

## Step 4 — Build + flash the firmware (10 min, mostly the first build)

Plug the board in via USB-C. Find the COM / tty port (see
[`HARDWARE_GUIDE.md`](./HARDWARE_GUIDE.md) step 2 if you're not sure).

```bash
esp-harness build --project .
esp-harness flash --project . --port COM9    # adjust port
```

First build takes 5-10 min. Subsequent builds: ~30 s.

When flash completes the AMOLED clears and parks on the **idle** scene
— a faint "zZz" pulse on a dim ring. That's the device telling you the
firmware is alive and waiting for a host.

## Step 5 — Start the bridge (1 min)

The bridge listens for Claude Code hooks on `127.0.0.1:7321`, keeps a
per-agent session registry, pushes throttled snapshots to the device,
and routes `EVT: permission` decisions back to the blocked hook.

```bash
python tools/claude_buddy_bridge.py serve --serial-port COM9
# [bridge] serving on 127.0.0.1:7321 | dry_run=False | serial=COM9
```

Leave it foreground for the rest of the session.

## Step 6 — Wire Claude Code's hooks to the bridge (5 min)

Open `~/.claude/settings.json` (create it if missing) and add:

```jsonc
{
  "hooks": {
    "PreToolUse":  "python D:/Code/esp32-agent-dashboard/tools/hook_dispatch.py pre_tool_use",
    "PostToolUse": "python D:/Code/esp32-agent-dashboard/tools/hook_dispatch.py post_tool_use",
    "Stop":        "python D:/Code/esp32-agent-dashboard/tools/hook_dispatch.py stop"
  }
}
```

Adjust the absolute path. Use forward slashes even on Windows — CC
accepts them and they dodge the backslash-escaping minefield.

> **Why three hooks?** `PreToolUse` blocks until the button is pressed.
> `PostToolUse` updates the transcript line. `Stop` tells the bridge
> the session is over so the device can return to idle.

## Step 7 — Run Claude Code on something (5 min)

In a *fresh* terminal (so the new `settings.json` is picked up):

```bash
claude "build a tiny hello-world esp32 demo"
```

Watch the AMOLED: **idle** wakes within a second → **sessions** shows
the agent's `cwd` and last few tool calls → when CC wants to `Bash`
something, **prompt** takes over (press **BOOT** to approve once,
**USER** to deny) → device drifts back to **idle** when CC finishes.

End-to-end latency (hook fires → device renders → button press → CC
unblocks) should be ≤ 1 s. If it isn't, see
[`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md).

## You're done

From here:

- [`examples/02_two_agents/`](../examples/02_two_agents/) — two agents
  concurrently (useful if you also use Codex).
- [`examples/03_prompt_roundtrip/`](../examples/03_prompt_roundtrip/)
  — the permission round-trip in isolation (button hardware test).
- [`PROTOCOL.md`](../PROTOCOL.md) — the full v1 wire grammar (for
  writing your own consumer).
- [`HARNESS_GAPS.md`](../HARNESS_GAPS.md) — rough edges and where
  they're resolved upstream.

## When something goes wrong

[`TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) covers the common
failure modes in the order they appear: COM port → build → idle →
button → ERR snapshots.
