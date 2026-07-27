# attic — retired sources, kept for reference

Code that was deliberately removed from the build but is worth reading
before re-solving a problem it already solved. Nothing here compiles as
part of the firmware; `main/CMakeLists.txt` does not reference it.

| file | retired | why |
|---|---|---|
| `scenes/scene_overview.c` | v5.2 | With one agent the overview and the dashboard said the same thing. `dash idle` now aliases to the dashboard. |
| `scenes/scene_prompt.c` | v5.2 | Approvals happen in the terminal, not on the panel. `dash prompt` is a no-op ACK and `prompt_active` must stay false forever (see `harness/agent_snapshot_apply.c`). |
| `scenes/scene_awaiting.c` | v6.0 | It and the dashboard's gold pose were two near-identical gold pages a key press flipped between. The dashboard gold pose absorbed it; "takeover" became a pull. |
| `pet.c` / `pet.h` | v5.3 | The mascot was charm, not signal. The breathing pulse ring is the one status glyph device-wide, colour = state. |

Why an attic instead of deleting: each of these encodes a decision with a
reason, and the reasons are recorded in CLAUDE.md. Why not leave them in
`main/`: four unbuilt files sitting beside three built ones made every
grep and every "which scenes exist?" question ambiguous — the cost landed
on navigation, every session, forever.

Deleting is fine once git history is enough. It is not the default because
the retirement rationale is the valuable part, not the code.
