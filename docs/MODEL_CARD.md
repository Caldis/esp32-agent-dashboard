# Model card — esp32-agent-dashboard summariser

> Format adapted from the Hugging Face model-card schema and the
> Mitchell et al. (2018) "Model Cards for Model Reporting" paper.
> This card describes the v1 anchor model (`SmolLM-135M-Instruct`)
> and the v2 ship target (a custom-distilled `dashboard-t5-tiny`).
> Both are described because v1.9.0 may ship either depending on
> partition layout decisions in the SEC1 cycle.

## Model details

### v1 anchor: SmolLM-135M-Instruct (Q4_K_M GGUF)

| Field | Value |
|---|---|
| Provenance | `HuggingFaceTB/SmolLM-135M-Instruct` on the Hugging Face Hub |
| License | Apache-2.0 (verified at <https://huggingface.co/HuggingFaceTB/SmolLM-135M-Instruct>) |
| Architecture | Llama-style decoder-only transformer, 135 M parameters |
| Original training data | Cosmopedia-v2, FineWeb-Edu (English) |
| Tokeniser | Byte-level BPE, ~49 k vocab |
| Original precision | bfloat16 |
| On-device precision | INT4 (Q4_K_M GGUF, ~80 MB) |
| Author / maintainer | HuggingFace TB team |
| Version pinned for this project | `v1.0` revision `b3df3b1` (Hugging Face commit hash to be locked at adoption time) |

### v2 ship target: dashboard-t5-tiny (custom)

| Field | Value |
|---|---|
| Provenance | Trained by the AI1 cycle on `tools/ai/eval_dataset.jsonl` plus a planned distillation expansion |
| License | MIT, weights shipped in this repository |
| Architecture | T5-small reduced to 4 enc / 4 dec layers, hidden=256, ~8 M parameters |
| Original training data | (1) the 20 hand-curated transcript pairs in `eval_dataset.jsonl`; (2) host-LLM-distilled expansion to ~5000 pairs; both derived from `tools/sample_dual.jsonl` + `tools/sample_session.jsonl` event streams |
| Tokeniser | SentencePiece BPE, 4096 vocab (covers tool names, file extensions, common verbs) |
| On-device precision | INT8 (target ~8 MB), INT4 fallback (~4 MB) |
| Author / maintainer | esp32-agent-dashboard maintainers |
| Version pinned for this project | trained per-release; weights hash in `tools/ai/model.bin.sha256` |

## Intended use

This model is intended to summarise the last 10 entries of an agent
transcript (tool names + short tool-output summaries) into a single
short English sentence rendered in the `msg` field of the
`agent_slot_t` shown on `scene_dashboard` and `scene_sessions`.

Example mapping:

```
Input transcript (entries[], reverse-chronological):
  10:42 Bash      git push
  10:41 Edit      src/auth.py (+8 -2)
  10:39 Read      src/auth.py (120 lines)

Expected output:
  "Refactored src/auth.py and pushed the change."
```

Acceptable input domains:

- Tool names from the canonical set in `main/tool_icons.c`
  (`Read` / `Edit` / `Bash` / `Grep` / `Glob` / `Write` / `shell` / `Task` /…)
- File paths, tool-output excerpts truncated to
  `AGENT_ENTRY_TEXT_MAX = 80` chars
- Mostly English; some path components, code identifiers

The model is **also** intended to do a coarse intent classification
of `dash prompt` hints in v2 (e.g. tag a pending permission as
"read-only" / "writes-file" / "network" / "destructive"). That is
out of scope for the v1 stub; the C API supports it but the eval
harness does not yet measure it.

## Out of scope (do NOT use the model for)

- General chat with the device. The model has no instruction-
  following safety training calibrated for arbitrary user input.
- Code generation. The model is a summariser, not a generator;
  outputs are bounded to ~80 chars and the architecture (for the v2
  T5) has no decoder vocabulary suitable for emitting code blocks.
- Decision-making on permission prompts. The `dash prompt` flow
  asks the human to approve a tool call. The summariser must not
  auto-decide; v2's intent classification is **advisory only** and
  the human button press remains authoritative.
- Translation. The model is English-only. Non-English transcripts
  should fall back to the host-supplied `msg`.
- Medical / legal / financial advice. The model is not trained for
  any factual-domain reasoning task; it summarises a fixed schema.
- Personally identifying information detection. The model may
  surface usernames, file paths, repository names — that is by
  design (the user **wants** to see "edited src/auth.py"). If
  privacy of file paths matters, redact them host-side **before**
  the snapshot push.

## Bias and safety caveats

### Bias

- The training corpus is English code-tool transcripts. The model
  will systematically produce worse summaries for non-English
  transcripts, transcripts heavy on non-Latin file paths, and
  transcripts from tools outside the canonical set
  (`main/tool_icons.c`).
- The hand-curated 20-pair eval dataset (v2 model only) reflects
  the maintainers' notion of what makes a "good" summary
  (action-verb leading, file-name preserved, count of tool calls
  elided). Other users may have different expectations.
- Distillation expansion uses a host-side LLM (OpenAI or Anthropic
  if a key is in the environment, dry-run `[ref]` placeholder
  otherwise). When a host LLM is used, its biases (style,
  formality, default vocabulary) propagate into the student.
  Documented in `tools/ai/eval_summariser.py` so the trail is
  auditable.

### Safety

- The model runs on-device and produces text rendered as plain LVGL
  labels. There is **no eval / code-execution / shell-out path**
  for model output. The worst case is a misleading `msg` string.
- The model never sees the human's permission decision; it only
  reads `entries[]`. It cannot influence the prompt outcome.
- The model is sandboxed by the `ai_summarise()` C contract: input
  is a `const char *transcript`, output is a fixed-size `char *out_msg`
  buffer. No file I/O, no network, no callbacks back into the
  agent state. See `main/ai/ai_summarise.h`.

### Hallucinations

- Sub-100 M parameter models hallucinate confidently. A summary
  like "Pushed bug fix" when the underlying entry is `git status`
  is plausible and unsafe-looking even though it cannot do harm.
- Mitigation: the v2 fine-tuning loss penalises any token that
  does not appear in the input transcript or the closed
  tool-vocabulary set, biasing the decoder toward extractive
  rather than abstractive output.
- Mitigation (UX): the `msg` field is rendered with the same
  visual weight as a tool entry, not as a system-authoritative
  status. Users learn to read it as a hint.

## Evaluation

### Bench shape

`tools/ai/eval_summariser.py` runs the model (or a host-LLM
reference when no on-device model is present) over the 20 pairs in
`tools/ai/eval_dataset.jsonl` and reports:

| Metric | Definition | Target (v2 ship gate) |
|---|---|---|
| BLEU-4 | corpus BLEU vs reference summary | ≥ 0.30 |
| ROUGE-L F1 | longest-common-subsequence F1 | ≥ 0.45 |
| Length compliance | fraction of outputs ≤ 80 chars | ≥ 0.95 |
| Length compliance (hard cap) | fraction of outputs ≤ 128 chars | 1.0 |
| Token-vocab compliance (v2 only) | fraction of output tokens in tool/file-path set | ≥ 0.80 |
| Latency p95 (on-device) | wall-clock per `ai_summarise()` call | < 2.0 s |
| Tag-leak rate | fraction of outputs containing host-LLM artefacts (`[ref]`, `<unk>`, `### `) | 0.0 |

Bench gates fail-closed: if any metric misses target on the 20-pair
bench, the model does not ship for that release.

### Eval dataset

20 hand-curated `(transcript, expected_summary)` pairs derived from
the real event streams:

- 8 pairs derived from `tools/sample_session.jsonl` (single-agent,
  Claude Code + Codex flows, including one destructive `rm -rf` case)
- 8 pairs derived from `tools/sample_dual.jsonl` (two-agent
  concurrent flows, including the `git diff main..HEAD` + `git push`
  case)
- 4 pairs synthesised by stitching the above into longer 5-entry
  transcripts to exercise the N=10 input window

Each pair is human-reviewed. The dataset lives at
`tools/ai/eval_dataset.jsonl` and is part of the release artefact —
shipping the eval harness without the dataset would let metrics
drift silently.

## Maintenance

- The eval dataset is regenerated whenever
  `tools/sample_session.jsonl` or `tools/sample_dual.jsonl` change.
  The regeneration is currently manual; v1.9.0 ship will add
  `tools/ai/refresh_dataset.py`.
- The model weights are pinned by SHA-256 in
  `tools/ai/model.bin.sha256`. `prepare_model.py` writes this file.
  Firmware refuses to load a model whose hash disagrees.
- The model card (this file) is updated when (a) the model
  architecture changes, (b) the training data set changes by more
  than 20%, or (c) any "out of scope" or "bias" item is added,
  removed, or modified.

## Contact

File issues against the esp32-agent-dashboard repository. Security
issues route to SEC1 (the security-engineer agent role); model-
behaviour issues route to AI1.
