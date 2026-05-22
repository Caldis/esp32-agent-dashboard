#!/usr/bin/env python3
"""Capture screenshots from the v1 firmware via the harness `?dump` command.

Usage: python _capture_v1.py <out_path.png> [scene_id]

Sends `?dump w=466` to the device on COM9, decodes the RGB565 base64
payload, writes a PNG. Optionally switches to a named scene first via
`scene <id>`.
"""

from __future__ import annotations

import base64
import io
import struct
import subprocess
import sys
import time
from pathlib import Path

PORT = "COM9"
HARNESS = r"D:\Code\esp-harness\tools\esp-harness\.venv\Scripts\esp-harness.exe"


def console(cmd: str, *extra: str, timeout: int = 10) -> str:
    proc = subprocess.run(
        [HARNESS, "console", "--port", PORT, "--cmd", cmd, "--raw",
         "--timeout", str(timeout), *extra],
        capture_output=True, timeout=timeout + 5,
    )
    return proc.stdout.decode("utf-8", errors="replace")


def dump_screen(w: int = 466) -> bytes:
    out = console(f"?dump w={w}", "--payload", "DUMP", timeout=20)
    lines = out.splitlines()
    begin = end = None
    for i, ln in enumerate(lines):
        if ln.startswith("DUMP_BEGIN"):
            begin = i
        elif ln.startswith("DUMP_END"):
            end = i
            break
    if begin is None or end is None:
        raise RuntimeError(f"no DUMP block in output:\n{out}")
    b64 = "".join(lines[begin + 1:end]).strip()
    return base64.b64decode(b64)


def rgb565_to_png(data: bytes, w: int, h: int, out_path: Path) -> None:
    """Naive PNG writer that avoids PIL dep. Falls back to using PIL if installed."""
    try:
        from PIL import Image
        img = Image.new("RGB", (w, h))
        pixels = img.load()
        for y in range(h):
            for x in range(w):
                idx = (y * w + x) * 2
                lo = data[idx]
                hi = data[idx + 1]
                v = lo | (hi << 8)
                r = ((v >> 11) & 0x1F) << 3
                g = ((v >> 5) & 0x3F) << 2
                b = (v & 0x1F) << 3
                pixels[x, y] = (r, g, b)
        img.save(out_path)
    except ImportError:
        sys.stderr.write("PIL not available; writing raw bytes\n")
        out_path.with_suffix(".bin").write_bytes(data)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    out_path = Path(sys.argv[1])
    scene_id = sys.argv[2] if len(sys.argv) > 2 else None
    if scene_id:
        console(f"scene {scene_id}", timeout=5)
        time.sleep(0.8)
    w = 466
    data = dump_screen(w=w)
    expected = w * w * 2
    if len(data) != expected:
        # Fall back to 128 if 466 didn't work — the firmware defaults to 128
        print(f"got {len(data)} bytes, expected {expected}; trying 128", file=sys.stderr)
        data = dump_screen(w=128)
        w = 128
    rgb565_to_png(data, w, w, out_path)
    print(f"wrote {out_path} ({w}x{w})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
