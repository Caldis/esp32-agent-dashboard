# 01_minimal — the smallest possible v1 consumer

A single Python file (`run.py`, ~120 lines, stdlib only) that opens a TCP
connection to `mock_device_v1.py` and walks the device through:

```
idle  →  sessions (one agent)  →  idle
```

Useful as a "hello world" for the wire protocol. Read the code top to
bottom — every line is commented with what it teaches.

## What you'll learn

| Line in `run.py` | Teaches |
|---|---|
| `send_command()` | How `dash <verb> "<json>"` is line-framed and why the JSON is wrapped in `"…"` (G-7 quote-leading tokeniser). |
| `read_one_reply()` | The `OK:` / `ERR:` / `EVT:` reply convention — one line at a time. |
| Step 1 (`dash idle`) | The first command after connect should always reset the device to a known scene. |
| Step 2 (`dash snapshot`) | The full v1 snapshot shape: `agents[]`, `totals{}`, per-agent identity = `(kind, session_id)`. |
| Step 3 (`dash idle`) | What the bridge does when the last agent goes away. |

## Run it

```bash
# Terminal A — start the device stand-in
python tools/mock_device_v1.py --port 9876 -v

# Terminal B — drive it
python examples/01_minimal/run.py
```

Expected last line: `OK — minimal round-trip complete.` with exit code 0.

## Next

- [`02_two_agents`](../02_two_agents/) — two concurrent agents,
  live-toggling between them. Demonstrates the `(kind, session_id)` slot
  model.
- [`03_prompt_roundtrip`](../03_prompt_roundtrip/) — sends a `dash prompt`,
  waits for the device's `EVT: permission` decision.
