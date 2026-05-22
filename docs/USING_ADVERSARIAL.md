# Using `esp-harness adversarial`

The v1.8 north-star command from the framework, landed at
`esp-harness@0fea03b`. This page covers what it does, how to invoke
it from this project, and how to interpret its output.

## What it does

Runs N adversarial personas against a consumer project (this one,
typically). Each persona is a prompt template + a result-shape
contract. The command spawns one subagent per persona per round,
collects findings in a uniform schema, dedupes by code location, and
reports convergence (critical-count == 0).

The pattern is the same six-round manual loop that took the framework
from v1.7.0 → v1.7.5 (critical count 5→3→3→1→2→1→0). Now it's one
command.

## Built-in personas (v0.3.0 cycle)

- **`verify`** — reproduces every documented claim; finds drift
  between docs and code. Conservative reader.
- **`falsify`** — finds sibling code paths where a fix was forgotten;
  attacks input boundaries. Suspicious auditor. Catches the
  Lesson-15-class "defence not applied to sibling X" bugs.

More personas (porter, cross-shell, process-audit, boundary,
concurrency, time-skew, fuzz, regression-recall, documentation) ship
in v0.3.1+ as the registry expands.

## Quick invocation

From this repo's root, with the esp-harness Python in `$PATH`:

```bash
# 1. list registered personas
esp-harness adversarial --list-personas

# 2. dry-run — print the prompts that would be sent (no AI calls)
esp-harness adversarial \
    --personas verify,falsify \
    --rounds 1 \
    --project . \
    --smoke-command "pwsh tools/release.ps1 -Version 99.99.99 -DryRun" \
    --findings-out ./.adversarial-out/ \
    --dry-run

# 3. when the real-AI dispatcher lands (v0.3.1), drop --dry-run and
#    pass --rounds 3 --until-converged. The command exits 0 only when
#    critical-count reaches 0.
```

## Wiring it into release.ps1

A future `tools/release.ps1` gate could chain into `adversarial` as
the very last pre-tag check:

```powershell
# Step 5.5 — adversarial gate (v0.3.1+ requires real-AI dispatcher)
$adv = & $HarnessPy -m esp_harness adversarial --personas verify,falsify `
    --rounds 1 --project $ProjectRoot --json
$advJson = $adv | ConvertFrom-Json
if ($advJson.by_severity.critical -gt 0) {
    Die "adversarial gate found $($advJson.by_severity.critical) critical findings"
}
```

We don't ship this yet because the dispatcher is dry-run only. Once
the dispatcher can call real AI subagents, the gate flips on
automatically and every release passes through it.

## Interpreting the output

```json
{
  "total": 7,
  "by_severity": {
    "critical": 0,
    "blocking": 2,
    "minor": 5,
    "informational": 0
  },
  "critical_first": [],
  "converged": true,
  "rounds_run": 3,
  "elapsed_s": 142.6,
  "persona_names": ["verify", "falsify"]
}
```

- `converged: true` ↔ `by_severity.critical == 0`. This is the only
  invariant releases gate on. Blocking findings are noted but don't
  block tagging; they go on the next-version backlog.
- `rounds_run` shows whether the loop hit `--rounds` or
  `--until-converged` exit early.

## Why this matters

Lesson 17 said "verify mode misses regressions; falsify mode catches
them" and demanded every release run a falsification round. Lesson 15
said "defence covers all entry points" and demanded a process-audit
persona. Both are now automated. Every lesson in
`esp-harness/docs/lessons-v1.7.md` either becomes a smoke case (already)
**or a persona**. The adversarial command runs the personas; the
personas catch the bugs; the bugs become smoke cases; the smoke cases
run every release. **The loop closes on itself.**

This is what closes G-6 (smoke gate Aurora-coupled): once consumer
projects use `esp-harness adversarial` instead of inheriting Aurora's
smoke gates, the framework-vs-consumer test-set drift problem becomes
structurally impossible. Each consumer's adversarial run is project-
scoped by `--project`.
