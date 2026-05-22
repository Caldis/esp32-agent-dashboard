# Adversarial agent-teams as a first-class harness primitive

> **Premise**: tokens are electricity. Compute is the new fuel — and
> the fuel is effectively infinite. Don't optimise for fewer agents
> or shorter prompts. Optimise for **more personas**, **more
> perspectives**, **more findings**.

This document proposes lifting the manual 6-round adversarial loop
that took esp-harness from v1.7.0 → v1.7.5 into a permanent
first-class capability: `esp-harness adversarial`.

## The pattern we manually executed (and want to bake in)

```
                    ┌──────────────────────────────────┐
                    │  HUMAN  / orchestrator agent     │
                    │  sets a goal:                    │
                    │    'converge on X'               │
                    └────────────────┬─────────────────┘
                                     │
                  ┌──────────────────┼──────────────────┐
                  ▼                  ▼                  ▼
          ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
          │  Persona 1   │   │  Persona 2   │   │  Persona N   │
          │  verify      │   │  falsify     │   │  port-pretend│
          │  first-user  │   │  process-    │   │  cross-shell │
          │              │   │  audit       │   │  ...         │
          └──────┬───────┘   └──────┬───────┘   └──────┬───────┘
                 │                  │                  │
                 └──────────────────┼──────────────────┘
                                    ▼
                       ┌──────────────────────────┐
                       │  Aggregator              │
                       │  - dedupes findings      │
                       │  - severity-classifies   │
                       │  - cross-checks each     │
                       │    one survives a 2nd    │
                       │    persona's probe       │
                       └────────────┬─────────────┘
                                    ▼
                       ┌──────────────────────────┐
                       │  Fixer (optional)        │
                       │  - per critical finding, │
                       │    spawn an agent to     │
                       │    propose the patch     │
                       │  - patch gated by smoke  │
                       └────────────┬─────────────┘
                                    ▼
                       ┌──────────────────────────┐
                       │  Convergence checker     │
                       │  - any criticals left?   │
                       │    yes → loop            │
                       │    no  → tag + report    │
                       └──────────────────────────┘
```

We did this by hand. Six rounds. Each round caught a sibling
regression the previous defence missed. The framework should do this
automatically the moment someone runs `esp-harness adversarial`.

## Proposed `esp-harness adversarial` shape

```bash
esp-harness adversarial \
    [--rounds N | --until-converged] \
    [--budget tokens=10M | budget hours=4 | budget loops=10] \
    [--personas verify,falsify,porter,cross-shell,first-user,fuzz] \
    [--mode parallel|sequential|cascade] \
    [--auto-fix] \
    [--gate-on-converged] \
    [--findings-out ./findings/] \
    [--project .]
```

### Persona library (initial set)

Each persona is a prompt template + a result-shape contract. The
adversarial command spawns one subagent per persona, gives it the
project + a budget, and collects findings in a uniform schema.

| Persona | What it does | Source-of-bug it catches |
|---|---|---|
| `verify` | reads claims, runs each, reports VERIFIED/PARTIAL/BROKEN | trivial regressions, surface bugs |
| `falsify` | tries to break each claim adversarially | sibling regressions (Lesson 15) |
| `first-user` | clones fresh, follows README verbatim | onboarding / docs drift |
| `porter` | tries to bring up on a different board/shell | platform coupling |
| `cross-shell` | runs from Git Bash / WSL / PowerShell / CMD | env-detection short-circuits |
| `process-audit` | grep for "same defence not applied to sibling X" | exactly what round-4/5/6 found |
| `boundary` | edge cases: empty inputs, huge inputs, special chars | parser / overflow bugs |
| `concurrency` | parallel invocations, port contention | race conditions |
| `time-skew` | clock drift, midnight rollovers, NTP jitter | time-dependent bugs |
| `fuzz` | random JSON / argv / serial byte streams | unhandled inputs |
| `regression-recall` | re-runs every closed lesson's smoke case | rotted gates |
| `documentation` | reads docs, asserts each example works | stale examples (this round's bench-compare findings) |

### Findings schema

Each persona returns:

```json
{
  "persona": "falsify",
  "started_at": "2026-05-23T01:23:45Z",
  "elapsed_s": 763,
  "findings": [
    {
      "id": "F-falsify-001",
      "severity": "critical",   // critical | blocking | minor | informational
      "what_broke": "run --no-build silently flashes stale binaries",
      "evidence": {
        "command": "esp-harness run --no-build --port COM9",
        "output": "...",
        "expected": "exit_code=100 with MSys trigger",
        "got": "ok=true, wrote_bytes=0"
      },
      "code_location": "tools/esp-harness/src/esp_harness/commands/run.py:96-122",
      "suggested_fix": "mirror flash.py's MSys check + wrote_bytes==0 gate",
      "cross_check_persona": "verify",      // who else can confirm
      "smoke_case_proposal": "run --no-build from Git Bash returns ok=false + MSys trigger"
    }
  ],
  "confidence": 0.9,
  "tokens_used": 102758
}
```

### Aggregator

Dedupe by `code_location + severity`. Cross-check each finding by
firing a `verify` persona at the claim ("does this break the way
falsify claims?"). Only persist findings that survive cross-check.

### Fixer (optional, `--auto-fix`)

For each critical finding the aggregator promotes:

1. Spawn a `fixer` persona with the finding as prompt + the
   `suggested_fix` as hint.
2. Fixer commits a patch on a branch + adds a smoke case.
3. Run the full smoke gate; if green, merge. If red, retry with
   diagnosis fed back into the fixer.

If `--gate-on-converged`, the command exits 0 only when a follow-up
adversarial round finds zero critical. Otherwise loops up to the
budget.

### Personas as a registry

Like firmware console commands, personas live in a directory:

```
tools/esp-harness/src/esp_harness/adversarial/personas/
├── verify.py          # PROMPT = "..." + result schema
├── falsify.py
├── first_user.py
├── porter.py
├── cross_shell.py
├── process_audit.py
├── boundary.py
├── concurrency.py
├── time_skew.py
├── fuzz.py
├── regression_recall.py
└── documentation.py
```

`esp-harness adversarial --personas all` enumerates the registry.
Adding a new persona is just dropping a file in this directory —
same discovery surface convention as `console_protocol_register` /
`scene_fw_register` / `TOOLKIT_COMMANDS`.

## Why this is the right shape for esp-harness

The framework's identity is **"AI-driven dev loop"**. The discovery
surface (manifest), the structured JSON I/O, the semantic exit codes
— all exist to let an AI drive without human translation. The
manual adversarial loop we executed tonight is *that exact same
pattern* applied recursively to the framework itself:

- **Build / flash / monitor** lets an AI iterate firmware → so the AI
  spends its tokens on firmware questions, not on parsing logs.
- **Adversarial** lets an AI iterate **the framework** → so the AI
  spends its tokens on finding bugs, not on remembering to test
  sibling code paths.

Lesson 17 ("verify mode misses regressions; falsify mode catches
them") demanded we run a falsification round per release. That's
now automatable. Lesson 15 ("defence covers all entry points")
demanded we run the process-audit persona to grep for sibling
patterns. Also automatable.

Every lesson in `docs/lessons-v1.7.md` either becomes a smoke case
(already does — Lesson 7's commitment) **or a persona**. The
adversarial command runs the personas; the personas catch the bugs;
the bugs become smoke cases; the smoke cases run every release.
**The loop closes on itself.**

## Token-as-electricity implications

If we treat tokens as infinite:

- **Run all 12 personas in parallel** for every release, not "pick
  the 3 most likely to find something".
- **Run adversarial after every commit**, not just at release time.
  Pre-merge gate.
- **Spawn 5 redundant falsify agents** with slightly different
  prompts and keep findings that appear in ≥2. Higher confidence,
  no incremental human cost.
- **Cross-personas adversarially**: persona-1 proposes a fix,
  persona-2 (with no knowledge of persona-1's reasoning) tries to
  break it. Findings that survive both are real.
- **Long horizons**: budget hours=24, --until-converged, leave it
  running overnight. Wake up to a tagged release with N regressions
  fixed.

This is the v1.8 north star.

## Concrete v0 implementation plan

1. **Persona registry skeleton**:
   `tools/esp-harness/src/esp_harness/adversarial/__init__.py`
   exporting `register_persona(name, prompt_fn, result_schema)`.
2. **Two personas to start**: `verify.py` and `falsify.py` ported
   from the prompts I sent the round-4/5/6 subagents. Both already
   work (proven this session) — codify the prompts.
3. **Aggregator**:
   `adversarial/aggregator.py` — dedupes by `code_location`,
   severity-classifies, cross-checks via a second persona.
4. **CLI entry**:
   `tools/esp-harness/src/esp_harness/commands/adversarial.py`
   plumbing `--personas`, `--mode`, `--findings-out`. Uses the
   `Agent` tool spawning pattern (same as Claude Code subagents)
   if running inside an AI session; falls back to subprocess
   pytest-style if standalone.
5. **Smoke gate update**: `tools/smoke.ps1` gains a case
   `adversarial dry-run finds known-good findings (smoke gate
   regression)` — feeds in a synthetic project with one known
   critical, asserts the persona finds it.
6. **Documentation**: an `esp-harness adversarial --help` that
   explains the persona library and a `docs/adversarial.md`
   describing the schema + lifecycle.

Each persona is ~50 lines of prompt + ~20 lines of result-parsing.
Total v0 ≈ 400 LOC. v1 with fixer-agent loop ≈ 800 LOC. Both fit
inside a single esp-harness release cycle.

## Relationship to today's project (agent-dashboard)

This document was triggered by the agent-dashboard project surfacing
G-6 ("smoke gate Aurora-coupled"). The user immediately reframed:
**don't fix G-6 in isolation; make adversarial review a native
capability so G-6-class problems get caught automatically the
moment they appear**.

That's exactly the framework north star. Logging here so it lands
on the v1.8 backlog with full context.
