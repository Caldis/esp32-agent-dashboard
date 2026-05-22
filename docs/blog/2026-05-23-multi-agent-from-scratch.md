# Building a physical AI-agent dashboard in 24 hours

**Posted:** 2026-05-23

The lessons that mattered, in order:

1. **A tokeniser bug in a framework can hide for months until a
   consumer pushes nested JSON through it. Make framework-vs-consumer
   parity a first-class test corpus, not a hope.**
2. **When the consumer mock and the firmware C share a tokeniser,
   they share a tokeniser, not a copy of one. Otherwise you're
   debugging two parsers and only know it after the divergence ships.**
3. **Tokens-as-infinite-fuel: spawning four agents in parallel and
   committing the convergence is dramatically cheaper than one agent
   doing the same work serially, even counting the failed branches.**

This is the story of how `esp32-agent-dashboard` went from "I saw a
nice picture of `anthropics/claude-desktop-buddy` and wanted my own"
to a tagged `v0.1.1` running multi-agent on real hardware, with a
public README, brand pack, CI, stress suite, and a 20-version
roadmap, in the wall-clock span of a long evening.

## The reference

The seed was [`anthropics/claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy) — a Claude-Desktop-themed
status orb on an ESP32. Nice idea, single agent, single transport.
The brief I wrote myself was different on four counts: **Claude Code
+ Codex CLI** instead of Claude Desktop, **both visible at once**
with stable left/right placement, **physical button approval** for
`PreToolUse` (the whole point of a dashboard is to not have to
alt-tab to deny `rm -rf`), and built on `esp-harness` — my own
framework — so the consumer would surface every gap the framework
still had.

That last point turned out to be the actual point of the exercise.
The framework benefits from the consumer benefits from the framework.

## The scaffold (00:18 — `init`)

`81f431c` — `init: esp32-agent-dashboard scaffold from esp-harness
v1.7.5`. The first commit kicks off three subagents in parallel:

- **F** owns [`main/`](../../main/) (firmware).
- **H** owns [`tools/`](../../tools/) (host bridge).
- **G** owns `D:\Code\esp-harness\` (framework, in a different repo).

The boundary contract is in [`docs/AGENT_ROLES.md`](../AGENT_ROLES.md). No agent
crosses repos. The orchestrator — me — is the only one that integrates
commits across ownership lines. This is the agent-role registry
concept, and it's the single most important load-bearing decision in
the whole project. Without it, four agents fighting over the same
`main/agent_state.c` looks exactly like a bad merge.

## Spec first (00:20 → 00:30)

Before any code, six commits of nothing but documents:

```text
6d28ce9  docs: PROTOCOL.md — wire schema between host bridge and device
d2b5a2a  docs: HOST_INTEGRATION.md — CC + Codex integration map
102d29a  test: demo_inputs.jsonl — 13 hook events simulating a 2-session CC loop
2600eba  docs: E2E_DEMO.md — runbook for the proof-of-life demo
f962f1d  test+gaps: mock_device.py + G-6 (smoke gate Aurora-coupled)
376262f  design: ADVERSARIAL_AS_PRIMITIVE.md — esp-harness v1.8 north star
```

The wire schema (`PROTOCOL.md`) was written **before** the firmware
side of the wire existed. The mock device (`docs/mock_device.py`)
existed **before** the firmware did. The bridge could run end-to-end
against the mock before COM9 had ever been opened.

This is the **fail-fast then converge** half of the mantra. Get the
shape on paper and a mock under test, *then* spend the expensive
device-flash cycles on actually flashing.

## v0 — five scenes alive (00:40)

`15638ca` — `v0: dashboard live on device — 5 scenes rendering,
bridge dry-run OK, G-7 closed`.

This is the moment the device started actually showing things. The
five LVGL scenes (`idle` / `sessions` / `prompt` / `tokens` /
`status`) registered through esp-harness's scene framework. The
bridge's `--dry-run` mode replayed `sample_session.jsonl` through the
expected `dash` verbs without needing a port open.

The interesting part of this commit is the suffix: `G-7 closed`.

### G-7: the tokeniser bug

The device's console parser splits a line like
`dash snapshot {"agents":[...]}` into tokens. The original tokeniser
broke on whitespace, and on quoted tokens took everything from `"`
to the next `"`. Sounds fine — until you push **nested** JSON, where
every key and value is itself quoted, and the tokeniser closes the
outer token on the first inner `"`. The device parses
`dash snapshot {`, fails to read the rest as an argument, emits
`ERR: bad json`. Success rate: zero. Repro: trivial.

The fix landed upstream at `esp-harness@664b14e`: tokens that lead
with a `"` only close on `"` **followed by whitespace or EOL**. The
parity corpus that backs this fix got 25 cases. The cost to find the
bug was twenty minutes of `mock_device.py` printing `ERR: bad json`
into a log; the cost to fix it was three lines; the cost to *prevent
the next one* was the corpus.

Lesson: a tokeniser bug in a framework hides forever until a
consumer pushes payloads the framework's author never imagined. The
firmware-side mock and the C tokeniser had agreed for the entire
lifetime of `esp-harness` because nobody was sending nested JSON
through them. The consumer was the falsifier.

## v0.1.0 — public release (00:56)

`1d1c3d5` — the public-release commit. LICENSE, README, CONTRIBUTING,
CHANGELOG, issue templates, PR template, CI workflow, GitHub Pages
homepage. 12 files, 1706 inserts.

Nothing here is novel; it's the open-source scaffolding tax. Worth
calling out:

- The **CI** replays `tools/sample_session.jsonl` through the bridge
  every push. Firmware build is **not** in CI; cross-compiling
  ESP-IDF on a Linux runner is slow and image-fragile. Real-hardware
  verification happens at release time.
- The **homepage** ([`docs/index.html`](../index.html)) is a single 836-line file
  styled to match `esp-harness`'s editorial type system — Fraunces /
  Geist / IBM Plex Mono on a paper palette. No framework, no build
  step, no JS beyond a smooth-scroll handler.

See the [`v0.1.0` release notes](../releases/v0.1.0.md) for the
detailed inventory.

## Post-v0.1.0 polish (01:06)

`1a249f8` — `post-v0.1.0: brand assets, V1-E stress suite, G-8
framework fix`. Three independent agents (V1-C brand, V1-E stress,
G framework) all landed working-tree state that the orchestrator
picked up here.

This is where **G-8** got found.

### G-8: the consumer mock drift

`docs/mock_device.py` carried its own copy of the firmware tokeniser
in Python. When G-7 landed upstream and the C version moved, the
Python mock didn't move with it. The stress suite (`tools/stress.py`)
was the falsifier: it pushed payloads the **real** firmware handled
fine and the **mock** rejected. Two parsers, one assumption they
agreed, zero alarm bells until someone ran the workload through both.

The upstream fix at `esp-harness@fb5a549` introduces
`esp_harness.core.parser.tokenise_console_line` — the canonical
Python port of the C tokeniser, with a 25-case parity test suite
pinning both implementations together. The mock now imports it.
There is no longer "the firmware tokeniser" and "the mock
tokeniser"; there is one tokeniser with two bindings.

Lesson: if a mock and the production code *can* disagree, they
**will**. Either share the implementation by construction, or
generate the mock from the spec. Two hand-written copies will
diverge under load, and the divergence will surface as a flaky test
you'll blame on the network.

## v0.1.1 — v1 firmware + v1 bridge (01:11)

Three commits land back-to-back:

```text
d0b128f  v1 firmware: multi-agent slots, theme palette, dashboard scene, anims
53559ca  v1 bridge: TCP transport, config file, status/bench subcommands, multi-agent
f423ac1  v1 polish: H1 reflection — new gaps + ERR logging + mock tiny-json fixes
```

`d0b128f` and `53559ca` are two parallel subagents (V1-A and V1-B)
that worked on isolated trees and converged. V1-A inserted +1868
lines into [`main/`](../../main/); V1-B inserted +1493 into
[`tools/claude_buddy_bridge.py`](../../tools/claude_buddy_bridge.py). They didn't touch each other's
files. The orchestrator integrated. Build was clean (0 warnings); the
bridge's `replay sample_session.jsonl --dry-run` against the TCP
mock measured 0.08 ms median push latency.

This is the **tokens-as-infinite-fuel** pattern in action. The
fan-out:

- V1-A — firmware multi-agent
- V1-B — bridge multi-agent + TCP + config
- V1-C — brand assets
- V1-D — public release docs
- V1-E — stress suite

Five subagents, one orchestrator, one repo. The cost per agent in
LLM tokens is something close to the cost of one agent making a
serial pass. The wall-clock saving is the multiplier. The failed
branch (when one agent gets the design wrong) is sunk cost, but it's
sunk against the *one agent's* output, not the others — the
orchestrator throws it away and the rest keep going.

The polish commit `f423ac1` is the v1.0.1-style follow-on:
[`main/tiny_json.c`](../../main/tiny_json.c) gets a five-line tightening,
[`tools/hook_dispatch.py`](../../tools/hook_dispatch.py) gets an eight-line fix to forward the
bridge's permission reply verbatim, and three new gap entries land in
[`HARNESS_GAPS.md`](../../HARNESS_GAPS.md) (G-H1 / G-H2 / G-H3). All real bugs, all
surfaced by the bridge agent's own debugging session, all logged for
the next `esp-harness` cycle to address.

See [`v0.1.1` release notes](../releases/v0.1.1.md) for the full
inventory.

## Post-v0.1.1 — the roadmap (01:21)

`9a1a13d` — `post-v0.1.1: roadmap + agent-roles registry + v1
acceptance shots`.

Two things land that point at the next 19 versions:

- [`ROADMAP.md`](../../ROADMAP.md) — a 20-version plan from `v0.2.0` (framework
  hardening: close G-1, G-3, G-4, G-H1, G-H3) through `v2.2.0`
  (native desktop client). Each entry has concrete deliverables and
  named driver agents.
- [`docs/AGENT_ROLES.md`](../AGENT_ROLES.md) — the contract registry. Every agent
  type the orchestrator can dispatch is defined here: its file
  ownership, its read scope, its return shape, the version it first
  appears in.

The agent-role registry is the load-bearing piece. A roadmap is just
a wishlist without a way to staff it. With the registry, every
roadmap row has a named role and the role has a contract: what it
owns, what it reads, what it returns. The orchestrator dispatches by
role; the role's prompt is the contract instantiated. No agent can
accidentally edit a sibling's files, because the prompt doesn't tell
it to and the policy doesn't permit it.

This is the part that scales. One agent doing 20 versions
sequentially is a very long-running agent that drifts as its context
grows. Twenty agent-runs against a registry, each isolated, each
small, each handed off to the orchestrator — that's a system.

## What I'd do differently

- **G-7 should have been caught in framework CI**, not in consumer
  use. The fix is exactly the test corpus that now exists. Apply
  retroactively: every framework primitive ships with a parity
  corpus, and every consumer pushes its actual payloads through that
  corpus.
- **G-8 — the consumer mock drift — points at a deeper rule.** If
  the mock and the production code can disagree, they will. Share
  the implementation by construction (one tokeniser, two bindings)
  not by discipline (two hand-written tokenisers, "we'll keep them in
  sync").
- **The agent-role registry should have existed at commit zero**,
  not at commit fifteen. I got lucky that the five subagents I ran
  against `main/` didn't actually overlap. Codifying the boundary up
  front turns luck into policy.
- **The "24 hours" framing is real but it's not the whole story.**
  The wall-clock from `init` (`00:18:59`) to `post-v0.1.1`
  (`01:21:43`) is **63 minutes** in a single evening, only possible
  because `esp-harness` was already at v1.7.5 (scene framework,
  console protocol, mock infra) and the orchestration patterns
  (subagent dispatch, role registry, tokens-as-fuel) were already
  practised. What the 24-hour window proves is that *once the
  framework and the patterns exist*, a polished real-project consumer
  drops out in an evening. That's the value of a framework worth
  building.

## What this unlocks

The framework now has a roadmap with 20 named versions and a
registry of 22 named agent roles. The named agents (`G2`, `F2`,
`H2`, `H3`, `TRANS1`, `SEC1`, …) take over from the ad-hoc
V1-A/B/C/D/E that built `v0.1.1`, each with a written contract:
files they own, files they read, return shape. The orchestrator
writes the dispatch prompt; the role definition writes the boundaries.

If you're reading this from a similar starting position — framework
plus consumer co-evolving in lockstep — the meta-pattern is:

1. **Spec before code.** Protocol, mock, demo runbook exist as
   documents before any firmware compiles.
2. **Mock everything across a boundary, and share the tokeniser** —
   anything both mock and production parse — by construction, not
   by discipline.
3. **Parallel agents for independent territory.** Make the territory
   explicit in a role registry; let the orchestrator integrate.
4. **Adversarial > validation.** Every parser, every protocol wants
   a falsifier. The cost of the falsifier is the bug count it finds;
   the savings are the bugs that don't ship.

Next stop: `v0.2.0` — framework hardening, where the `G2` agent
closes G-1 (persistent console session API), G-3 (shared port API),
G-4 (self-describing manifest), G-H1 (`PayloadFollowsReader`), and
G-H3 (ERR-callback). Then the bridge gets its multi-agent rewrite
on top of the new framework surface.

— Caldis
