# Persona A — Power-User Usability Report

**Reviewer profile.** 3-4 concurrent CC sessions running 24/7. Custom
`~/.claude/settings.json` with my own hooks (united-memory, dashboard
status line, a bunch of permission allow-lists). Live in tmux + iTerm.
Have a drawer of dev boards I never finished projects with. I am
exactly the person you want to convert, and exactly the person who
will bounce hardest if you waste 10 minutes of my time.

I cloned the repo cold, read the docs front-to-back, and stared at the
screenshots. Below is what I think — unvarnished.

---

## 1. The pitch (README)

**Verdict: sold on the concept, skeptical on the price.**

The README is doing a lot of things right. The one-line tagline
("**A live dashboard for AI-agent sessions on a 466×466 AMOLED panel.**")
plus the immediately-following paragraph nails the "who is this for"
question in under 10 seconds. The four-row layer table (Firmware / Host
bridge / Wire format) is the kind of architectural respect-the-reader
move I wish more hobby repos pulled.

**The sentence that lands:**

> "It lives next to your keyboard and shows what your agents are doing
> without you having to context-switch into the terminal that owns
> them."

That's the actual sales pitch. That is the loop. I have 4 tmux panes
and I can never remember which one is waiting for me. *That* is the
problem this thing solves. Put this sentence higher — it's currently
buried at the top of "What this is" and the actual hero text above it
is more generic ("Watch what Claude Code and Codex are doing on your
desk in real time"). The keyboard-adjacency framing is the killer
detail; the generic one is the safe one.

**The sentence that misfires:**

> "We took the protocol *shape* (JSON-over-line, one verb per state
> class) from claude-desktop-buddy and extended it to handle several
> concurrent agents at once."

This reads like a defensive footnote in an academic paper, in the
middle of the "How it compares" section that's supposed to be selling
me on differentiation. If you took the shape from upstream, fine — but
the framing buries the lede. Rewrite as "Multi-agent from day one,
unlike <X> which is single-session". Lead with the differentiator, not
the lineage.

**Other README issues a busy person hits:**

- The Quickstart says "30 seconds" but step 1 is "clone and pip-install
  a *separate* repo" (`esp-harness`). That is not 30 seconds. That is
  ~5 minutes, plus whatever yak-shaving happens when esp-harness wants
  a specific Python or `cmake` version. Either rename to "Quickstart
  (5 minutes, assuming clean Python)" or bundle/vendor esp-harness so
  step 1 disappears. Lying about setup time is the #1 hobby-repo sin.
- `esp-harness flash --project . --port COM9` — what if I'm on macOS
  or Linux? `COM9` is the most Windows-specific copy-paste I've seen
  in a hardware README in years. At minimum, a comment: `# Linux:
  /dev/ttyACM0, macOS: /dev/cu.usbmodem*`.
- Step 4 ("Wire the bridge into Claude Code") embeds a JSON snippet
  with a hard-coded Windows path. Below the snippet there's no mention
  of the fact that you also need to be running the bridge as a
  long-lived daemon (step 5) — which means *every fresh boot* requires
  manually relaunching `claude_buddy_bridge.py serve` before CC starts
  spitting events. No `systemd` / `launchd` / Task Scheduler example.
  This is the silent-fail trap. Cover it.
- The "How it compares" table is good but the third row says transport
  is "USB-Serial (v0.1) → BLE NUS (v1.0) → WiFi (v2.0)" while the
  Status/Roadmap table immediately below says BLE is **v1.0 in design**
  and WiFi is **v2.0 sketched**. Those are pretty different
  commitments. As a buyer, "in design" and "sketched" both mean "not
  shipping this quarter, don't plan around it". Be straight: today
  USB-Serial only.
- 5 README badges is one too many. Drop the LVGL or the ESP-IDF badge;
  they're not actionable for the reader.

**Net.** The README knows what it is and the architecture map is
beautiful. It oversells onboarding speed and undersells the keyboard-
adjacency framing. Tighten and you've got a great README.

---

## 2. The protocol — would I trust this with my hooks?

Read `PROTOCOL.md` and `docs/DASH_STATE_CONTRACT.md`. Specifically
read them through the lens of *would I add these hooks to my live
`~/.claude/settings.json` without segregating to a test profile first?*

**Honest answer: I'd hesitate for ~5 reasons.**

1. **`PreToolUse` blocks for up to 60 s on a hardware button press.**
   This is the headline feature, but also the thing that makes me
   nervous. Right now my `PreToolUse` hooks finish in <50 ms. The
   bridge can stall CC for a full minute waiting for a *physical
   button* on a *USB device* I might have unplugged because I needed
   the port for something else. The README doesn't tell me what
   happens when the bridge daemon is up, the device is offline, the
   serial port is dead — does the hook return `{continue: true}` after
   the full 60s timeout? Does it fail closed (deny)? The bridge code
   `PROMPT_TIMEOUT = 60.0` in `hook_dispatch.py` plus the comment
   "60s timeout falls through to 'deny' by default" in the README
   give *opposite* answers. **Make this contract loud and obvious in
   PROTOCOL.md.** Right now I'd read the code to figure it out, and
   I'm fast at reading code, and I still don't entirely trust my
   answer.

2. **`hook_dispatch.py` reads `transcript_path` and slurps the whole
   file into memory on every `Stop` event.** Look at
   `_read_last_assistant_text`: `lines = f.readlines()`. CC transcript
   files can grow to many MB on long sessions. On every single `Stop`
   you're allocating the entire file. Fine for a 30-minute session,
   not fine for a 12-hour CC marathon. I want a `tail`-style reverse
   reader, or at minimum a size cap with a warning.

3. **The dash-state extraction regex is END-anchored with DOTALL.**
   `<dash-state>\s*(.*?)\s*</dash-state>\s*$`. If the agent emits the
   block then keeps writing (e.g., a hook injects a follow-up notice),
   the regex silently returns None. The contract doc says "Block must
   be at the END" but the protocol gives no error feedback to the
   agent when it isn't. Silently dropping is the wrong default for an
   instrumentation system. The bridge should log "found dash-state
   block but it wasn't last" at INFO.

4. **No allow-list / sandbox on the agent kind field.** The bridge
   reads `payload["agent"]` from the hook command-line arg (defaults
   to `"claude-code"`) but the wire-format protocol only documents
   `claude-code | codex | other`. If a malicious or buggy hook passes
   `agent=../../../etc/passwd`, what happens? Looking at the dispatch
   code, this lands in a JSON dict that goes to localhost-only TCP
   so it's not a security boundary per se — but the device might
   render the kind as a label and overflow a fixed buffer. (LVGL's
   `lv_label_set_text` does its own bounds-check, fine, but
   defense-in-depth costs you 3 lines of `if kind not in
   {...}: kind = "other"` in the bridge.)

5. **The 5-hook install pattern means a busted bridge breaks every CC
   command on the machine.** Yes, `_passthrough()` returns
   `{continue: true}` on ConnectionRefused — *good*. But the 1.0 s
   connect timeout plus the 5.0 s read timeout means that every hook
   waits up to 6 seconds before deciding the bridge is dead. Imagine
   the bridge daemon hangs (not "down" — *hung*, accepting the connect
   but never replying). Now every Bash, Edit, Read, Grep in CC eats
   5 s of latency. That'd murder my workflow inside one session. I
   want a **circuit breaker**: if `N` events in a row time out,
   short-circuit to `_passthrough` immediately for the next `M`
   seconds. This is a hardening gap that *will* bite real users.

**Specific friction in DASH_STATE_CONTRACT.md.**

- The "Why a markdown-style block (not JSON / TOML)" justification is
  good and I agree with it. But the regex `<dash-state>...</dash-state>`
  is going to collide with someone's actual XML/HTML someday — pick a
  more namespaced sentinel like `<dash:state>` or
  `<!-- @dash-state -->`. Adding a non-existent custom tag to your
  natural-language output is fragile; if CC ever renders that block
  to the user (and it will, eventually, with markdown extensions or
  preview), it'll look weird.
- Spec says `summary: 60-200 chars` but `hook_dispatch.py` truncates
  to `[:240]` and `PROTOCOL.md` says wire-size cap is `≤ 200`. Three
  numbers, three sources, two disagree with the third. Pick one.
- "Empty options are dropped" — but the bullets in the example only
  show `-`, while the parser also accepts `*` and `•`. Add `*` to the
  doc.
- No spec for what happens with **5+** options. Parser silently drops
  past 4; doc says max 4. Make it explicit and surface "agent emitted
  N>4 options, dropped tail" in the bridge log.

**Would I install the hooks today on my live profile?** No.
On a fresh CC profile, yes. The circuit-breaker missing is the only
hard blocker for live-profile use — everything else is
nice-to-fix-soon.

---

## 3. The dash-state contract — would I emit these from my own scripts?

**Yes, this is the right shape, and I would absolutely emit it from my
own agents.** The markdown-fenced block is the right call: it's
parser-friendly *and* human-readable in the conversation transcript.
JSON would have been a worse choice for exactly the reasons the doc
explains. Good design.

**What I'd change before adopting it elsewhere:**

1. **Drop the `summary` continuation tolerance.** The current parser
   says "Tolerate freeform lines before `options:` only as summary
   continuation if summary not yet set." This is a YAGNI feature
   that complicates the spec and the parser. The spec is clear:
   one-line summary. Make freeform-before-options an *error*
   (well — a silently-dropped line, with a debug log). Don't reward
   sloppy emitters.

2. **Make the agent declare its `kind`.** Right now the bridge
   classifies based on the assistant text via heuristics (line 41
   of the contract doc mentions a "classifier"). That's fine as a
   fallback but it's a tax the agent doesn't need to pay if it
   already knows what kind of turn it is. Add `kind:` to the
   block (the doc even acknowledges this in "Future revisions"):
   ```
   <dash-state>
   kind: pick
   summary: Decision: how to migrate?
   options:
     - inline migration
     - defer to next sprint
     - abort the feature
   </dash-state>
   ```
   Make it ship in v2.5 not "future". Heuristic classifiers are
   debt.

3. **Add an `id`.** Each block should have an optional
   `id: <stable-hash>` so the bridge can dedupe. Long CC sessions
   often have the agent end the turn the same way twice in a row
   ("status check") and the device shouldn't re-animate the
   AWAITING takeover for an identical block.

4. **Spec the option text constraints.** "8-32 chars" is too loose.
   What about emoji? RTL? Trailing periods? Auto-numbered prefixes
   (the agent might helpfully write "1. ship it" and now there's
   a double-numbering bug). Pin it down — verb-first, no leading
   numbering, no punctuation, no emoji. Or explicitly allow them.

5. **Future-proof for streaming.** If you ever want the dashboard to
   reflect the agent's *current* state mid-stream (not just on Stop),
   you'll want a `<dash-state-streaming>` variant or a
   `partial: true` flag. Worth thinking about now while the format
   is malleable.

6. **The numbered-option-as-prompt trick is genius but undocumented
   as a CC-runtime behavior.** "The user types `1` or `ship v2.4.0
   too` in the terminal — both work because Claude Code accepts the
   option as a prompt verbatim." Wait, does CC actually do that?
   Typing "1" doesn't send "ship v2.4.0 too" unless the agent is
   told to interpret it that way. This sounds like a contract the
   agent has to be *prompted* to honor. Document the prompt
   side of this — there should be a system-prompt snippet like
   "When the user replies with just a number 1-4 and your last
   message contained a `<dash-state>` block with options, treat
   their reply as if they typed option N verbatim." Without that,
   typing "1" does nothing, and the whole "multiple-choice not
   fill-in-the-blank" promise falls apart.

**Format verdict: yes, I'd emit it.** I'd want the above clarifications
shipped, but the shape is right.

---

## 4. The screenshots — roasting time

### `live-ambient-v251.png` (the ambient feed)

What I see: a 466x466 panel. Top section: oversized `00:11` clock with
"Clawd" device-name as a centered subtitle. Middle: 6-row feed of
tool-call history. Bottom: split footer with a teal "2 active" and a
white "2.1k tokens today".

**What's good:**
- The minimalist aesthetic is on-brand and easy on the eyes.
- The teal accent for the count is the right amount of color.
- Mono-style table for the feed is correct for a status surface.

**What I'd file bugs for:**

- **P0: Bash tool icons aren't icons.** Every row is plain text:
  `Edit  00:11  ok  cc x_a3  src/auth.py +8 -2`. The README and
  PROTOCOL both promise tool *icons* ("Device may render an icon
  based on it (Bash/Edit/Read/Grep/Write/...)"). Either ship the
  icons or update the spec. Right now the tool name is
  visually-prominent text that wastes a *huge* chunk of the row
  width — Edit, Bash, Grep all rendered at maybe 28pt while the
  actual *content* (the file + diff) is at 16pt. The information
  hierarchy is inverted. **What I care about is "what changed in
  what file" — that should be the loudest thing.** Right now the
  loudest thing is the verb. Wrong priority.

- **P0: The clock is too big and not load-bearing.** `00:11` at what
  looks like 80pt is using 25% of the panel for information I get
  on my menu bar, taskbar, watch, phone, and microwave. The only
  reason a wall-dashboard shows a clock is *contextual stamping*
  ("when did the last event fire"), and you already have per-row
  timestamps. Cut the clock to a tiny eyebrow or drop it. Use the
  reclaimed space for either (a) 2-3 more feed rows, or (b) an
  active-session pip indicator.

- The "Clawd" subtitle below the clock is wasted ink. I named the
  device. I know what it's called. Eyebrow text should be
  context-changing info — most-recent-event timestamp, active agent
  count, *something*.

- The feed list has redundant `ok` markers on every row. If every
  row is `ok`, the column adds no information. Reserve the column
  for *non-ok* states (red dot for fail, yellow for warning).
  Otherwise drop it.

- `cc x_a3` and `cx x_b1` are agent IDs. Why do I care about the
  hash suffix? It's a 5-character random string that means nothing
  to me. Use the cwd's basename instead (`auth-py`, `migrate-db`) —
  same width, infinitely more meaningful at a glance.

- Two consecutive "Edit  src/auth.py +8 -2" rows. Are these two
  separate edits, or the same edit logged twice? If duplicate-collapse
  is intended for the device, ship it. If not, this looks like
  a bug in the feed.

- Footer: "2 active" in teal, "2.1k tokens today" in white. The
  inconsistent prominence between two equally-relevant stats is
  jarring. Pick: either both glanceable color-coded or both muted.

### `live-awaiting-options.png` (the your-turn screen)

What I see: the marquee summary is **clipping at both ends** —
"ind variants on real device   Shipped v" is visible, which means
the start of the summary ("...mind variants...") and the end
("Shipped v[ersion 2.4.0]" or whatever) are both cut off mid-word.

**THIS is the P0 visual bug I'd file.**

A marquee that scrolls is fine. A marquee that's caught mid-scroll
in a static screenshot, *with both ends clipped through the
device bezel area*, looks broken. Either:
1. The actual rendering pads the marquee with start/end whitespace
   so the static moment doesn't show mid-word clipping, or
2. The marquee text fits without scrolling, and you center it, or
3. The marquee text exceeds the panel width and you fade it out
   at both edges with a gradient mask (this is the LVGL idiom).

The screenshot as shipped looks like a render bug. Bad first
impression for the headline feature.

Other issues with `live-awaiting-options.png`:

- The agent chip "cc 0_demo" is teal at what looks like 32pt while
  "your turn" is white at 56pt. The agent chip is the wrong color
  to be the second-loudest element — it's noise, not signal. The
  user already knows this is their turn; they need to scan to the
  *options* fast. Demote the chip, promote the options.

- Numbered options 1-4 are left-aligned with two spaces between
  number and text ("1.  ship v2.4.0"). The optical center of
  this list isn't where the headline's optical center is. Either
  align both on the panel's vertical centerline or commit fully
  to left-rag. The half-measure looks accidental.

- The center pulse glyph (teal ring with dot) consumes a quarter
  of the screen vertical real estate and adds no information that
  "your turn" doesn't already convey. It's decoration competing
  with content.

- "0_demo" as a session ID is the kind of placeholder text that
  shouldn't be in marketing screenshots. The earlier `cc sx4`
  example in the strip is better.

### `live-awaiting-strip.png` (the 5 awaiting variants)

Five panels side-by-side: your-turn, approve, pick-one, type-a-reply,
clarify. Each with its own icon and accent color (teal for
neutral/pick/type, amber for approve/clarify).

**This strip is the best image in the docs.** It immediately tells
me what state the device can be in, what the icons mean, and how
the accent color signals urgency. The icon vocabulary is
consistent (target / warning / grid / pencil / bell). I'd put this
strip near the top of the README, above the table.

But:

- The summary text under each headline is also clipping —
  "All set. Refactor of src/auth.py is committed" on the leftmost
  panel is cut off at "All" on the left (look at the left edge
  of the leftmost panel — the "A" of "All" is starting at the
  bezel). Same marquee-clipping bug as #2. Once again: static
  screenshot of a scrolling element looks broken.

- The "type a reply" panel is suspect. If the user has to *type*
  the reply, the buttons (BOOT approve / USER deny) don't work
  for that scene — what's the button behavior? Does the user
  walk over to their keyboard? This scene is begging for an
  explanation that isn't in the doc.

- The icon palette is good but the warning triangle for "approve"
  feels heavier than the bell for "clarify". Approve is the
  baseline routine path (`Bash: git push --force` — hmm, that one
  *should* be heavy actually). Reconsider the icon for the
  ambiguous-clarification case — a question mark would beat a bell.

- Footer "waiting 4s / 5s / 3s / 4s" is a nice touch. Could it tick
  up live with subtle animation? In a screenshot you can't tell,
  but worth thinking about: a slowly-ticking number adds urgency
  better than a static one.

### Typography overall

The headline weight is good. The body weight is reasonable. But the
**numeric monospaced rendering is inconsistent**: `00:11` in the
clock uses tabular figures (correct) but `2.1k` in the bottom-right
uses proportional figures (visible from the narrower "1" between
"2" and ".1"). Pick one and stick. Tabular for status surfaces.

### P0 visual bug to file

The **marquee mid-scroll text clipping in both the strip and the
options screenshot is the single most professional-looking-broken
thing about the ship**. Two of the marketing screenshots show
clipped mid-word text. File P0 against marquee rendering: pad
start/end with min(half-width) whitespace OR apply edge-fade mask
gradient so static frames never show mid-word cuts. This is the
"oh no someone shipped this with a placeholder" reaction.

---

## 5. The roadmap — does v3.0+ make sense?

The roadmap as written goes to **v2.2** (web mirror, Tauri tray app),
not v3.0+. So I'll evaluate "v0.2 → v2.2" since that's what's there.

**Density verdict.** 20 versions of solid value in a single roadmap is
ambitious to the point of being LARP-y. The "Mantra: tokens are
infinite fuel — use parallel subagents aggressively" framing in the
opening is endearing if you're vibing with the author and unsettling
if you're a reviewer who's seen this pattern fail. Roadmaps with
21 milestones tend to ship 4-6 of them and silently abandon the rest.
**I'd cut this to 8 milestones and over-deliver on each.**

**What's missing:**

1. **No "v1.0 release-quality" bar that's actually about quality.**
   The v1.0.0 row says "Public RC. All prior versions stabilised."
   This is the lowest-information row in the whole table. Replace
   with a concrete *what does "stable" mean* checklist: e.g. "device
   handles 100M snapshot cycles without OOM", "bridge survives
   `kill -9` + restart with zero lost events", "panic-free under
   fuzzer", "battery life >7 days idle", "USB-Serial reconnect
   <2s after replug". Right now v1.0.0 is a marketing milestone, not
   an engineering one. **A power-user wants to know what makes
   v1.0 v1.0.**

2. **No "test coverage / chaos / fuzz" milestone.** v0.3 has
   "adversarial primitive" which is closer but framed as a tool to
   *build*, not an outcome to *hit*. Add: "v0.5: zero P0 bugs found
   by 24h adversarial soak". Quality gates without quantification
   are theater.

3. **No "what happens when the bridge crashes mid-prompt" story.**
   The whole roadmap is feature-additive. Where's the resilience-
   ratchet? "Bridge recovers from process crash without dropping
   any pending CC permission request" is a v0.2 must-have, not a
   v1.7-power-management thing.

4. **The agent-role section ("the 'team' I'm hiring") is fun and
   I get the LLM-orchestrator-vibe, but it's not a roadmap, it's
   a casting call.** As a reader I want to know: are these roles
   *real subagents you're spawning*, or are they fictional roles
   for the author to put on different hats? If real, link to the
   subagent definitions / system prompts. If fictional, this
   section belongs in a `MANIFESTO.md` not in `ROADMAP.md`.

5. **No "drop a feature" item.** No roadmap is honest without a
   "things we considered and won't do" section. What's *not* in
   scope? Voice (v1.4) and on-device AI (v1.9) feel particularly
   speculative for a device with limited PSRAM and a constrained
   power budget — but they're listed alongside boring-but-needed
   work like power management (v1.7). Triage these.

6. **Missing: companion CLI for the device.** A `dash-cli` that lets
   me `dash-cli send "hello"` from my terminal would be huge for
   testing and for one-off "send a note to my dashboard" use cases.
   Doesn't appear anywhere in the 21-row table.

7. **No "ecosystem / docs site polish" milestone before v1.0.** The
   gh-pages site is in the README badges but isn't anchored in the
   roadmap. Polish that *before* the v1.0 launch, not after.

8. **The off-roadmap principles section is great** ("framework
   benefits the consumer benefits the framework") — keep that, it's
   the actual signal in this doc.

**v3.0+ direction (the question asked).** If I were planning v3+:
networked fleet (one bridge → N devices around the office),
multi-user sessions (multiple humans approving), and protocol-as-spec
(publish PROTOCOL.md as an open RFC, get other agent vendors to emit
the wire format). The current roadmap teases multi-device fleet at
v1.2 but doesn't push it to "ecosystem" stage. v3+ should be about
*adoption*, not features.

---

## 6. Hooks integration — would I install this?

I looked at `tools/hook_dispatch.py` and at my live
`~/.claude/settings.json`. Quick observations:

- **Hook dispatch is correctly minimal.** No dependencies, ~180 lines,
  fails open on bridge unavailable. Good architecture.
- **The bridge swallows pgrade-pain** but only in the catch block. If
  the bridge daemon is *up* but the device serial port is wedged, the
  bridge accepts the connection, queues the prompt, the device never
  responds, and CC hangs for 60s. Need an end-to-end "device alive"
  health check, not just bridge-alive.
- **No `Notification` hook handler.** CC has a `Notification` event
  that fires when the agent is *waiting for the user without a tool
  call* (e.g., asking a clarifying question). This is *exactly* the
  AWAITING / your-turn case. If the dispatch doesn't subscribe to
  `Notification`, the AWAITING takeover only fires on `Stop`. That's
  half the use cases.
- **The 5-event hook footprint** (`SessionStart`, `UserPromptSubmit`,
  `PreToolUse`, `PostToolUse`, `Stop`) is a lot. Each one is a Python
  cold-start. On my machine that's ~80ms per invocation. PostToolUse
  fires on *every* tool call. So a CC session with 50 tool calls eats
  4 seconds of pure Python cold-start latency. **Make the dispatch
  optional per-event** so I can opt into just `PreToolUse` + `Stop`
  if I don't care about real-time feed updates.
- **The example in the README uses bare `python`** but the user's
  actual installed config calls `D:/Code/esp-harness/tools/esp-harness/.venv/Scripts/python.exe`.
  Make the README example reflect a venv pattern; bare `python` will
  pick up whatever's first on PATH and break.
- **No `disable` mechanism.** If I want to temporarily mute the
  dashboard (because my kid is sleeping next to my desk and the OLED
  glows blue), I have to comment out 5 settings.json blocks and
  restart CC. Want: `dash mute` console command + env var
  `CLAUDE_BUDDY_DISABLED=1` that makes `hook_dispatch.py` short-
  circuit to `_passthrough` immediately.

**Friction summary.** Installation is two-pieces: edit settings.json
(easy, 5 lines) + run bridge as long-lived daemon (medium, needs
service config). The "easy" part is well-documented. The "medium" part
is not. **The single thing that would move my install decision from
"after I finish this CC session" to "right now" is a one-shot
installer**: `python tools/install.py --service` that writes the
hooks, registers the bridge with launchd/systemd/SchTasks, and
verifies a round-trip to the device. Without that, every user
re-derives the same yak from the README.

Would I install it on my live profile? Today: no (the circuit-breaker
issue from §2 is real). After §2 is fixed: yes.

---

## 7. Top 3 P0 findings

Ranked must-fix-before-shipping-to-strangers:

### P0-1: Hook dispatch has no circuit breaker against a hung bridge

**Where.** `tools/hook_dispatch.py`, every event-type path.
**Why.** A hung (not down — accepting connections but not replying)
bridge causes every CC tool invocation to wait up to 5s (60s for
`PreToolUse`) before failing open. With ~50 tool calls per session
this is catastrophic latency. The `_passthrough` fallback only
fires on `ConnectionRefusedError | socket.timeout | OSError` — it
does *not* fast-path on "bridge has misbehaved 3 times in a row".
**Fix.** Add a counter file (or shared in-process state via a tiny
unix socket / named pipe) that tracks recent failures. After N
consecutive timeouts within W seconds, short-circuit to passthrough
for the next M seconds. Print the trip event so users notice.

### P0-2: Marquee text clipping in marketing screenshots

**Where.** `docs/img/live-awaiting-options.png` and
`docs/img/live-awaiting-strip.png`.
**Why.** Both screenshots show mid-word text clipping at the panel
edges where the marquee is mid-scroll. This is the first impression
the README gives to potential users. It looks like a render bug.
Either the rendering really is buggy (high priority firmware fix)
or it's a screenshot-capture timing issue (high priority docs fix).
**Fix.** Implement edge-fade gradient mask on marquee scenes
(LVGL idiom: `lv_obj_set_style_bg_grad_dir`) AND re-capture the
screenshots at moments when marquee text fully fits OR at
pause-between-loop frames so static images never show mid-word
cuts. The strip image needs to be re-shot; the options image
needs the actual rendering fixed.

### P0-3: Ambient feed information hierarchy is inverted

**Where.** `docs/img/live-ambient-v251.png` (and presumably the
firmware that generates it).
**Why.** Tool names (Edit, Bash, Grep) are rendered as the largest
text on the row, while the actual change content (file + diff) is
smaller and more muted. The clock at the top uses 25% of the panel
for non-load-bearing info. Glanceable status surfaces work because
*the most important thing is the loudest thing*. Right now the
loudest things are duplicated verbs and a redundant clock.
**Fix.** Compose feed rows as: small icon (24pt) + bold filename or
context (28pt) + dim diff/result (20pt) + tiny timestamp (16pt).
Drop the clock or shrink to a 14pt eyebrow. Use the reclaimed
vertical space for 2-3 more feed rows. The teal accent should
fall on *most-recent / active* rows, not on the "2 active" count.

---

## Closing notes

This project is good. It's *almost* shippable to power users. The
architecture is right, the protocol is the right shape, and the
contract design is grown-up. The work between here and "I'd recommend
this to my team" is mostly hardening (circuit breakers, error
paths, marquee polish) and a 30% README tightening pass. Six weeks
of focused work, not six months.

What I'd do next:
1. Fix P0-1 (circuit breaker) — 2 days.
2. Fix P0-2 (marquee + screenshots) — 1 day.
3. Fix P0-3 (feed hierarchy) — 2 days.
4. Add `Notification` hook support — 1 day.
5. Ship a one-shot installer — 2 days.

Then come back and pitch me again. I'll be using it.

— Persona A
