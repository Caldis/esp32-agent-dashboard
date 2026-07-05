"""AWAITING state variants by input-type — design for v2.3.0.

The AWAITING takeover is a TEMPLATE; the headline + glyph + subhead
varies by what kind of input the agent needs. Five canonical types:

  1. continue     — Stop event. Default "your turn" rally continuation.
  2. approve      — PreToolUse(permission). Yes/no decision required.
  3. pick         — Agent presented options (numbered list in last msg).
  4. type         — Agent asked open-ended question (last msg ends with ?).
  5. clarify      — Agent flagged ambiguity / asked to clarify intent.

Same skeleton every time:
   [glyph]
   <HEADLINE>            52pt bold       <-- changes by type
   cc · a3                28pt teal       <-- agent (constant shape)
   <context line(s)>     22pt dim         <-- type-specific
   waiting Xs · +N       16pt mute        <-- footer
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT_DIR = Path("docs/img")
W = H = 466

# noir palette
BG          = (11, 10, 9)
SURFACE     = (28, 24, 20)
TEXT        = (243, 238, 226)
TEXT_DIM    = (138, 128, 122)
INK_MUTE    = (90, 81, 74)
TEAL        = (43, 179, 177)
TEAL_DIM    = (14, 124, 123)
RUST        = (184, 67, 26)
MOSS_BRIGHT = (88, 138, 92)
GOLD        = (184, 144, 32)


def font(size, bold=False):
    candidates = (
        "C:/Windows/Fonts/seguisb.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    )
    for p in candidates:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()


def draw_round_clip(img):
    mask = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse((0, 0, W - 1, H - 1), fill=255)
    bg_img = Image.new("RGB", (W, H), BG)
    bg_img.paste(img, mask=mask)
    return bg_img


# ── glyph renderers (pre-LVGL — these become LV_SYMBOL_* in C code) ──
def glyph_dot(d, cx, cy, color):
    """continue — soft breathing dot + ring"""
    d.ellipse((cx - 36, cy - 36, cx + 36, cy + 36), outline=color, width=2)
    d.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=color)


def glyph_lock(d, cx, cy, color):
    """approve — lock shape (LV_SYMBOL_KEYBOARD-ish vector)"""
    # Body
    d.rounded_rectangle((cx - 24, cy - 8, cx + 24, cy + 30), radius=4, outline=color, width=3)
    # Shackle (arc)
    d.arc((cx - 18, cy - 32, cx + 18, cy + 4), start=180, end=360, fill=color, width=3)
    # Keyhole
    d.ellipse((cx - 4, cy + 4, cx + 4, cy + 12), fill=color)


def glyph_list(d, cx, cy, color, n=3):
    """pick — numbered list"""
    for i in range(n):
        y = cy - 22 + i * 22
        d.ellipse((cx - 30, y - 4, cx - 22, y + 4), fill=color)
        d.line((cx - 14, y, cx + 28, y), fill=color, width=3)


def glyph_pencil(d, cx, cy, color):
    """type — pencil"""
    # Pencil at 30deg
    points = [(cx - 22, cy + 22), (cx - 14, cy + 28), (cx + 22, cy - 14), (cx + 14, cy - 22)]
    d.polygon(points, outline=color, width=3)
    # Tip dot
    d.ellipse((cx + 14, cy - 22, cx + 22, cy - 14), fill=color)
    # Eraser
    d.line((cx - 22, cy + 22, cx - 28, cy + 28), fill=color, width=3)


def glyph_qmark(d, cx, cy, color):
    """clarify — question mark inside circle"""
    d.ellipse((cx - 30, cy - 30, cx + 30, cy + 30), outline=color, width=3)
    d.text((cx, cy), "?", fill=color, font=font(38, bold=True), anchor="mm")


def render_awaiting(kind, headline, agent_chip, ctx_lines, duration_s, more_waiting=0, accent=TEAL):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    # Tiny eyebrow
    d.text((W // 2, 48), "14:32 · Clawd", fill=INK_MUTE, font=font(13), anchor="mm")
    # Glyph
    cy_g = 110
    {
        "continue": glyph_dot,
        "approve":  glyph_lock,
        "pick":     lambda d, cx, cy, c: glyph_list(d, cx, cy, c, n=3),
        "type":     glyph_pencil,
        "clarify":  glyph_qmark,
    }[kind](d, W // 2, cy_g, accent)
    # Headline — biggest, calm
    d.text((W // 2, 200), headline, fill=TEXT, font=font(52, bold=True), anchor="mm")
    # Agent
    d.text((W // 2, 252), agent_chip, fill=accent, font=font(28, bold=True), anchor="mm")
    # Context (1-3 lines)
    cy_ctx = 296
    for line in ctx_lines[:3]:
        d.text((W // 2, cy_ctx), line, fill=TEXT_DIM, font=font(22), anchor="mm")
        cy_ctx += 30
    # Duration + overflow
    foot = f"waiting {duration_s}s"
    if more_waiting:
        foot += f"   ·   +{more_waiting} more"
    d.text((W // 2, H - 50), foot, fill=INK_MUTE, font=font(15), anchor="mm")
    return draw_round_clip(img)


# ── 5 canonical AWAITING variants ────────────────────────────────────


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    variants = {
        "awaiting-continue": dict(
            kind="continue",
            headline="your turn",
            agent_chip="cc · a3",
            ctx_lines=["finished refactor", "of src/auth.py"],
            duration_s=38, more_waiting=2, accent=TEAL,
        ),
        "awaiting-approve": dict(
            kind="approve",
            headline="approve?",
            agent_chip="cc · a3",
            ctx_lines=["Bash:", "git push --force origin master"],
            duration_s=12, more_waiting=0, accent=GOLD,
        ),
        "awaiting-pick": dict(
            kind="pick",
            headline="pick one",
            agent_chip="cx · b1",
            ctx_lines=["migrate strategy:", "inline · defer · abort"],
            duration_s=25, more_waiting=1, accent=TEAL,
        ),
        "awaiting-type": dict(
            kind="type",
            headline="type a reply",
            agent_chip="cc · b2",
            ctx_lines=["what should the new", "branch be called?"],
            duration_s=8, more_waiting=0, accent=TEAL,
        ),
        "awaiting-clarify": dict(
            kind="clarify",
            headline="clarify",
            agent_chip="cx · c1",
            ctx_lines=["did you mean v2 or v3", "of the protocol?"],
            duration_s=4, more_waiting=0, accent=GOLD,
        ),
    }

    images = []
    for name, kwargs in variants.items():
        img = render_awaiting(**kwargs)
        path = OUT_DIR / f"mock-{name}.png"
        img.save(path)
        print(f"wrote {path}")
        images.append(img)

    # Strip image: 5 panels side by side, 30px gutter
    GUTTER = 30
    strip = Image.new("RGB", (W * 5 + GUTTER * 4, H), BG)
    for i, im in enumerate(images):
        strip.paste(im, (i * (W + GUTTER), 0))
    strip_path = OUT_DIR / "mock-awaiting-strip.png"
    strip.save(strip_path)
    print(f"wrote {strip_path}")


if __name__ == "__main__":
    main()
