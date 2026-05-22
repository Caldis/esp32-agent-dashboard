# On-Device AI Summarisation — v1.9.0 design doc

> **Status**: scaffolding for v1.9.0. Architecture only. No model is
> shipped in v0; `ai_summarise()` returns a deterministic placeholder.
> See `docs/MODEL_CARD.md` for the selected model's provenance and
> intended use.

## Goal

A tiny language model embedded on the ESP32-S3-Touch-AMOLED-2.16 that
turns the last **N=10** transcript entries (per agent slot) into the
one-line `msg` field rendered on `scene_sessions` and
`scene_dashboard`. Today the host bridge supplies `msg`; once v1.9.0
ships the device synthesises it locally from the same `entries[]`
array the snapshot already carries.

The primary user value is **privacy**: the model runs on-device, no
transcript line ever leaves the LAN — let alone the chip — and the
"agent activity" surface no longer depends on the host having a
network reachability or LLM API key. Privacy is the headline story,
not the latency story.

### Numeric targets

| Target | Value | Rationale |
|---|---|---|
| Inference budget | < 2 s wall-clock | Snapshot push cadence is ~250 ms; we tolerate one summarise call per agent per few seconds |
| Output length | ≤ 80 chars (fits `AGENT_ENTRY_TEXT_MAX`) | Hard cap; the existing `msg` field is `AGENT_MSG_MAX = 128` and renders to one line |
| Concurrency | 1 (serial) | Only one summarise call in flight; queue per agent slot |
| Idle interruption | none | LVGL frame rate must not stutter while we summarise |
| Network | **none** | Loadable model lives in flash partition; no OTA call to outside services |

## Hardware budget

ESP32-S3 (Waveshare 2.16" AMOLED board, classic S3 not S3R8):

```
Flash      16 MB           — partitions.csv splits 2x OTA (~6 MB each) + 1 MB NVS + 2 MB model
PSRAM       8 MB octal     — LVGL framebuffer ~466*466*2*2 = 868 KB; rest free for model weights
DRAM      512 KB           — stack + heap; reserve ≤ 64 KB for inference scratch
CPU       240 MHz dual core, no NPU/no vector ext on classic S3
```

The classic S3 has no AI accelerator. The **S3R8** variant adds
"Vector instructions" (esp-dsp can use them for SIMD), but the
Waveshare 2.16 AMOLED carries a plain S3. We assume scalar inference.

## Model selection

We evaluated five candidates against the budget. **A custom T5 / BART
summariser fine-tuned for code-tool transcripts (~25 M params, INT8)**
is the only realistic shipping option. SmolLM-135M Q4_K_M is the
best off-the-shelf option for a v1 milestone before we ship a custom
distilled model.

| Candidate | Params | Quantised | Fits PSRAM? | Fits CPU budget? | Verdict |
|---|---|---|---|---|---|
| Qwen3-0.6B Q4_K_M | 600 M | ~380 MB | No (60x over) | No | Reject |
| TinyLlama-1.1B Q4_K_M | 1.1 B | ~640 MB | No | No | Reject |
| Phi-3.5-mini Q4_K_M | 3.8 B | ~2.4 GB | No | No | Reject |
| SmolLM-135M Q4_K_M | 135 M | ~80 MB | Yes (10x headroom) | Marginal: ~6 t/s scalar S3 | **Adopt for v1 stub** |
| Custom T5-small fine-tune | 25 M | ~25 MB INT8 | Yes (300x headroom) | Yes: ~30 t/s scalar S3 | **Adopt for v2 ship** |

### Why SmolLM-135M is the right v1 anchor

- Open weights (Apache-2.0). HF: `HuggingFaceTB/SmolLM-135M-Instruct`.
- Q4_K_M GGUF lands at ~80 MB. Fits in the 2 MB-aligned model
  partition after we expand `partitions.csv` (out of scope for AI1
  — TRANS1/SEC1 will adjust).
- Has an "Instruct" variant so we can prompt it directly:
  `"Summarise this agent transcript in one short line: ..."`
- Runs in `llama.cpp` and there is a working ESP-IDF port
  (`llama2.c-esp32` style minimal C inference loop) we can lift.

### Why a custom T5-small is the eventual answer

- 25 M params is small enough to ship in the 4 MB OTA partition
  alongside firmware without growing flash.
- T5 is encoder-decoder: ideal shape for "compress 10 lines → 1 line"
  with strict output length control.
- Fine-tuned on a corpus of **real** dashboard transcripts
  (the `(transcript, expected_summary)` pairs the eval harness
  generates) so it will outperform a generalist 135 M model on
  this narrow task while using a fifth of the memory.
- INT8 inference fits cleanly in scalar S3 without quantisation
  pain points (4-bit GGUF requires more careful packing/unpacking).

For v1.9.0 ship we will likely train the T5 ourselves and skip
SmolLM. SmolLM is documented here because it gives the team a
"buy" option if the "build" option slips.

## Inference engine

We adopt a fork of **`llama2.c`** (Karpathy's minimal C inference
loop) ported to ESP-IDF, rather than the full `llama.cpp` runtime:

| Engine | Pros | Cons | Verdict |
|---|---|---|---|
| `llama.cpp` (full) | Battle-tested, all quant formats | ~600 KB binary, threading assumptions, GPU detection scaffolding we don't need | Reject |
| `llama2.c` (Karpathy minimal) | ~1 KLOC pure C, single allocation, no threads | Only supports its own export format | **Adopt** — we control the model format anyway |
| `micro-llm.cpp` | Designed for MCUs | Project archived, no maintenance | Reject |
| Custom T5 fp16 in C | Smallest possible, encoder-decoder fits task | Requires us to write attention from scratch | Adopt for v2 ship (alongside the custom model) |

The `llama2.c` core is ~30 KB compiled. We add ESP-specific glue:
PSRAM allocation, `esp_dsp` matmul stubs (for when we move to S3R8),
deterministic stop on newline.

## Memory budget

Numbers below are for SmolLM-135M Q4_K_M (the v1 anchor). The custom
T5 path is **smaller** across the board.

| Region | Bytes | Notes |
|---|---|---|
| Flash: model partition | 80 MB | Won't fit on 16 MB; **blocker**: we either (a) ship SmolLM via OTA from a paired phone, (b) shrink to a custom 25 M T5, or (c) skip on-device inference and document the constraint in the model card. We choose (b) for ship; (a) is a stretch. |
| Flash: model partition (T5 25M INT8) | 25 MB | Still too big — final ship target is sub-4 MB |
| Flash: model partition (T5 25M INT4) | ~14 MB | Plausible; carves into one OTA slot |
| Flash: model partition (TinyT5 8M INT8) | ~8 MB | **Ship target.** Trades quality for fit |
| PSRAM: model resident | matches flash size | We mmap the partition into PSRAM at boot; partial loading possible if it doesn't fit |
| PSRAM: kv-cache (T5 8M) | ~256 KB | 12 layers * 64 dim * 2 (k,v) * 10 tokens * fp16 |
| PSRAM: tokeniser vocab | ~150 KB | SentencePiece BPE, ~30 k tokens |
| DRAM: inference scratch | ~32 KB | matmul tiling buffers, softmax workspace |
| DRAM: input/output strings | 1.2 KB | `AGENT_ENTRY_TEXT_MAX * AGENT_ENTRY_COUNT + AGENT_MSG_MAX` |
| **Free PSRAM after model** | **~6 MB** | LVGL framebuffer + everything else stays in the same place |

The LVGL framebuffer (~868 KB) and the agent_state slot array
(~5 KB) sit in their existing allocations. The summariser does not
touch them.

## Threading model

```
+--------------------------+        +----------------------------+
| console_protocol task    |        | LVGL render task           |
| (dash snapshot handler)  |        | (per_tick scene callbacks) |
+----+---------------------+        +-----------+----------------+
     |                                          |
     | mutates agent_slot_t.entries[]           | reads slot.msg under lock
     | (under agent_state lock)                 |
     |                                          |
     v                                          ^
+------------------------+                      |
| ai_summarise_task      |  --> writes slot.msg + bumps slot.entry_seq
| (priority = tskIDLE+1) |      under agent_state lock
| stack: 16 KB           |
| period: poll queue     |
+------------------------+
```

Key invariants:

1. The summariser runs on its own low-priority FreeRTOS task. It
   never blocks console_protocol or LVGL.
2. The summariser takes the agent_state lock only to copy entries
   in (read-side) and to copy the result out (write-side).
   Inference happens with the lock released.
3. Only one inference call is in flight. New summarise requests for
   the same agent slot coalesce (`entry_seq` bookkeeping).
4. If inference takes > 5 s the task logs `EVT: ai_slow elapsed=Ns`
   so OBS1's telemetry sees it.

## Privacy story (primary user benefit)

The phrasing for the marketing / README copy:

> Your AI agent transcripts never leave the device. The summariser
> that turns "Read main.c / Edit main.c / Bash git push" into
> "Pushed main.c edits" runs on the ESP32 itself. No tokens are
> shipped to OpenAI, Anthropic, or any other endpoint. Pull the
> network cable, the summary still works.

Concretely we enforce this through:

1. **No outbound HTTP** in `main/ai/`. The component does not link
   `esp_http_client` or `esp_websocket_client`.
2. **No DNS lookups**. `ai_summarise()` only touches PSRAM and the
   model partition.
3. **Documented in the model card** (this directory's sibling doc).
4. **Smoke-tested**: a planned `tools/smoke.ps1` gate runs
   `idf.py size-components --json` and asserts the ai component has
   zero references to networking symbols.

The custom-model story sharpens this: we train on transcripts we
control, ship the weights in firmware, and the user can audit the
binary for any external symbol.

## Failure modes

| Failure | Behaviour | UI |
|---|---|---|
| Model not flashed | `ai_summarise_init` returns false | `msg` falls back to host-supplied value (today's behaviour) |
| Inference > 2 s | Cancel, return partial output | `msg` shows last entry summary verbatim; `EVT: ai_timeout` |
| Output > 80 chars | Truncate at last word boundary, append `…` | Clean truncation, no clipping |
| OOM in PSRAM | `ai_summarise_init` returns false; module disables itself | Same as "not flashed" |
| Bad UTF-8 in transcript | SentencePiece BPE falls back to byte-level; never crashes | Garbled-but-bounded output |

## Build integration

The component is Kconfig-gated behind `CONFIG_AI_SUMMARISE`
(default n). When n, `ai_summarise.c` compiles to a no-op stub —
the firmware builds identically to v1.8.x.

`main/CMakeLists.txt` will need this once integration lands (NOT
done in this AI1 cycle — outside the file ownership boundary):

```cmake
# Add to SRCS:
"ai/ai_summarise.c"
# Add a PRIV_REQUIRES entry only if CONFIG_AI_SUMMARISE is on:
# (idf_component_register doesn't conditional cleanly; instead the
#  .c file is always compiled and guarded by the #ifdef inside.)
```

Build integration note is here so the firmware engineer (F3) merging
v1.9.0 doesn't need to spelunk for it.

## Open questions for v1.9.0 final ship

1. **Where does the model live?** 16 MB flash with 2x OTA + NVS +
   model = needs partitions.csv rewrite. SEC1 owns OTA so the
   partition layout change is their cycle.
2. **Custom T5 training data.** Do we use the eval dataset (20
   pairs) as training, or expand to 5000+ pairs via host-side LLM
   distillation? Distillation needs a budget; orchestrator approval.
3. **i18n.** v1.8.0 ships zh-CN/en/ja. The summariser is English-
   only at v1.9.0; non-English transcripts fall back to host `msg`.
4. **Quantisation accuracy.** INT4 vs INT8 vs fp16 — the eval harness
   measures this; we pick the smallest that keeps BLEU within 10%
   of fp16.

## File map

```
main/ai/ai_summarise.h     — public C API (3 functions)
main/ai/ai_summarise.c     — stub implementation
tools/ai/prepare_model.py  — GGUF → on-device blob converter (stub)
tools/ai/eval_summariser.py — eval harness (real)
tools/ai/eval_dataset.jsonl — 20 (transcript, summary) pairs
docs/MODEL_CARD.md         — selected model provenance + caveats
docs/ON_DEVICE_AI.md       — this doc
```
