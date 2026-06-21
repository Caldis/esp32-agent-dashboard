"""Regression tests for hook_dispatch logic added this session:
  - _read_turn_output_tokens: pull this turn's output tokens from a transcript
  - _forward_allowed: targeted-debug session filtering
Run: python -m pytest tools/test_hook_dispatch.py
"""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hook_dispatch  # noqa: E402


def _write_jsonl(rows):
    f = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False, encoding="utf-8")
    for r in rows:
        f.write(json.dumps(r) + "\n")
    f.close()
    return f.name


# ── _read_turn_output_tokens ─────────────────────────────────────────────────

def test_turn_tokens_sums_assistant_output():
    """Sum output_tokens of assistant msgs in the latest turn (CC's usage is
    nested under message.usage)."""
    p = _write_jsonl([
        {"type": "user", "message": {"role": "user"}},
        {"type": "assistant", "message": {"role": "assistant", "usage": {"output_tokens": 100}}},
        {"type": "assistant", "message": {"role": "assistant", "usage": {"output_tokens": 50}}},
    ])
    try:
        assert hook_dispatch._read_turn_output_tokens(p) == 150
    finally:
        os.unlink(p)


def test_turn_tokens_stops_at_user_boundary():
    """Tokens from a PREVIOUS turn (before the last user msg) must not count."""
    p = _write_jsonl([
        {"type": "assistant", "message": {"usage": {"output_tokens": 999}}},  # prev turn
        {"type": "user", "message": {}},
        {"type": "assistant", "message": {"usage": {"output_tokens": 42}}},
    ])
    try:
        assert hook_dispatch._read_turn_output_tokens(p) == 42
    finally:
        os.unlink(p)


def test_turn_tokens_missing_file_is_zero():
    assert hook_dispatch._read_turn_output_tokens("/no/such/file.jsonl") == 0
    assert hook_dispatch._read_turn_output_tokens("") == 0


# ── _forward_allowed (targeted debug) ────────────────────────────────────────

def _with_target(cfg):
    """Point hook_dispatch at a temp target file holding cfg; returns (path, restore)."""
    tf = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
    if cfg is not None:
        tf.write(json.dumps(cfg))
    tf.close()
    orig = hook_dispatch._TARGET_FILE
    hook_dispatch._TARGET_FILE = tf.name
    return tf.name, orig


def test_forward_allowed_no_file_allows_all():
    orig = hook_dispatch._TARGET_FILE
    hook_dispatch._TARGET_FILE = "/no/such/target.json"
    try:
        assert hook_dispatch._forward_allowed({"session_id": "anything"})
    finally:
        hook_dispatch._TARGET_FILE = orig


def test_forward_allowed_exclude():
    path, orig = _with_target({"exclude": ["fb0f"]})
    try:
        assert not hook_dispatch._forward_allowed({"session_id": "fb0f6612-xyz"})  # excluded
        assert hook_dispatch._forward_allowed({"session_id": "other-sess"})        # allowed
    finally:
        hook_dispatch._TARGET_FILE = orig
        os.unlink(path)


def test_forward_allowed_only():
    path, orig = _with_target({"only": ["test-"]})
    try:
        assert hook_dispatch._forward_allowed({"session_id": "test-123"})          # in allowlist
        assert not hook_dispatch._forward_allowed({"session_id": "fb0f6612"})      # not listed
    finally:
        hook_dispatch._TARGET_FILE = orig
        os.unlink(path)


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("ALL PASS")
