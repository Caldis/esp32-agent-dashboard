#!/usr/bin/env python3
"""eval_summariser.py — v1.9.0 summariser evaluation harness.

Reads ``tools/ai/eval_dataset.jsonl`` (20 ``(transcript, reference_summary)``
pairs derived from real event streams in ``tools/sample_dual.jsonl`` +
``tools/sample_session.jsonl``), runs each transcript through a
candidate summariser, and emits ``metrics.json`` with BLEU-4 / ROUGE-L
/ length statistics.

This is the harness for the model that doesn't exist yet. We ship the
harness first so the bench gate is wired before the model lands. When
``v1.9.0`` final ships, the same script will accept ``--engine
on-device`` and stream transcripts through ``ai_summarise()`` via a
USB-Serial JTAG round-trip; the metrics file's shape stays identical.

Engines:

  --engine dry-run   (default)   echoes a deterministic ``[ref]`` style
                                  fallback. Useful when no host LLM key
                                  is in the environment and we just want
                                  to check the harness end-to-end.
  --engine openai                Uses OPENAI_API_KEY if present. ANY
                                  failure (no key, no network, rate
                                  limit) falls back to dry-run with a
                                  warning per row.
  --engine anthropic             Uses ANTHROPIC_API_KEY if present.
                                  Same fallback behaviour.

Reference / Anthropic / OpenAI calls are PLACEHOLDERS for the v0
scaffolding. They make HTTP calls only when an API key is in the
environment; otherwise the script is offline and deterministic.

Usage::

    python tools/ai/eval_summariser.py \
        --dataset tools/ai/eval_dataset.jsonl \
        --out    metrics.json \
        --engine dry-run

CLI exit codes:

    0   metrics file written, no protocol errors
    2   dataset file missing or malformed
    3   metrics file write failed
"""

from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import sys
import time
from collections import Counter
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Callable

# ---- summariser engines --------------------------------------------------


@dataclass
class EngineResult:
    summary: str
    latency_ms: float
    engine: str
    fallback_reason: str | None = None


def engine_dry_run(transcript: str, reference: str | None = None) -> EngineResult:
    """Deterministic offline 'summariser'.

    Picks the most recent tool action from the transcript and emits a
    "[ref] <tool>: <text>" line. Not a model — a debug-friendly stub.
    """
    t0 = time.perf_counter()
    last_tool_line = None
    for line in transcript.splitlines():
        line = line.strip()
        if line.startswith("tool "):
            last_tool_line = line
    if last_tool_line is None:
        # Fall through to the first non-empty line.
        for line in transcript.splitlines():
            if line.strip():
                last_tool_line = line.strip()
                break
    summary = f"[ref] {last_tool_line or 'no transcript'}"
    return EngineResult(
        summary=summary[:128],
        latency_ms=(time.perf_counter() - t0) * 1000.0,
        engine="dry-run",
    )


def engine_openai(transcript: str, reference: str | None = None) -> EngineResult:
    key = os.environ.get("OPENAI_API_KEY")
    if not key:
        r = engine_dry_run(transcript, reference)
        r.engine = "openai"
        r.fallback_reason = "OPENAI_API_KEY not set"
        return r
    # Real network call deliberately deferred — see docstring. We keep
    # the engine 'real' enough that wiring it later is one import away.
    try:
        import urllib.request  # noqa: F401 — proves the import works
    except Exception as exc:  # pragma: no cover — defensive
        r = engine_dry_run(transcript, reference)
        r.engine = "openai"
        r.fallback_reason = f"urllib import failed: {exc!r}"
        return r
    r = engine_dry_run(transcript, reference)
    r.engine = "openai"
    r.fallback_reason = (
        "v0 scaffolding: real OpenAI call gated behind v1.9.0 ship cycle"
    )
    return r


def engine_anthropic(transcript: str, reference: str | None = None) -> EngineResult:
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        r = engine_dry_run(transcript, reference)
        r.engine = "anthropic"
        r.fallback_reason = "ANTHROPIC_API_KEY not set"
        return r
    r = engine_dry_run(transcript, reference)
    r.engine = "anthropic"
    r.fallback_reason = (
        "v0 scaffolding: real Anthropic call gated behind v1.9.0 ship cycle"
    )
    return r


ENGINES: dict[str, Callable[[str, str | None], EngineResult]] = {
    "dry-run":   engine_dry_run,
    "openai":    engine_openai,
    "anthropic": engine_anthropic,
}


# ---- metrics -------------------------------------------------------------


def tokenise(text: str) -> list[str]:
    """Whitespace + lowercase tokenisation. Cheap and deterministic so
    the metric is reproducible. The real on-device tokeniser is BPE;
    we only need a per-word view here for BLEU / ROUGE."""
    return [t.lower() for t in text.split() if t]


def _ngrams(tokens: list[str], n: int) -> Counter:
    if len(tokens) < n:
        return Counter()
    return Counter(tuple(tokens[i:i + n]) for i in range(len(tokens) - n + 1))


def bleu4(hypothesis: str, reference: str) -> float:
    """Simple corpus-of-one BLEU-4 with smoothing.

    Not nltk-grade; intentionally dependency-free so the harness runs
    in a clean Python environment with no pip install.
    """
    hyp = tokenise(hypothesis)
    ref = tokenise(reference)
    if not hyp or not ref:
        return 0.0
    precisions = []
    for n in range(1, 5):
        hyp_n = _ngrams(hyp, n)
        ref_n = _ngrams(ref, n)
        if not hyp_n:
            precisions.append(1e-9)  # smoothing — avoid log(0)
            continue
        overlap = sum(min(c, ref_n.get(g, 0)) for g, c in hyp_n.items())
        precisions.append((overlap + 1e-9) / (sum(hyp_n.values()) + 1e-9))
    geo_mean = math.exp(sum(math.log(p) for p in precisions) / len(precisions))
    bp = 1.0 if len(hyp) > len(ref) else math.exp(1 - len(ref) / max(len(hyp), 1))
    return bp * geo_mean


def rouge_l_f1(hypothesis: str, reference: str) -> float:
    """ROUGE-L F1 via classic dynamic-programming LCS."""
    hyp = tokenise(hypothesis)
    ref = tokenise(reference)
    if not hyp or not ref:
        return 0.0
    # LCS length
    m, n = len(hyp), len(ref)
    dp = [[0] * (n + 1) for _ in range(m + 1)]
    for i in range(m):
        for j in range(n):
            if hyp[i] == ref[j]:
                dp[i + 1][j + 1] = dp[i][j] + 1
            else:
                dp[i + 1][j + 1] = max(dp[i + 1][j], dp[i][j + 1])
    lcs = dp[m][n]
    if lcs == 0:
        return 0.0
    p = lcs / m
    r = lcs / n
    return 2 * p * r / (p + r)


# ---- harness -------------------------------------------------------------


@dataclass
class RowResult:
    id: str
    transcript_chars: int
    reference: str
    hypothesis: str
    engine: str
    fallback_reason: str | None
    latency_ms: float
    bleu4: float
    rouge_l: float
    output_chars: int


@dataclass
class Metrics:
    engine: str
    dataset_path: str
    n_rows: int
    rows: list[RowResult] = field(default_factory=list)
    bleu4_mean: float = 0.0
    rouge_l_mean: float = 0.0
    latency_ms_mean: float = 0.0
    latency_ms_p95: float = 0.0
    output_chars_mean: float = 0.0
    length_compliance_80: float = 0.0
    length_compliance_128: float = 0.0
    fallback_count: int = 0


def load_dataset(path: Path) -> list[dict]:
    if not path.is_file():
        raise FileNotFoundError(f"dataset not found: {path}")
    rows = []
    with path.open("r", encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, start=1):
            line = raw.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: {exc}") from exc
            for required in ("id", "transcript", "reference_summary"):
                if required not in rec:
                    raise ValueError(
                        f"{path}:{lineno}: missing field {required!r}"
                    )
            rows.append(rec)
    return rows


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * p
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return s[int(k)]
    return s[f] + (s[c] - s[f]) * (k - f)


def run_eval(dataset_path: Path, engine_name: str) -> Metrics:
    rows = load_dataset(dataset_path)
    engine = ENGINES[engine_name]
    metrics = Metrics(
        engine=engine_name,
        dataset_path=str(dataset_path),
        n_rows=len(rows),
    )
    for rec in rows:
        result = engine(rec["transcript"], rec["reference_summary"])
        b = bleu4(result.summary, rec["reference_summary"])
        r = rouge_l_f1(result.summary, rec["reference_summary"])
        metrics.rows.append(
            RowResult(
                id=rec["id"],
                transcript_chars=len(rec["transcript"]),
                reference=rec["reference_summary"],
                hypothesis=result.summary,
                engine=result.engine,
                fallback_reason=result.fallback_reason,
                latency_ms=result.latency_ms,
                bleu4=b,
                rouge_l=r,
                output_chars=len(result.summary),
            )
        )
    if metrics.rows:
        metrics.bleu4_mean = statistics.mean(r.bleu4 for r in metrics.rows)
        metrics.rouge_l_mean = statistics.mean(r.rouge_l for r in metrics.rows)
        metrics.latency_ms_mean = statistics.mean(r.latency_ms for r in metrics.rows)
        metrics.latency_ms_p95 = percentile(
            [r.latency_ms for r in metrics.rows], 0.95
        )
        metrics.output_chars_mean = statistics.mean(
            r.output_chars for r in metrics.rows
        )
        metrics.length_compliance_80 = sum(
            1 for r in metrics.rows if r.output_chars <= 80
        ) / len(metrics.rows)
        metrics.length_compliance_128 = sum(
            1 for r in metrics.rows if r.output_chars <= 128
        ) / len(metrics.rows)
        metrics.fallback_count = sum(
            1 for r in metrics.rows if r.fallback_reason
        )
    return metrics


def write_metrics(metrics: Metrics, out: Path) -> None:
    payload = asdict(metrics)
    out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="eval_summariser.py",
        description="Run the v1.9.0 summariser bench against a JSONL dataset.",
    )
    here = Path(__file__).resolve().parent
    p.add_argument(
        "--dataset",
        type=Path,
        default=here / "eval_dataset.jsonl",
        help="Path to eval_dataset.jsonl (default: tools/ai/eval_dataset.jsonl).",
    )
    p.add_argument(
        "--out",
        type=Path,
        default=here / "metrics.json",
        help="Path to write metrics.json (default: tools/ai/metrics.json).",
    )
    p.add_argument(
        "--engine",
        choices=sorted(ENGINES.keys()),
        default="dry-run",
        help="Which summariser to call. Network engines fall back to dry-run when no API key is set.",
    )
    p.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress the human-readable summary on stdout.",
    )
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        metrics = run_eval(args.dataset, args.engine)
    except FileNotFoundError as exc:
        print(f"eval_summariser: {exc}", file=sys.stderr)
        return 2
    except ValueError as exc:
        print(f"eval_summariser: malformed dataset: {exc}", file=sys.stderr)
        return 2

    try:
        write_metrics(metrics, args.out)
    except OSError as exc:
        print(f"eval_summariser: cannot write {args.out}: {exc}", file=sys.stderr)
        return 3

    if not args.quiet:
        print(f"eval_summariser: engine={metrics.engine}, n={metrics.n_rows}")
        print(f"  bleu4_mean         = {metrics.bleu4_mean:.3f}")
        print(f"  rouge_l_mean       = {metrics.rouge_l_mean:.3f}")
        print(f"  latency_ms_mean    = {metrics.latency_ms_mean:.3f}")
        print(f"  latency_ms_p95     = {metrics.latency_ms_p95:.3f}")
        print(f"  output_chars_mean  = {metrics.output_chars_mean:.1f}")
        print(f"  length_compliance_80  = {metrics.length_compliance_80:.2f}")
        print(f"  length_compliance_128 = {metrics.length_compliance_128:.2f}")
        print(f"  fallback_count     = {metrics.fallback_count}")
        print(f"  wrote              = {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
