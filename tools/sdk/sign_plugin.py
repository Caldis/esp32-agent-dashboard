#!/usr/bin/env python3
"""
sign_plugin.py — produce ed25519 signatures for an esp32-agent-dashboard plugin.

Workflow (per docs/PLUGIN_SDK.md §5):

    H = sha256( canonicalised_manifest.toml )
    M = sha256( merkle_root_of_source_tree )
    P = b"esp32-agent-dashboard.plugin.v1\\n" + H + M
    sig_author = ed25519_sign(author_secret, P)
    sig_user   = ed25519_sign(user_secret,   P)

The two 64-byte raw signatures are written next to the manifest as:
    manifest.toml.sig.author
    manifest.toml.sig.user

Key sources:
    --author-key   path to author's ed25519 private key (32-byte raw or
                   PEM). Default: $ESP32_AGENT_PLUGIN_AUTHOR_KEY env var,
                   else tools/sign/keys/author.key (SEC1's layout).
    --user-key     path to user-trust private key. Default:
                   $ESP32_AGENT_USER_KEY env var, else
                   ~/.esp32-agent-dashboard/keys/user.key.
    --self-trust   shorthand: if --user-key isn't set, sign with the
                   same key as --author-key. ONLY for local development
                   — on a real device the user-trust pubkey is distinct
                   and provisioned via `dash plugin trust ...`.

SEC1 integration:
    This script ASSUMES `tools/sign/` exists and provides:
        tools/sign/keys/author.key      (ed25519 32-byte seed, 0600)
        tools/sign/keys/author.pub      (32-byte raw public key)
        tools/sign/ed25519_helpers.py   (sign(seed, msg) -> 64 B sig)

    If `tools/sign/ed25519_helpers.py` is importable we delegate to it
    so signing behaviour stays consistent with firmware OTA signing.
    Otherwise we fall back to `cryptography` (`pip install cryptography`)
    and emit a clear hint in --help.

Exit codes:
    0  signed
    2  CLI argument error
    3  missing key file(s)
    4  no crypto backend available (pip install cryptography)
    5  plugin dir invalid (no manifest.toml or malformed)
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
from pathlib import Path
from typing import Callable, Optional


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SEC1_KEYS_DIR = PROJECT_ROOT / "tools" / "sign" / "keys"
SEC1_HELPER   = PROJECT_ROOT / "tools" / "sign"
USER_KEYS_DIR = Path.home() / ".esp32-agent-dashboard" / "keys"

SIGNING_PREFIX = b"esp32-agent-dashboard.plugin.v1\n"


# ----------------------------- crypto backend ---------------------------- #

def _load_sec1_helper() -> Optional[Callable[[bytes, bytes], bytes]]:
    """If SEC1's helper module exists, use it. Returns a sign() fn or None."""
    helper_py = SEC1_HELPER / "ed25519_helpers.py"
    if not helper_py.exists():
        return None
    sys.path.insert(0, str(SEC1_HELPER))
    try:
        import ed25519_helpers  # type: ignore[import-not-found]
        fn = getattr(ed25519_helpers, "sign", None)
        if callable(fn):
            return fn
    except Exception as e:
        print(f"warning: tools/sign/ed25519_helpers.py present but "
              f"unloadable ({e}); falling back to `cryptography`.",
              file=sys.stderr)
    finally:
        # leave sys.path tweak in place — we may need it later
        pass
    return None


def _load_cryptography_backend() -> Optional[Callable[[bytes, bytes], bytes]]:
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey,
        )
    except ImportError:
        return None

    def _sign(seed: bytes, msg: bytes) -> bytes:
        if len(seed) == 32:
            sk = Ed25519PrivateKey.from_private_bytes(seed)
        else:
            # try PEM
            from cryptography.hazmat.primitives.serialization import (
                load_pem_private_key,
            )
            sk = load_pem_private_key(seed, password=None)
        return sk.sign(msg)

    return _sign


def _resolve_sign_fn() -> Callable[[bytes, bytes], bytes]:
    fn = _load_sec1_helper()
    if fn is not None:
        return fn
    fn = _load_cryptography_backend()
    if fn is not None:
        return fn
    print(
        "error: no signing backend available.\n"
        "  Either:\n"
        "    (a) wait for SEC1 to ship tools/sign/ed25519_helpers.py, or\n"
        "    (b) `pip install cryptography` to use the fallback backend.",
        file=sys.stderr,
    )
    sys.exit(4)


# ----------------------------- key loading ------------------------------- #

def _default_author_key() -> Path:
    env = os.environ.get("ESP32_AGENT_PLUGIN_AUTHOR_KEY")
    if env:
        return Path(env)
    return SEC1_KEYS_DIR / "author.key"


def _default_user_key() -> Path:
    env = os.environ.get("ESP32_AGENT_USER_KEY")
    if env:
        return Path(env)
    return USER_KEYS_DIR / "user.key"


def _load_key(path: Path) -> bytes:
    if not path.exists():
        print(
            f"error: key file not found: {path}\n"
            "  hint: SEC1 owns the firmware-signing key layout. Until\n"
            "  tools/sign/keys/ exists, use --self-trust with a dev key\n"
            "  you generated yourself (e.g. via openssl genpkey -algorithm Ed25519).",
            file=sys.stderr,
        )
        sys.exit(3)
    return path.read_bytes()


# ----------------------------- merkle ------------------------------------ #

def _canonicalise_manifest(text: str) -> bytes:
    """Trim trailing whitespace per line, normalise newlines, keep order."""
    lines = [ln.rstrip() for ln in text.replace("\r\n", "\n").split("\n")]
    # drop trailing empty lines to make the canonicalisation deterministic
    while lines and lines[-1] == "":
        lines.pop()
    return ("\n".join(lines) + "\n").encode("utf-8")


def _merkle_root(plugin_dir: Path) -> bytes:
    """sha256 of sorted (relpath, file_sha256) tuples — everything except
    .sig files and __pycache__."""
    accum = hashlib.sha256()
    entries: list[tuple[str, str]] = []
    for path in sorted(plugin_dir.rglob("*")):
        if path.is_dir():
            continue
        rel = path.relative_to(plugin_dir).as_posix()
        if rel.endswith(".sig.author") or rel.endswith(".sig.user"):
            continue
        if "__pycache__" in rel.split("/"):
            continue
        file_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        entries.append((rel, file_hash))
    for rel, h in entries:
        accum.update(f"{rel}\0{h}\n".encode("utf-8"))
    return accum.digest()


def _build_signing_payload(plugin_dir: Path) -> bytes:
    manifest = plugin_dir / "manifest.toml"
    if not manifest.exists():
        print(f"error: {manifest} not found.", file=sys.stderr)
        sys.exit(5)
    h = hashlib.sha256(_canonicalise_manifest(manifest.read_text(encoding="utf-8"))).digest()
    m = _merkle_root(plugin_dir)
    return SIGNING_PREFIX + h + m


# ----------------------------- main -------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="sign_plugin.py",
        description="Sign an esp32-agent-dashboard plugin.",
    )
    parser.add_argument("plugin_dir", help="plugin root (contains manifest.toml)")
    parser.add_argument(
        "--author-key",
        type=Path,
        default=None,
        help="path to author ed25519 private key (default: tools/sign/keys/author.key)",
    )
    parser.add_argument(
        "--user-key",
        type=Path,
        default=None,
        help="path to user-trust ed25519 private key (default: ~/.esp32-agent-dashboard/keys/user.key)",
    )
    parser.add_argument(
        "--self-trust",
        action="store_true",
        help="dev mode: sign user countersignature with author key too",
    )
    args = parser.parse_args(argv)

    plugin_dir = Path(args.plugin_dir).expanduser().resolve()
    if not plugin_dir.is_dir():
        print(f"error: {plugin_dir} is not a directory", file=sys.stderr)
        return 2

    author_key_path = args.author_key or _default_author_key()
    user_key_path   = args.user_key   or _default_user_key()

    author_seed = _load_key(author_key_path)
    if args.self_trust and args.user_key is None:
        user_seed = author_seed
        user_source = f"(self-trust → {author_key_path})"
    else:
        user_seed = _load_key(user_key_path)
        user_source = str(user_key_path)

    sign_fn = _resolve_sign_fn()
    payload = _build_signing_payload(plugin_dir)

    sig_author = sign_fn(author_seed, payload)
    sig_user   = sign_fn(user_seed,   payload)

    if len(sig_author) != 64 or len(sig_user) != 64:
        print(
            f"error: signing backend returned non-64-byte signature "
            f"(author={len(sig_author)}, user={len(sig_user)}). "
            "Plugin signatures must be raw 64-byte ed25519.",
            file=sys.stderr,
        )
        return 4

    sig_author_path = plugin_dir / "manifest.toml.sig.author"
    sig_user_path   = plugin_dir / "manifest.toml.sig.user"

    sig_author_path.write_bytes(sig_author)
    sig_user_path.write_bytes(sig_user)

    print(f"signed: {sig_author_path.name} ({author_key_path.name})")
    print(f"signed: {sig_user_path.name}   {user_source}")
    print(f"payload: {len(payload)} bytes; "
          f"prefix={SIGNING_PREFIX!r}; "
          f"H+M={32+32} bytes appended.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
