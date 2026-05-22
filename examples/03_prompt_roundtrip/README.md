# 03_prompt_roundtrip — the permission-button round-trip

The most interesting protocol round-trip:

1. host sends `dash prompt {id, tool, hint, agent_kind, session_id}`
2. device flips to the **prompt** scene
3. human presses BOOT (approve once) or USER (deny) — or the device's
   60 s timeout fires
4. device emits an unsolicited `EVT: permission id=<id> decision=<…>`
5. host correlates by `id`, unblocks the matching `PreToolUse` hook

Against the **mock**, step 3 is auto-fired after `--decision-delay-ms`
ms (default 500). Against a **real board**, you actually press the
button.

## Run it

```bash
# Terminal A — bias the mock to "approve once" after 800 ms
python tools/mock_device_v1.py --port 9876 -v --decision-delay-ms 800

# Terminal B
python examples/03_prompt_roundtrip/run.py
```

Expected output ends with:

```
OK — prompt round-trip complete (decision=once).
```

To see a denial, pass `--auto-deny` to the mock:

```bash
python tools/mock_device_v1.py --port 9876 -v --auto-deny
python examples/03_prompt_roundtrip/run.py
# → decision = 'deny'
```

## Notes

- `id` is opaque to the device — it just echoes it back in the EVT.
  Always pass a *unique* id per request; the bridge uses it to wake the
  correct hook.
- `agent_kind` + `session_id` are also opaque but the device renders
  them on the prompt scene so the human knows *which* agent is asking.
  Always set them when you have more than one agent.
- The 60 s timeout is firmware-side, not protocol-side. If you want a
  different timeout, don't change the protocol — keep your own timer
  and drop the prompt by sending the next snapshot without the
  `prompt` field set.
