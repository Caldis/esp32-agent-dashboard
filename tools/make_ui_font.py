#!/usr/bin/env python3
"""Subset Consolas (regular + bold) to ASCII + UI symbols for the device UI.

v4.2 replaces the built-in Montserrat bitmap fonts with runtime-rendered
Consolas (the terminal look, matching the agent-console subject matter).
Latin/symbols render Consolas; CJK falls through to the SimHei subset via
the lv_font_t fallback chain wired in cjk_font.c's ui_font()/ui_font_bold().

Usage:
    python tools/make_ui_font.py

Sources are the Windows system Consolas (consola.ttf / consolab.ttf).
Outputs fonts/ui.ttf + fonts/ui_bold.ttf, copied to main/ — gitignored
like zh.ttf (derived from proprietary system fonts; regenerate locally).
Requires: pip install fonttools
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "fonts")
os.makedirs(FONTS, exist_ok=True)

# ASCII + the separators/symbols the scenes compose ("·" activity dots,
# "×°—…" occasionally from hosts).
CHARS = "".join(chr(c) for c in range(0x20, 0x7F)) + "·×°—…"

JOBS = [
    (r"C:/Windows/Fonts/consola.ttf",  "ui.ttf"),
    (r"C:/Windows/Fonts/consolab.ttf", "ui_bold.ttf"),
]

for src, name in JOBS:
    out = os.path.join(FONTS, name)
    subprocess.run([
        sys.executable, "-m", "fontTools.subset", src,
        f"--text={CHARS}", f"--output-file={out}",
        "--no-hinting", "--desubroutinize", "--layout-features=",
        "--drop-tables+=GSUB,GPOS",
    ], check=True)
    shutil.copy(out, os.path.join(ROOT, "main", name))
    print(f"subset -> main/{name} ({os.path.getsize(out)} bytes)")
