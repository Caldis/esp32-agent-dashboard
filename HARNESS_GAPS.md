# Harness gaps surfaced by esp32-agent-dashboard

A running list of places where the esp-harness framework fell short
during this project. **Agent G** (in `D:\Code\esp-harness\`) watches
this file and lifts each gap upstream — either as a code fix in
esp-harness master, or a documented-recipe so the next consumer
doesn't reinvent it.

The point isn't to complain — it's to make every real project that
consumes esp-harness improve the framework itself. By the end of
this project, this file should be empty OR every entry should have
an upstream commit / docs entry it points at.

## Format

```
### G-N — <one-line title>

**Context**: Which sub-agent hit it (F = firmware, H = host bridge).
**What I needed**: …
**What I got**: …
**Workaround used**: …
**Suggested upstream fix**: …
**Resolution**: link to commit hash or doc PR in esp-harness/.
```

## Open

(none yet — agents are working)

## Resolved

(none yet)
