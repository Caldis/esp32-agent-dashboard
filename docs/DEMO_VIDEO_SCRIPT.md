# Demo video script — 2:30 target

**Audience.** Hacker News front page. Don't explain what an LLM agent
is. Don't explain hooks. Don't apologise for the form factor.

**Tone.** Confident, technical, "I built this because I needed it."
Not marketing. Show, don't sell.

**Length.** 2:30 hard cap.

**Cameras.**

- **A** — tight on the AMOLED, 60 fps (scene transitions die at 30).
- **B** — wide of the desk, board next to the keyboard, hand visible
  when pressing the button.
- **C** — screen capture of terminal + Claude Code, monospace ≥ 18pt.

---

## Shot list

| # | Time | Cam | Action | Narration |
|---|---|---|---|---|
| 1 | 0:00–0:08 | B | desk wide; board dark; hands typing | "When I'm watching an agent work I'm staring at the same terminal it's working in. Context switch every prompt." |
| 2 | 0:08–0:18 | A | board wakes to **idle** as bridge starts | "So I built a thing. ESP32, 466-by-466 AMOLED, the agent renders here." |
| 3 | 0:18–0:32 | C → A | start a CC session; device flips to **sessions**, cwd + tool calls populate live | "Claude Code's hooks pipe into a host-side bridge. Per-agent session registry, throttled snapshot push at 4 Hz. Multi-agent — Codex shows up next to it." |
| 4 | 0:32–0:48 | C → A | CC tries to `Bash` something; device flips to **prompt** with the exact command preview | "PreToolUse blocks the agent until I decide. The bridge throws the request at the device. The device throws it at me." |
| 5 | 0:48–0:58 | B | tight on hand pressing **BOOT**; cut to C showing CC unblocking | "Approve once, deny, 60 s timeout. The decision rides back as an EVT line over the serial channel." |
| 6 | 0:58–1:18 | A | quick tour: **tokens** (sparkline + per-agent), **status** (heap, uptime, battery), back to **sessions** with two agents | "Five scenes. Tokens — cumulative, today, sparkline. Status — heap, FPS. Sessions with two agents at once, slot-keyed by kind and session id." |
| 7 | 1:18–1:32 | C | clone the repo; show `examples/01_minimal/run.py` running against the mock | "Line-framed JSON over serial. One verb, one blob, one OK or EVT back. Mock device ships in the repo so you can write a consumer without owning hardware." |
| 8 | 1:32–1:50 | C | scroll `HARNESS_GAPS.md` past G-1 through G-8 | "Building this surfaced eight rough edges in the underlying esp-harness framework. Two are already upstream. Five are in design. The gaps are public." |
| 9 | 1:50–2:08 | B | desk wide; human is heads-down; device shimmers in peripheral vision | "The point of the dashboard is it lives where your eyes already are. You glance, you tap, you don't context-switch." |
| 10 | 2:08–2:22 | A | montage: four scenes back to back, noir → lab → mono theme switch midway | "Three themes ship. Bring your own if you want — theme is a sdkconfig knob." |
| 11 | 2:22–2:30 | A | device returns to idle; soft "zZz" pulse | "MIT licence. Hardware is a forty-dollar Waveshare board, link in the repo. That's the whole thing." |

---

## Delivery

- One take. Don't re-record for fluency; HN smells overproduced.
- Pause between sentences — let the scene transitions breathe.
- Don't say "AI" without a qualifier ("agent", "LLM", "model").
- Don't say "magical", "delightful", or "powerful".

## B-roll to have ready

- 5 s AMOLED in the dark (cold-open / gap filler)
- 5 s finger on BOOT, slow close-up
- 5 s bridge terminal scrolling snapshot logs
- 5 s board in its anti-static bag + cable (for the "forty-dollar" line)
- 10 s sessions scene with two agents bouncing (capture from `examples/02_two_agents/` against a real device)

## Cuts if it runs long

1. Themes (shot 10) — drop entirely.
2. HARNESS_GAPS (shot 8) — voice-over only, no screen cap.
3. Examples (shot 7) — trim to the run line.

**Never cut shots 4 + 5** — the button round-trip is the whole point.

## Thumbnail

Single still: the prompt scene on the device, a hand about to press
BOOT, slightly blurred. Overlay: `your agent, on your desk`. No
subtitle.
