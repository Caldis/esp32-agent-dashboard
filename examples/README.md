# Examples

Three self-contained Python scripts that talk to
[`tools/mock_device_v1.py`](../tools/mock_device_v1.py) over TCP using
the v1 wire protocol. Each example is the minimum code that demonstrates
exactly one concept — read them top to bottom.

| Example | Concept | LOC |
|---|---|---|
| [`01_minimal`](./01_minimal/) | the smallest possible round-trip: `idle → sessions → idle` | ~120 |
| [`02_two_agents`](./02_two_agents/) | the `(kind, session_id)` slot model, two concurrent agents | ~140 |
| [`03_prompt_roundtrip`](./03_prompt_roundtrip/) | a `dash prompt` and the `EVT: permission` decision that comes back | ~130 |

## Requirements

- Python 3.11+
- Standard library only — no `pip install` needed for the examples
- The mock device, started in another terminal:

  ```bash
  python tools/mock_device_v1.py --port 9876 -v
  ```

## Run them all

From the repo root:

```bash
# Terminal A — once, leave running
python tools/mock_device_v1.py --port 9876 -v

# Terminal B
python examples/01_minimal/run.py
python examples/02_two_agents/run.py --duration 4
python examples/03_prompt_roundtrip/run.py
```

Each script exits 0 on success.

## Going further

Once these click, read [`PROTOCOL.md`](../PROTOCOL.md) for the full v1
verb set (`dash config`, `dash time`, `dash health`) and
[`tools/claude_buddy_bridge.py`](../tools/claude_buddy_bridge.py) for
the production-grade host bridge that drives a real Claude Code session.
