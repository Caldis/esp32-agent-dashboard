# Release process

The end-to-end recipe for shipping one version. Built for orchestrator
agents driving the 20-version roadmap; humans can also run it manually.

## Quick path (one command)

```powershell
pwsh tools/release.ps1 `
    -Version 0.2.0 `
    -Title "Framework hardening" `
    -Notes "Closes G-1, G-3, G-4, G-H1, G-H3 in esp-harness; bridge swaps to persistent ConsoleSession. See docs/releases/v0.2.0.md."
```

The script enforces all 10 quality gates from `ROADMAP.md`. On any
failure it exits non-zero with a one-line reason and the build is NOT
tagged. Re-run after fixing.

## What the script does

1. Pre-flight: working tree clean, tag v$Version doesn't exist.
2. Firmware build clean (0 warnings).
3. esp-harness `pytest -q` passes.
4. `tools/stress.py --all` passes (5/5) against fresh `docs/mock_device.py`.
5. `claude_buddy_bridge.py bench --events 1000 --dry-run` completes.
6. CHANGELOG.md: lift `[Unreleased]` content into a new `[$Version]` stanza.
7. `git commit -m "release(v$Version): $Title"`.
8. `git tag -a v$Version -m "..."`.
9. `git push origin master + v$Version`.
10. Optional: `gh release create` (pass `-CreateGhRelease`).

## When NOT to use the script

- Real-hardware verification step is OPTIONAL but recommended (it's
  not in the script because COM9 is a finite shared resource). Flash
  the new firmware to the device yourself between gate-pass and tag
  if you want the screenshots in the release notes.
- If you're shipping a version that has no firmware change (e.g.
  pure docs or pure bridge), pass `-SkipBuild`.
- If you want to dry-run the gates without tagging, pass `-DryRun`.

## Multi-agent waves

The orchestrator runs versions in concurrent waves where the agents'
file ownership doesn't overlap (see `docs/AGENT_ROLES.md`). A typical
wave:

```
wave-N start ──┬─ Agent A (owns X)
               ├─ Agent B (owns Y, parallel)
               └─ Agent C (owns Z, parallel)
                                         ┌───────────────────────┐
all return ──→ orchestrator integrates ─→│  pwsh release.ps1     │
                                         └───────────────────────┘
```

Conflict policy: if two agents end up touching the same file, the
orchestrator picks the one whose role declares ownership in
`AGENT_ROLES.md` and rejects the other's edits. Re-dispatch the
losing agent with a tighter scope.

## Per-version release-notes doc

Each version writes `docs/releases/vX.Y.Z.md` with the same content
the tag message has, expanded into prose. The orchestrator's standard
template:

```markdown
# vX.Y.Z — <Title>

Released YYYY-MM-DD.

## Summary
<one paragraph, what changed and why>

## What's new
- ...

## Breaking changes
<none | list>

## Framework upstreams
<which esp-harness commits this version pulled in>

## Gaps surfaced this cycle
<links to HARNESS_GAPS entries>

## Verification
- Build: ok=true, 0 warnings
- stress: 5/5 PASS
- bench: events_per_s=N
- Real-device flash: <commit hash if performed>
```

## Versioning rules

- semver. Minor for new features, patch for fixes only.
- Pre-1.0 is allowed to break interfaces with a minor bump if the
  CHANGELOG calls it out and the affected APIs are documented as
  experimental. The roadmap version targets are guidance, not
  contracts.
- v1.0.0 is the first stable-API milestone. After that, breaking
  changes need a major bump.

## Failure recovery

If `release.ps1` fails partway through (e.g. CHANGELOG bumped but
push failed because of network), the working tree state shows the
abandoned edits. Either:
- `git reset --hard HEAD~1` to roll back the CHANGELOG commit, fix
  the network issue, re-run; OR
- Push manually: `git push origin master && git push origin vX.Y.Z`.

Don't re-run the script while the abandoned commit is still on
HEAD — the pre-flight clean-tree check will pass (the abandoned
commit is committed) and the tag-exists check will trip on the
already-created tag, leaving you mid-release.
