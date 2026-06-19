"""Classify agent turns into device-facing awaiting states."""

from __future__ import annotations

import re


NUMBERED_LINE_RE = re.compile(
    r"^\s*(?:[\d]+\s*[\.\)]\s+|[\-\*•]\s+|[a-eA-E]\s*[\.\)]\s+)",
    re.MULTILINE,
)
CLARIFY_KEYWORDS = (
    "did you mean",
    "do you mean",
    "could you clarify",
    "could you confirm",
    "to clarify",
    "ambiguous",
    "unclear",
    "not sure which",
    "which one",
    "which of these",
    "should i assume",
    "want me to",
    "shall i",
)


def _has_numbered_options(text: str, min_options: int = 2) -> bool:
    if not text:
        return False
    matches = NUMBERED_LINE_RE.findall(text)
    return len(matches) >= min_options


def _extract_options(text: str, max_n: int = 4) -> list[str]:
    out = []
    for line in text.splitlines():
        match = re.match(
            r"^\s*(?:\d+\s*[\.\)]|\-|\*|•|[a-eA-E]\s*[\.\)])\s+(.+?)\s*$",
            line,
        )
        if match:
            out.append(match.group(1)[:32])
            if len(out) >= max_n:
                break
    return out


def _short_sentences(text: str, max_chars: int = 80) -> list[str]:
    text = " ".join(text.split())
    text = text.rstrip(".?!:;,")
    if len(text) <= max_chars:
        return [text]
    cut = text.rfind(" ", 0, max_chars)
    if cut < 12:
        cut = max_chars
    return [text[:cut], text[cut:].lstrip()[: max_chars - 4] + "…"]


def _ends_with_question(text: str) -> bool:
    stripped = text.rstrip()
    return stripped.endswith("?") or stripped.endswith("？")


def classify_awaiting(
    event_type: str,
    last_assistant_text: str,
    tool_name: str = "",
    tool_input_summary: str = "",
) -> tuple[str, list[str]]:
    """Return awaiting kind plus short context lines for the device."""
    if event_type == "pre_tool_use_permission":
        ctx = [f"{tool_name}:" if tool_name else "tool:"]
        if tool_input_summary:
            ctx.append(tool_input_summary[:60])
        return "approve", ctx

    text = (last_assistant_text or "").strip()
    if not text:
        return "continue", ["finished its turn"]

    if _has_numbered_options(text, min_options=2):
        opts = _extract_options(text, max_n=4)
        if opts:
            joined = "  ".join(opts[:3])
            lead = text.split("\n", 1)[0]
            first_num = NUMBERED_LINE_RE.search(text)
            if first_num and first_num.start() > 0:
                lead_lines = text[: first_num.start()].strip().splitlines()
                lead = lead_lines[-1] if lead_lines else ""
                lead = lead.rstrip(":.").strip()
            if lead and len(lead) < 60:
                lead_clean = lead.rstrip("?!.:;").strip()
                return "pick", [lead_clean[:48] + ":", joined[:60]]
            return "pick", [f"{len(opts)} options:", joined[:60]]

    lowered = text.lower()
    if any(keyword in lowered for keyword in CLARIFY_KEYWORDS):
        return "clarify", _short_sentences(text)

    sentences = re.split(r"(?<=[\.\!\?])\s+", text)
    last_sentence = sentences[-1] if sentences else ""
    if _ends_with_question(last_sentence) and 4 < len(last_sentence) < 200:
        return "type", _short_sentences(last_sentence)

    return "continue", _short_sentences(text)
