---
name: Bug report
about: Something broken in the firmware, bridge, or wire protocol
title: 'bug: '
labels: bug
assignees: ''
---

## What happened

A clear description of the bug. Which layer was affected — firmware
(device-side), bridge (host-side), or the wire between them?

## What you expected

A clear description of what should have happened instead.

## How to reproduce

Minimal steps. Ideally something runnable against `docs/mock_device.py`
so the reproducer doesn't require a physical board:

```bash
# e.g.
python docs/mock_device.py --port 9876 &
python tools/claude_buddy_bridge.py replay <(echo '<offending event JSON>') \
    --port-kind tcp --port 127.0.0.1:9876
# → wrong dash command emitted / mock returned ERR / etc.
```

If the bug requires real hardware, say so and include the exact
`esp-harness console --cmd '...'` line.

## Output

```
<paste the full error + last 20-30 lines of context>
```

If it's a wire-protocol bug, paste the verbatim `dash <verb> "<json>"`
line and the device's `OK:` / `ERR:` reply.

## Environment

```bash
esp-harness --version
esp-harness doctor --json
python --version
```

Plus:

- **OS** (and version): e.g. Windows 11 / Ubuntu 22.04 / macOS 14.2
- **ESP-IDF version**: `idf.py --version`
- **Board**: e.g. Waveshare ESP32-S3-Touch-AMOLED-2.16
- **Firmware commit**: `git -C esp32-agent-dashboard rev-parse HEAD`

## Additional context

Anything else that might help — a stack trace, a partial workaround,
a related esp-harness gap (link to [`HARNESS_GAPS.md`](../../HARNESS_GAPS.md)).
