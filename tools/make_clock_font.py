#!/usr/bin/env python3
"""Subset a rounded display font to clock glyphs (0-9 : -) for scene_clock.

SimHei digits (main/zh.ttf) are too thin and square for the StandBy-style
face, and its colon carries wide side bearings that read as a gap. This
builds a tiny (~5KB) subset of M PLUS Rounded 1c Black — the usual open
substitute for Apple's SF Rounded — containing exactly the twelve glyphs
status_bar_format_time() can emit ("HH:MM" digits, ':', and '-' for the
no-host "--:--" state).

Usage:
    python tools/make_clock_font.py [path/to/source.ttf]

Default source is fonts/MPLUSRounded1c-Black.ttf (OFL-licensed; fetch with
  curl -L -o fonts/MPLUSRounded1c-Black.ttf \
    https://raw.githubusercontent.com/google/fonts/main/ofl/mplusrounded1c/MPLUSRounded1c-Black.ttf
). Output fonts/clock_digits.ttf is copied to main/clock_digits.ttf and IS
committed (unlike zh.ttf: the source font is OFL, not a proprietary system
font). Requires: pip install fonttools
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "fonts")
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    FONTS, "MPLUSRounded1c-Black.ttf")

CHARS = "0123456789:-"

# M PLUS Rounded's colon ships with ~123/1000em side bearings — at 150px
# that reads as an awkward ~18px hole on each side of the ':'. Shave this
# many units off EACH side (advance -2x, outline shifted left) for the
# tight StandBy look. Picked from a host-side render comparison: -60 is
# safe, -90 matches iOS, -120 crowds the digits.
COLON_SHAVE = 90

out = os.path.join(FONTS, "clock_digits.ttf")
subprocess.run([
    sys.executable, "-m", "fontTools.subset", SRC,
    f"--text={CHARS}", f"--output-file={out}",
    "--no-hinting", "--desubroutinize", "--layout-features=",
    "--drop-tables+=GSUB,GPOS",
], check=True)

from fontTools.ttLib import TTFont
f = TTFont(out)
cmap = f.getBestCmap()
g = cmap[ord(":")]
adv, lsb = f["hmtx"][g]
glyph = f["glyf"][g]

# The colon ships bottom-heavy (dots hug the baseline, bbox 0..580 vs the
# digits' -10..740): centred against "23:16" it reads as sunk. Raise it so
# its bbox midpoint matches the '0' glyph's.
zero = f["glyf"][cmap[ord("0")]]
raise_y = (zero.yMin + zero.yMax) // 2 - (glyph.yMin + glyph.yMax) // 2

coords = glyph.coordinates
for i in range(len(coords)):
    x, y = coords[i]
    coords[i] = (x - COLON_SHAVE, y + raise_y)
glyph.xMin -= COLON_SHAVE
glyph.xMax -= COLON_SHAVE
glyph.yMin += raise_y
glyph.yMax += raise_y
f["hmtx"][g] = (adv - 2 * COLON_SHAVE, lsb - COLON_SHAVE)
f.save(out)
print(f"colon advance {adv} -> {adv - 2 * COLON_SHAVE}, raised {raise_y}")

shutil.copy(out, os.path.join(ROOT, "main", "clock_digits.ttf"))
print(f"subset '{CHARS}' -> main/clock_digits.ttf ({os.path.getsize(out)} bytes)")
