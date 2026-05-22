---
name: Question
about: How do I do X with the dashboard?
title: 'q: '
labels: question
assignees: ''
---

## What you're trying to do

Be specific. "How do I integrate codex" → "How do I make the bridge
pick up `codex exec --json` events without modifying my codex
invocation?"

## What you've already tried

Approaches that didn't work, with why. Helps us not repeat suggestions
you've already eliminated.

## Where you looked

Check these first if you haven't:

- [ ] [`README.md`](../../README.md)
- [ ] [`PROTOCOL.md`](../../PROTOCOL.md) — wire-format reference
- [ ] [`CONTRIBUTING.md`](../../CONTRIBUTING.md)
- [ ] [`docs/E2E_DEMO.md`](../../docs/E2E_DEMO.md) — the runbook
- [ ] [`docs/HOST_INTEGRATION.md`](../../docs/HOST_INTEGRATION.md) — bridge wiring
- [ ] [`HARNESS_GAPS.md`](../../HARNESS_GAPS.md) — known framework gotchas
- [ ] Existing closed issues searching the question

## Environment

```bash
esp-harness --version
esp-harness doctor --json
python --version
```
