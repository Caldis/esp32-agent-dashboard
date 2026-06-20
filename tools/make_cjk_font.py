#!/usr/bin/env python3
"""Subset a CJK font to GB2312 + ASCII for on-device rendering via lv_tiny_ttf.

The full SimHei is ~9.7MB; this trims it to the ~6800 common simplified-Chinese
chars (GB2312) + ASCII + punctuation → ~2MB, small enough to embed in the app
(see main/CMakeLists.txt EMBED_FILES "zh.ttf"). tiny_ttf rasterizes glyphs at
runtime, so any char in the subset renders at any size.

Usage:
    python tools/make_cjk_font.py [path/to/source.ttf]

Default source is a Windows SimHei. Output fonts/zh.ttf is copied to main/zh.ttf
and is gitignored (derived from a system font — don't commit the binary).
Requires: pip install fonttools
"""
import os
import shutil
import subprocess
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else r"C:/Windows/Fonts/simhei.ttf"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "fonts")
os.makedirs(FONTS, exist_ok=True)

chars = set(chr(c) for c in range(0x20, 0x7F))                 # ASCII
for c in "　、。·ˉ…—～‖''""〔〕〈〉《》「」『』【】（）！？：；，．±×÷°":
    chars.add(c)
for b1 in range(0xB0, 0xF8):                                  # GB2312 hanzi rows
    for b2 in range(0xA1, 0xFF):
        try:
            chars.add(bytes([b1, b2]).decode("gb2312"))
        except UnicodeDecodeError:
            pass

charfile = os.path.join(FONTS, "_gb2312.txt")
open(charfile, "w", encoding="utf-8").write("".join(sorted(chars)))
out = os.path.join(FONTS, "zh.ttf")

subprocess.run([
    sys.executable, "-m", "fontTools.subset", SRC,
    f"--text-file={charfile}", f"--output-file={out}",
    "--no-hinting", "--desubroutinize", "--layout-features=",
    "--drop-tables+=GSUB,GPOS",
], check=True)

shutil.copy(out, os.path.join(ROOT, "main", "zh.ttf"))
print(f"subset {len(chars)} chars → main/zh.ttf ({os.path.getsize(out)} bytes)")
