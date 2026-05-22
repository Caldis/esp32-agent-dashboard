#!/usr/bin/env python3
"""prepare_model.py — convert a GGUF/HF model blob into the on-device
format expected by ``main/ai/ai_summarise.c``.

v1.9.0 scaffolding stub. The shape of the input/output and the CLI
contract is real; the conversion itself is not. The stub writes a
fixed-layout header followed by a SHA-256 hash of the input bytes, so
the firmware can verify it's loading the expected blob even before the
real conversion lands.

Usage:

    python tools/ai/prepare_model.py \
        --model SmolLM-135M-Q4_K_M.gguf \
        --out  model.bin

Real conversion (out of scope for AI1 scaffolding cycle) will:

  1. Parse the GGUF metadata and weight tensors.
  2. Re-quantise from Q4_K_M to a flat layout the llama2.c-esp32 fork
     understands (sequential headerless float / int8 tensor blocks).
  3. Pack tokeniser vocab + merges into the blob so the firmware
     doesn't need a second partition.
  4. Append a SHA-256 hash and emit ``model.bin.sha256`` alongside so
     the firmware bootloader can refuse mismatched blobs.

The stub does steps (1) and (3) as no-ops; step (4) is implemented
faithfully so the integration smoke test ``firmware-rejects-bad-hash``
can run end-to-end.

Header layout (24 bytes, big-endian):
    +0   4   magic = b"AID0"   (Agent-dashboard model, v0 layout)
    +4   2   header_version    (uint16 = 1)
    +6   2   reserved          (zero)
    +8   8   payload_size      (uint64, size in bytes of the payload
                                following the header — not including
                                the trailing 32-byte SHA-256)
    +16  8   reserved          (zero — future use: arch / quant tag)
    +24  N   payload           (the source bytes verbatim in the stub;
                                the real packed model in v1.9.0 ship)
    +24+N 32 sha256(payload)   (trailing — matches model.bin.sha256)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HEADER_MAGIC = b"AID0"
HEADER_VERSION = 1
HEADER_SIZE = 24
SHA256_SIZE = 32


def build_header(payload_size: int) -> bytes:
    """Build the 24-byte file header."""
    if payload_size < 0 or payload_size >= 2**63:
        raise ValueError(f"payload size out of range: {payload_size}")
    return (
        HEADER_MAGIC
        + struct.pack(">H", HEADER_VERSION)
        + struct.pack(">H", 0)  # reserved
        + struct.pack(">Q", payload_size)
        + struct.pack(">Q", 0)  # reserved
    )


def convert(model_in: Path, out: Path, manifest: Path | None) -> dict:
    """Read ``model_in``, write the on-device blob to ``out``.

    Returns a dict of metadata that the caller may print or persist.
    """
    if not model_in.is_file():
        raise FileNotFoundError(f"model not found: {model_in}")

    payload = model_in.read_bytes()
    digest = hashlib.sha256(payload).digest()
    hex_digest = digest.hex()

    header = build_header(len(payload))
    out.write_bytes(header + payload + digest)

    # Companion .sha256 sidecar so the firmware can compare without
    # parsing the full file.
    sidecar = out.with_suffix(out.suffix + ".sha256")
    sidecar.write_text(f"{hex_digest}  {out.name}\n", encoding="utf-8")

    info = {
        "input_path": str(model_in),
        "output_path": str(out),
        "input_bytes": len(payload),
        "output_bytes": HEADER_SIZE + len(payload) + SHA256_SIZE,
        "sha256": hex_digest,
        "header_version": HEADER_VERSION,
        "magic": HEADER_MAGIC.decode("ascii"),
        "warning": (
            "STUB CONVERSION — this is the raw input bytes wrapped with "
            "a header and trailing SHA-256. Real GGUF→on-device "
            "re-quantisation lands in the v1.9.0 ship cycle."
        ),
    }

    if manifest is not None:
        manifest.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")

    return info


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="prepare_model.py",
        description=(
            "Stub converter for the v1.9.0 on-device summariser model. "
            "Wraps the input file with a tiny header and trailing SHA-256."
        ),
    )
    p.add_argument(
        "--model",
        required=True,
        type=Path,
        help="Path to the input model (GGUF in production, any bytes in stub mode).",
    )
    p.add_argument(
        "--out",
        required=True,
        type=Path,
        help="Path to write the on-device blob.",
    )
    p.add_argument(
        "--manifest",
        type=Path,
        default=None,
        help="Optional path for a JSON manifest summarising the conversion.",
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
        info = convert(args.model, args.out, args.manifest)
    except (FileNotFoundError, ValueError) as exc:
        print(f"prepare_model: {exc}", file=sys.stderr)
        return 2

    if not args.quiet:
        print(f"prepare_model: wrote {info['output_path']}")
        print(f"  input_bytes  = {info['input_bytes']}")
        print(f"  output_bytes = {info['output_bytes']}")
        print(f"  sha256       = {info['sha256']}")
        print(f"  note         : {info['warning']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
