# Persona C — The Skeptical Desk-Mate

I sit across the desk from someone who recently put a small black screen
next to their monitor. I have no idea what an "AI agent" is. I don't
know the codebase. I just keep catching this thing in my peripheral
vision and wondering: *what is that, and is it going to keep doing the
thing it just did?*

This is my honest reaction to the two screenshots and the five-variant
strip, with the caveat that I'm an ordinary office worker, not a
designer or an embedded engineer.

---

## 1. Glanceability — what I absorb in 2 seconds

### `live-ambient-v251.png` (the resting / feed screen)

**At 50 cm, counting to two:**
- A big number: **00:11**. My first instinct is "that's a timer" or "it's
  11 minutes past midnight". I genuinely cannot tell which.
- The word **Clawd** under the number. I read it as a typo for "Cloud"
  and then as a name. I do not know it's an agent.
- A wall of six rows that look like a spreadsheet: `Edit / Bash / Grep`
  repeating, identical timestamps `00:11`, identical-looking gibberish
  (`cc x_a3`, `cx x_b1`), and short tails like `src/auth.py +8 -2`.
- Down at the bottom: a teal **2** with the word "active", and a white
  **2.1k** with "tokens today".

The thing my eye actually lands on first is **2.1k tokens today** —
because it's the only bright-white number in the bottom half and it sits
alone. The big `00:11` at the top is so large it stops being a number
and starts being decoration; I don't process it as information.

**At 2 m, glance:**
- A clock-ish number at the top.
- A vertical column of dim grey text I cannot read.
- Two small bright glyphs at the bottom — a teal one and a white one.
- General impression: *"some kind of log viewer"*.

The middle 60% of the screen — the entire activity feed — is unreadable
from across the desk. From 2 m the rows blur into a single grey block.
That's a lot of pixels spent on something I can't use.

### `live-awaiting-options.png` (the "your turn" with a list)

**At 50 cm, 2 seconds:**
- I see the words **your turn** — huge, bright, unmissable.
- A teal circle-with-a-dot above it, like a record button.
- A numbered list of four short phrases: "ship v2.4.0", "polish
  marquee", "rewrite dashboard", "take a break".
- I get the gist: *"the screen wants my colleague to choose one of
  four things."*

This screen is much better than the ambient one. The hierarchy is
strong, the call to action is obvious, and the list is short enough
to scan.

**Problems I still notice in 2 seconds:**
- A line of grey text reading "ind variants on real device   Shipped v"
  is **clipped on both sides**. As a bystander this is the single most
  distracting thing on the screen — clipped text reads as a bug, and
  bugs are stickier than features.
- "cc 0_demo" under the headline means nothing to me. I assume it's
  internal jargon. It earns prime real estate (right under the
  headline, in the accent colour) but communicates nothing to an
  outsider.

**At 2 m:** I can read **your turn**. Everything else is mush. That is
actually the correct outcome for a takeover screen — if my colleague is
needed, the room should tell them. Good.

---

## 2. The "your turn" takeover — five variants

Viewed left-to-right in `live-awaiting-strip.png`:

| Variant | Headline | Icon | Immediately obvious? |
|---|---|---|---|
| 1 | **your turn** | teal target / record dot | Yes — "do the thing" |
| 2 | **approve?** | gold warning triangle | Mostly — "say yes/no" |
| 3 | **pick one** | teal grid icon | Yes — "choose" |
| 4 | **type a reply** | teal pencil | Yes — "write something" |
| 5 | **clarify** | gold bell | **No — I have no idea what to do** |

**Most confusing: variant 5, "clarify".**

The headline is a verb addressed to *me*, but the sub-line ("Did you
mean v2 or v3 of the protocol") is a question being asked *of* me.
The grammar flips mid-screen. The gold bell suggests "alert", but the
content is a yes/no choice that should look more like variant 2
(`approve?`). I'd expect variant 5 to be re-titled "v2 or v3?" or
"answer please" — something where the headline matches the shape of
the action.

**Second most confusing: variant 2, "approve?".**

Headline is fine. But the body is literally `Bash:` then
`$ git push --force`. As a bystander I read that as "the screen wants
my colleague to push to a git thing forcefully". The word *force* in
red-adjacent gold next to a warning triangle reads as *danger*, and
yet the call to action is just "approve?". The tonal mismatch is
small but real — it asks casually about something visually marked as
scary.

**Cleanest of the five: variant 3, "pick one".**

The icon clearly says "list", the headline matches, and the body
(`inline defer abort`) is three short options laid out as a row. A
non-coder still understands "pick from a short menu". This is the
template the other four should imitate.

**Common problem across all five variants:** the `cc sx4` /
`cc ve_sx5` line is the second-most-prominent text on every screen
(big, teal, dead-centre) and it is **opaque jargon** to anyone who
isn't the developer who wrote it. It looks like a session ID. It
looks like a row of letters and digits that took up the slot a
human-readable subtitle should have occupied. Every one of these
five screens would improve by deleting that line outright.

---

## 3. Visual hierarchy — what *should* be biggest vs. what *is* biggest

On the ambient screen:

- **What is biggest:** `00:11` — the elapsed time / clock.
- **What should be biggest:** honestly, on a calm desk surface, nothing
  should be huge. But if forced to pick, I'd want the *agent state*
  (sleeping / working / waiting for human) to dominate, because that's
  the one fact a passing colleague cares about. The numeric counter is
  the third or fourth most-useful thing on the screen and it's getting
  60-pt treatment.

On the awaiting screen:

- **What is biggest:** `your turn` — correct, no notes.
- **Second biggest:** `cc 0_demo` (or `cc sx4`, etc.) — the session ID
  glyph. This is wrong. The second-largest thing on screen should be
  *the question being asked*, not an internal identifier. Right now the
  question lives in the body text and is smaller than the meta-data.

**The hierarchy inversion is the single biggest issue.** Across both
screens, identifiers and timers (machine-relevant) are huge, and
content (human-relevant) is small.

---

## 4. The noir theme on an always-on desk

I read `docs/brand/palette.md`. The intent is clear and the palette
itself is tasteful — teal as the cool dashboard accent, gold for warn,
moss for ok, near-black `#0b0a09` ground. As a *palette in a doc*, it's
nice.

**As an always-on desk surface, though, noir-as-default is a
double-edged choice:**

Good:
- A near-black panel sitting next to a bright monitor is genuinely
  unobtrusive — the dark rectangle reads as "off" most of the time,
  which is exactly what I want from a coworker's gadget.
- Teal-bright `#2BB3B1` on near-black has good contrast for the
  headline letters at glance distance, and doesn't scream.

Bad:
- The grey body text (`ink-mute` and `ink-fade`-equivalents) on
  near-black is genuinely hard to read at 50 cm. The activity rows on
  the ambient screen, and the `waiting 5s` / sub-headline on the
  awaiting screens, are all in low-contrast grey. From a bystander
  perspective that's fine — I'm not the reader. But for the *owner* of
  the device, if they're sitting at 50 cm, that's going to be straining.
- Gold (`#b89020`) and teal-bright (`#2BB3B1`) are the only two
  accent colours actually used on these screens. They have **almost
  the same luminance**. From 2 m, the warning bell on `clarify` and
  the pencil on `type a reply` look the same colour to me. The
  semantic distinction (gold = warn, teal = neutral) is doing zero
  work for a glancer because both read as "the bright glyph".
- The white `2.1k` token counter on the ambient screen breaks the
  monochromatic mood — it's the **brightest pixel on the screen** and
  it's used for a metric I don't care about as a bystander, and that
  the owner probably doesn't urgently care about either.

Overall: the *palette* is good. The *application* of it on these
screens uses brightness budget badly — the brightest things are the
least-important things.

---

## 5. Distraction profile

If this device sat 60 cm from my keyboard, would it annoy me?

**Well-behaved moments:**
- The ambient screen is *static* in the screenshot. If it stays static
  most of the time, it's a black rectangle with some grey rows. Fine.
- The awaiting variants are full-screen single events. They're loud,
  but they're loud *on purpose* and *briefly*. Acceptable.

**Distracting moments:**
- The **scrolling activity feed** is the obvious one. Six identical-
  looking rows of `Edit / Bash / Grep` with timestamps ticking is
  exactly the kind of low-amplitude motion that drags peripheral
  attention without ever rewarding it. Every time it scrolls I'll
  look at it, and every time I look I'll learn nothing. That's the
  worst possible distraction profile: high attentional cost, zero
  informational reward.
- **Clipped text on the awaiting-options screen** ("ind variants on
  real device   Shipped v"). Cut-off text is a magnet for the eye.
  Bystanders read it as broken.
- The transition from ambient → "your turn". I can't see this in a
  still, but going from a dim grey grid to a giant **your turn** with
  a teal pulse dot is going to be a hard cut. If it pulses or animates,
  it's effectively a flashing light on a coworker's desk. That's the
  kind of thing that gets a device unplugged.
- Two clocks/timers on screen at once (the `00:11` headline and the
  `00:11` repeated down the rows) create the illusion of constant
  ticking even when nothing is changing.

---

## 6. What I would remove

If I were allowed to take a black marker to these screens:

1. **Drop the session IDs entirely** (`cc sx4`, `cc ve_sx5`, `cx sx3`,
   `cc 0_demo`). They are the second-most-prominent element on every
   awaiting screen and they communicate nothing to anyone who isn't
   debugging the device. Move them to a long-press / debug view.
2. **Drop the big `00:11` headline on the ambient screen.** Or shrink
   it to footer size. A 60-pt elapsed timer reads as *urgency*, and the
   ambient screen is supposed to be the *non-urgent* state. The timer
   is fighting the mood.
3. **Drop one of the two metrics at the bottom.** "2 active" and "2.1k
   tokens today" are both there. Pick one. A bystander cannot use both,
   and the owner mostly only needs one at a time. My vote: keep
   "2 active", drop the token count — token usage is a billing
   concern, not a desk-glance concern.
4. **Drop the duplicate rows in the activity feed.** Three of the six
   rows are visibly identical pairs (`Edit … src/auth.py +8 -2` twice,
   `Bash … cx_a3` twice, `Grep … login 42 hits` twice). Either
   deduplicate or collapse with a "x2" marker.
5. **Drop `Clawd`** as a visible string on the ambient screen. The
   word is a developer in-joke for "Claude", and from 50 cm it just
   looks like a misspelling. Either show nothing, or show the actual
   agent name.

---

## 7. Top-3 P0s — fix before this device deserves desk space

### P0-1. Kill the activity feed scroll, or make it stop ticking
The six-row Edit/Bash/Grep list is the largest single element on the
ambient screen, it is unreadable from any normal viewing distance, and
it provides exactly the wrong distraction profile (constant motion,
zero glance value). Either collapse it to a single "last action: 11s
ago" line, or hide it behind a press. As shipped, this is the feature
that will get the device put in a drawer.

### P0-2. Fix the visual hierarchy inversion
On every screen I looked at, the second-largest text is a machine
identifier (`cc sx4`, elapsed timer, session ID) and the human-
relevant content (the actual question, the actual choice) is the
third- or fourth-largest. Swap them. The question being asked of the
human should be larger than the ID of the session asking it.

### P0-3. Fix the clipped text on `live-awaiting-options.png`
"ind variants on real device   Shipped v" being clipped on both
sides is a shipping-blocker for a polished desk device. Truncation
with an ellipsis would be tolerable; mid-word clipping is not.
Bystanders read clipped text as "this thing is broken", and once
they've decided that, no amount of feature work convinces them
otherwise.

---

## Closing note from across the desk

I want to like this. The awaiting variants — especially `pick one` —
show that the team knows how to make a calm, legible, one-glance
screen. The palette is grown-up. The dark surface is genuinely
considerate of the people sitting nearby.

The trouble is that the *resting* state — the state the device will
be in 95% of the day — is the busiest, noisiest, least-legible
screen of the bunch. That's backwards. The thing my colleague's
desk-mate sees should be the thing that asks the least of us.

Make the quiet screen quieter. Make the loud screen earn its loudness.
That's the whole brief.
