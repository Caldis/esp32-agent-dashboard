# Persona B — Newcomer fresh off the HN front page

> **Who I am.** Fullstack JS/Python dev, 6 yrs. I have a terminal, Claude
> Code, Python, and Git already installed. I have never run `idf.py`,
> never flashed an ESP32, do not own a soldering iron. I just opened the
> Waveshare box, plugged the board in over USB-C, and the demo
> rotating-sphere thing is glowing at me. I have ~30 min before my next
> meeting. Let's go.

---

## 1. First impression — `README.md`

**Visual scan, ~10 s.** The hero image, badges, and the comparison
table all read very well. I trust this is a real project. The pitch
("watch what Claude Code is doing on your desk, approve with a
button") landed inside the first paragraph. Good.

**Then I scroll to "Quickstart (30 seconds)".**

> This is where I started squinting.

Counting what the "30-second" quickstart actually asks of me:

| Step | What it really is | Realistic time |
|---|---|---|
| `git clone esp-harness` + `pip install -e` | Install a Python toolchain I've never heard of | 1 min if pip is fast |
| `git clone esp32-agent-dashboard` | Fine | 10 s |
| `esp-harness build --project .` | A full ESP-IDF C build. First build is **5–10 min** (this is even stated in GET_STARTED) | **5–10 min** |
| `esp-harness flash --project . --port COM9` | Needs me to know what a COM port is and which one I have | 30 s + lookup |
| Edit `~/.claude/settings.json` | JSON merge — I might already have one | 1–2 min |
| Start the bridge | Easy | 5 s |

So the quickstart is honestly a **~15–20 minute path**, not 30
seconds. The label "30 seconds" sets me up to feel like I'm failing
when I'm 3 minutes in and `esp-harness build` is still chugging. I
would rename this section honestly: e.g. *"Quickstart — 15 minutes
end-to-end, mostly the first build"*. Honesty here builds trust;
exaggeration burns it.

**Second snag.** The quickstart doesn't say *"you need ESP-IDF v6+
installed first"*. The badge says `ESP-IDF v6.0+` but it doesn't read
as a prerequisite — it looks like metadata. I would happily run `pip
install -e esp-harness/tools/esp-harness/` thinking that *is* the
toolchain. Then `esp-harness build` fails with some IDF error I can't
parse and I bounce to Google.

**Third snag — the no-board fallback contradicts itself.** The
README's "No board?" callout uses:

```
python docs/mock_device.py --port 9876 &
```

`GET_STARTED.md` step 3 (and `examples/01_minimal/run.py`'s docstring)
both use:

```
python tools/mock_device_v1.py --port 9876 -v
```

Both files exist on disk (I checked). A newcomer who copies one from
README and then follows GET_STARTED will end up with two mocks open,
or — worse — assume one of them is broken because they tried the
"wrong" name. **Pick one.** The `v1`-suffixed one in `tools/` is the
one the examples directly reference; I would deprecate `docs/mock_device.py`
or make it a shim that prints a deprecation pointer.

**Net verdict on README quickstart.** Good marketing copy, but the
*30-second* claim is the single biggest credibility cost on the
landing page. Without ESP-IDF preinstalled, that step is unbounded.

---

## 2. The hardware guide — `docs/HARDWARE_GUIDE.md`

**This is the strongest document in the repo for me.** Honest.

- The BOM is one line, with an explicit "$0 if you already have a
  USB-C cable" callout. That's exactly the friction-reducing tone a
  newcomer needs.
- The labelled SVG diagram (`hardware-board-layout.svg`) actually
  renders — I opened it. It shows BOOT/USER/RST positions clearly,
  with colour coding that matches the prose.
- The polarity warning ("you will let out the magic smoke") earns
  trust because it acknowledges a real failure mode that an
  unmotivated docs writer would skip.
- The 5-step first-power-on flow is concrete, ends with a verifiable
  `curl` round-trip, and points at the next doc. Excellent.

**What's missing for a newcomer, though:**

- **Nowhere does it say "this guide assumes ESP-IDF v6+ is already
  installed."** Steps 3 and 4 (`esp-harness flash`, `claude_buddy_bridge.py
  serve`) silently require it. Same gap as the README.
- **What does "factory firmware" actually look like?** Step 1 says
  *"the AMOLED should illuminate immediately to the Waveshare default
  demo"* — but a newcomer doesn't know what that demo *is*. A small
  inline photo of the rotating sphere (or whatever ships in 2026)
  would let me verify "yes, that's the expected baseline" before I
  start tearing my hair out.
- **The curl smoke-test in step 5 uses `127.0.0.1:7321/snapshot`** —
  but the bridge starts on USB-Serial mode by default. Is the HTTP
  endpoint always exposed, or only with a flag? The README and the
  troubleshooting guide both treat `:7321` as the canonical port, but
  this is the first place a newcomer is told to hit it with curl, and
  I don't know whether `serve --serial-port COM9` automatically
  exposes HTTP, or whether I need a second `--http-port` flag. Quick
  clarifying sentence would close that gap.

Otherwise: this doc would alone get me through unboxing → idle scene.

---

## 3. Get Started — `docs/GET_STARTED.md`

I read this expecting it to fill the gap. It mostly does. But here's
where a newcomer like me **actually gets lost.**

**Step 0 (the assumption).** The doc opens with three assumptions:
*you have Claude Code, you have ESP-IDF v6+ (`idf.py --version`
returns something), you have hardware*.

Then it says, in a smaller line:

> No ESP-IDF? Install it first via Espressif's [Get Started] — ~20
> minutes, not repeated here.

**This is where I get lost first.** I'm now staring at a 30-minute
tutorial that says "first, do an unscoped 20-minute tutorial on a
different site, but trust us that it's 20 min". A newcomer who
clicks that Espressif link discovers it's actually:
- choose an installer flavour (Windows / VS Code / manual),
- pick an IDF version (only v5.x is on the default download page in
  most regions),
- install Python + a 1.5 GB toolchain,
- run `install.bat` / `install.sh`,
- run `export.bat` / `. ./export.sh` **in every new terminal**
  forever.

That last point — IDF needing `export.sh` per shell — is invisible
here. I will get all the way to step 4, run `esp-harness build`, get
a "command not found" or a confusing CMake error, and have no idea
that I forgot to source `export.sh`. **This is the #1 first-boot trap
for ESP-IDF newcomers and it's not in your troubleshooting guide.**

**Step 3 (mock round-trip).** Good. Two terminals, a clear
success line, exit code 0. This is the kind of low-stakes verification
that builds confidence. I like this step a lot — but see the
mock-name inconsistency callout from §1; this doc uses
`tools/mock_device_v1.py`, not the README's `docs/mock_device.py`.

**Step 4 (build + flash).** Realistic about the 5–10 min first build.
Good. But: the flash command silently presumes the COM port. On
Windows the port number is rarely COM9 — it's whatever the OS
allocated on first enumeration, often COM3 / COM5 / COM10+. The doc
points back to HARDWARE_GUIDE step 2 for *how to find it*, which is
fine, but I'd surface the `esp-harness list-ports` hint (only mentioned
in troubleshooting!) here in the body, not by reference.

**Step 6 (hooks).** The `settings.json` block is fine as a snippet
but **glosses over the merge case**. If I already have a
`~/.claude/settings.json` (which I do, because that's what plugins
edit), I need to merge into the existing `hooks` key, not replace the
whole file. The doc doesn't say so. A newcomer who already has CC set
up could overwrite their config by following the snippet literally.

**Step 7 (run CC on something).** Clear. The latency budget (≤ 1 s) is
a useful "is it working?" check. Good.

**First point of confusion, ranked:**

1. **ESP-IDF prerequisite + per-shell `export.sh` ritual** — invisible
   here, fatal in practice.
2. **Mock-device path inconsistency** between README and this doc.
3. **`~/.claude/settings.json` merge semantics** — not discussed.

---

## 4. Troubleshooting — `docs/TROUBLESHOOTING.md`

I tried to imagine **three things that could plausibly go wrong on
my first boot**, then checked whether the doc covers them.

| My imagined failure | Covered? | Notes |
|---|---|---|
| **a.** I ran `esp-harness build` and got `idf.py: command not found` (forgot to source `export.sh`) | **No** | Closest entry is "build fails with cJSON" — that's a v6 migration issue, not a fresh-install issue. The single most common newcomer ESP-IDF failure isn't here. |
| **b.** I'm on Windows, Device Manager shows COM9 but `esp-harness flash` says "could not open port" | Partly | "COM port not found" covers the case where the OS doesn't see the board, but not the case where the OS *does* see it but esp-harness can't claim it. On Windows this is often the Espressif IDE / a terminal / PuTTY still holding the port — a 30-second mention would save hours. |
| **c.** I followed README quickstart literally and ran `python docs/mock_device.py` but examples reference `tools/mock_device_v1.py` and they don't behave the same | **No** | This is the mock-name inconsistency from §1. Troubleshooting doesn't mention it because the docs author presumably didn't notice the README→GET_STARTED divergence. |

So 1 of my 3 imagined failures is covered well, 1 partially, 1 not at
all. The entries that *are* there (cJSON, idle scene, button, G-8
tokeniser, factory demo, blank AMOLED, throttling, dialout) all read
as written by someone who actually hit them. The doc has a strong
"this is real" smell. It's just shaped around problems v0.1 *survivors*
encountered, not problems v0.1 *newcomers* will encounter.

**Gap list for newcomer-coverage:**

- ESP-IDF not installed / not sourced (`idf.py`/`export.sh` ritual).
- Port enumerated but locked by another process (Windows-specific).
- `~/.claude/settings.json` already exists and the user replaced
  rather than merged it.
- "How do I know the bridge is actually pushing serial?" — i.e. a
  diagnostic where you watch the wire (e.g. `esp-harness console --port
  COM9` while the bridge runs).

---

## 5. The screenshots — `docs/img/live-ambient-v251.png`

I opened it. Here's what I see:

- A round-ish dark canvas.
- Top: a large `00:11` clock-looking number, with `Clawd` below it.
- A list of seven rows: `Edit / Edit / Bash / Bash / Bash / Grep / Grep`
  with timestamps (`00:11`), `ok` status, a session label
  (`cc_x_a3`, `cx_x_b1`), and a short payload (`src/auth.py +8 -2`,
  `git push master`, `git push ok`, `login 42 hits`).
- Bottom: two big metrics — `2 active` (teal) and `2.1k tokens today`
  (white).

**Is it clear what I'm looking at?** As a visual, yes — it's
elegantly designed, the noir theme reads well, the typography is
restrained. **But it does NOT match what the README promised.**

The README says the dashboard cycles through **five scenes**:
**idle / sessions / prompt / tokens / status**.

This screenshot is none of those. It's called **`live-ambient-v251.png`**.
"Ambient" doesn't appear once in the README. Doesn't appear in
GET_STARTED. The image even mixes things from multiple scenes
(per-tool history rows like *sessions*, big totals like *tokens*) into
a unified view that's not in the five-scene table.

So one of two things is true:
- The "ambient" view is the **real production scene** and the
  five-scene model in the README is stale (v0.1 docs vs. a newer
  build). A newcomer ends up confused: "is the dashboard going to look
  like the README scene strip, or like this screenshot?"
- The "ambient" view is a **prototype / variant** and shouldn't be
  the headline screenshot in user testing.

Either way: **the headline visual and the prose contradict each
other.** A newcomer staring at this image and then reading "5 scenes"
will lose confidence in the docs.

Also worth flagging: "Clawd" instead of "Claude" in the screenshot.
Reads cute, but I had to do a double-take — is this a different agent?
A typo? A nickname? Nothing in the docs explains it. (I suspect it's a
demo-data joke, but a newcomer can't tell.)

---

## 6. Time-to-first-output — minimum steps to see ANYTHING on screen

If I'm honest about *the absolute minimum*, before even integrating
Claude Code:

| # | Step | Wall time |
|---|---|---|
| 1 | Plug the board into USB-C | 5 s |
| 2 | (Implicit) See the Waveshare factory demo | already on |
| **— or, after flashing —** | | |
| 3 | Install ESP-IDF v6+ (not covered here, ~20 min realistic, often more on Windows) | **~20–40 min** |
| 4 | `git clone esp-harness && pip install -e ...` | 1 min |
| 5 | `git clone esp32-agent-dashboard && cd ...` | 10 s |
| 6 | `esp-harness build --project .` (first build) | **5–10 min** |
| 7 | `esp-harness flash --project . --port COMx` | 30 s |
| 8 | See `idle` scene render on AMOLED | done |

**Optimistic, ESP-IDF preinstalled: ~10 minutes.**
**Realistic, fresh laptop: ~30–60 minutes**, dominated entirely by
ESP-IDF setup.

There is currently **no "preflashed firmware image" path** offered. A
newcomer who just wants to *see the device do something* before they
sink an hour into IDF has no shortcut. A `.bin` release artifact +
`esptool.py --port COMx write_flash 0x0 dashboard-v0.1.bin` line in
the README would cut this dramatically — bypass the build, prove the
hardware works, then optionally come back to do source builds. The
roadmap table mentions release tags, and the badges link to a
releases page — so the infrastructure is there. Just expose it.

I would consider this the **single largest UX improvement available
right now** for a newcomer landing from HN. Half of them will not
have ESP-IDF, will not want to install ESP-IDF for a 5-min demo, and
will close the tab.

---

## Where I came close to giving up

Honest answer: **right after I'd skim-read the README and noticed
that "30 seconds" actually meant "first do a 20-minute Espressif
install you didn't sign up for".** That's the make-or-break moment.
If I weren't doing this evaluation, I would have closed the tab and
filed the project under "neat but not for me right now". The hardware
guide and the GET_STARTED doc would have saved me — but only if I'd
clicked further, which the over-promising quickstart actively
discouraged.

The other near-give-up was the screenshot-vs-prose mismatch in §5.
That doesn't make me leave, but it makes me trust the README less,
which compounds with the "30 seconds" issue.

---

## 7. Top 3 P0 findings (must-fix-before-newcomer-can-succeed)

### P0-1 — "Quickstart 30 seconds" is dishonestly framed; ESP-IDF prerequisite is invisible

The README's headline path implies a 30-second copy-paste win, but the
*first command after installing pip deps* (`esp-harness build`) silently
requires a 20–40-minute ESP-IDF v6+ install. Newcomers either:
(a) hit a cryptic "idf.py not found" / CMake error and bounce, or
(b) feel like they're failing when in fact the build is *expected* to
take 10 minutes.

**Fix:** rename to *"Quickstart — about 15 minutes (longer if you
need ESP-IDF)"*, add an explicit "Prerequisites: ESP-IDF v6+ — see
[link]" callout **above** the code fence, and add a single sentence
about sourcing `export.sh` / running `export.bat` every shell.
Bonus: ship a prebuilt `.bin` and a `esptool.py write_flash` one-liner
so the no-build path exists.

### P0-2 — Mock-device path and scene-name vocabulary diverge between README, GET_STARTED, and screenshots

Three concrete contradictions a copy-pasting newcomer will hit:
1. README quickstart: `python docs/mock_device.py`. GET_STARTED step 3
   + `examples/01_minimal/run.py`: `python tools/mock_device_v1.py`.
   Both files exist; they are presented as the same thing.
2. README documents **5 scenes** (idle / sessions / prompt / tokens /
   status). The headline screenshot is called `live-ambient-v251.png`
   and shows an "ambient" view that's none of the five.
3. The screenshot uses `Clawd` as an agent name with no explanation.

Trust in the docs takes a steep cut every time these mismatch.

**Fix:** consolidate to one canonical mock path (`tools/mock_device_v1.py`).
Decide whether "ambient" replaces "sessions" or is a new scene, then
update the README's scenes table + image map. Either drop the "Clawd"
demo string or add a one-line aside calling it out as a demo
nickname.

### P0-3 — Troubleshooting doesn't cover the three failures newcomers actually hit

The current troubleshooting doc reads like a survivor's diary — it
covers cJSON migration, G-8 tokeniser bugs, throttling. All real,
all great. But the three failures a *first-boot newcomer* will
actually hit are missing:

1. `idf.py / esp-harness build`: command not found / CMake error
   because IDF wasn't sourced this shell.
2. Windows: port enumerates but is locked by another process
   (IDE, PuTTY, prior bridge instance) — `flash` fails with
   `could not open port`.
3. `~/.claude/settings.json` already exists and the user replaced it
   wholesale instead of merging the `hooks` block.

**Fix:** add three short sections to TROUBLESHOOTING.md at the top
(before cJSON), each with the symptom verbatim, the diagnosis, and a
two-line fix. These three would close ~80% of the realistic
first-attempt support load.

---

## Things that worked very well (so we don't only complain)

- `docs/HARDWARE_GUIDE.md` is genuinely excellent: BOM table, polarity
  warning, 5-step verifiable flow ending in curl, a real SVG diagram.
- The comparison table in the README ("How it compares") earns trust
  fast — it acknowledges prior art instead of pretending to be alone.
- The mock-device escape hatch (any of them — see P0-2) is a
  thoughtful inclusion. *Having* a no-hardware fallback is rare and
  good. Just unify it.
- The troubleshooting doc's entries that *are* present are written
  in the right shape (symptom → cause → fix → diagnose-with-four-
  commands). When the newcomer-targeted entries are added (P0-3), the
  shape is already there for them.

— end —
