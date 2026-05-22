# End-to-end demo runbook

The sequence that proves "Claude Code session live on device + button-press
permission decision" works. Run after all three subagents finish.

## Pre-flight

```powershell
cd D:\Code\esp32-agent-dashboard

# 1. esp-harness smoke must be green (Agent G shouldn't have broken anything)
cd D:\Code\esp-harness
.\tools\smoke.ps1 -SkipDevice
# expected: 7/7 host cases green
# new case should include: oversize line: one ERR + no spurious cmd
#   (agent-dashboard regression)

# 2. Build + flash the dashboard firmware
cd D:\Code\esp32-agent-dashboard
$py = "D:\Code\esp-harness\tools\esp-harness\.venv\Scripts\python.exe"
& $py -m esp_harness build --project . --json | Select-Object -Last 1
& $py -m esp_harness flash --project . --port COM9 --json | Select-Object -Last 1
```

## Stage 1 — dry-run with synthetic events

```powershell
# Drive the bridge from the canned demo inputs (no live CC needed)
Get-Content docs\demo_inputs.jsonl | python tools\claude_buddy_bridge.py serve --port COM9 --dry-run
# Expected: prints each `dash <verb> ...` line that WOULD have been sent.
# Verify count: 1 idle → 1 SessionStart → N PostToolUse → 1 PreToolUse (prompt) → ...
```

Then live:

```powershell
Get-Content docs\demo_inputs.jsonl | python tools\claude_buddy_bridge.py serve --port COM9
# Watch the AMOLED for scene transitions: idle → sessions → prompt → sessions → idle.
# At the prompt scene, press BOOT to approve OR USER to deny. Either should
# log a `permission` EVT and the bridge should print the matching decision.
```

## Stage 2 — live Claude Code

```powershell
# 1. In a long-lived terminal, start the bridge as a daemon
python D:\Code\esp32-agent-dashboard\tools\claude_buddy_bridge.py serve --port COM9 --log-level info

# 2. Add the hook config to ~/.claude/settings.json
#    (the bridge agent's tools/setup_hooks.ps1 should automate this — if not,
#     copy the snippet from docs/HOST_INTEGRATION.md by hand)

# 3. In a second terminal, run Claude Code on something
claude-code "build a tiny esp32 demo"

# Watch the device:
#  - SessionStart hook fires → device shows sessions scene with total=1, running=1
#  - PostToolUse hooks update the entries list
#  - When CC wants to Bash something, device flips to `prompt` — press BOOT or USER
#  - When CC finishes, device returns to `idle`
```

## Stage 3 — Codex (read-only side)

```powershell
# Pre-req: agent H's tee wrapper in place: PATH override / shim
# Run codex normally; the wrapper teestrm events into the bridge.
codex "summarise the README"

# Device should show a second concurrent session if Claude Code is also running.
# Codex's prompts stay interactive (no veto from device in v0; documented).
```

## What success looks like

- 5 scenes each rendered on device, scenes transitioned correctly
- Permission round-trip latency end-to-end ≤ 1 second
  (host hook fires → device renders prompt → button press → CC unblocks)
- No `ERR:` lines from the device for normal flow
- Bridge's `--log-level info` shows snapshot throttling working
  (≤ 1 push per 250 ms even when hooks fire rapidly)
- `HARNESS_GAPS.md` has at least one entry, each resolved with an esp-harness commit hash

## Capture for the writeup

```powershell
# Screenshots of each scene
& $py -m esp_harness console --cmd "dash idle" --port COM9 ; & $py -m esp_harness screenshot --port COM9 --out docs/img/idle.png
& $py -m esp_harness console --cmd 'dash snapshot "{\"total\":2,\"running\":1,\"waiting\":1,\"msg\":\"approve: Bash\",\"entries\":[\"10:42 git push\",\"10:41 yarn test\"],\"tokens\":184502,\"tokens_today\":31200,\"prompt\":null}"' --port COM9 ; & $py -m esp_harness screenshot --port COM9 --out docs/img/sessions.png
& $py -m esp_harness console --cmd 'dash prompt "{\"id\":\"req_demo\",\"tool\":\"Bash\",\"hint\":\"rm -rf /tmp/foo\"}"' --port COM9 ; & $py -m esp_harness screenshot --port COM9 --out docs/img/prompt.png
& $py -m esp_harness console --cmd 'dash tokens "{\"cumulative\":184502,\"today\":31200,\"latest_sample\":1240}"' --port COM9 ; & $py -m esp_harness screenshot --port COM9 --out docs/img/tokens.png
```

Put the four PNGs in `docs/img/`, embed in README.md.
