## Summary

<!-- One paragraph: what does this PR do, and *why*? -->

## Type of change

<!-- Tick one (or more, if genuinely multi-faceted): -->

- [ ] Bug fix (firmware, bridge, or protocol)
- [ ] New feature (new scene / new hook handler / new verb)
- [ ] Wire-protocol change (breaking — bumps the version handshake)
- [ ] Documentation only
- [ ] CI / build / chore

## Affected layers

- [ ] Firmware (`main/`) — device-side C code
- [ ] Bridge (`tools/`) — host-side Python
- [ ] Wire protocol (`PROTOCOL.md`) — the contract between the two
- [ ] Docs (`README.md`, `docs/`)
- [ ] `HARNESS_GAPS.md` — discovered an esp-harness limitation

## Checklist

- [ ] I have read [`CONTRIBUTING.md`](../CONTRIBUTING.md)
- [ ] My PR has a single conceptual change
- [ ] I ran the smoke gate locally — `tools/smoke.ps1` (Windows) or the bridge mock roundtrip (cross-platform) is green
- [ ] If I changed the wire protocol, [`PROTOCOL.md`](../PROTOCOL.md) is updated in this PR, AND both firmware and bridge sides match the new shape
- [ ] If I added a new event type to the bridge, I added a representative line to `docs/demo_inputs.jsonl` so CI replays it
- [ ] If I added a regression case, it's wired into `.github/workflows/ci.yml` (or `tools/smoke.ps1` for device-only cases)
- [ ] If I hit an esp-harness gap, I logged it in [`HARNESS_GAPS.md`](../HARNESS_GAPS.md) with a reproducer

## How tested

<!-- What did you actually run? -->

```
e.g.
python docs/mock_device.py --port 9876 &
python tools/claude_buddy_bridge.py replay docs/demo_inputs.jsonl \
    --port-kind tcp --port 127.0.0.1:9876 --assert-dash-commands
# all 13 expected verbs emitted, mock returned 0 ERRs
```

## Related issues / discussions

<!-- Closes #123, addresses #456, etc. -->

## Screenshots / device captures

<!-- For scene changes. Drop a PNG from docs/img/ in here. -->
