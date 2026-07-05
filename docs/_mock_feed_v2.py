"""Mockup v2 — bigger text + AWAITING takeover state (v2.3.0 proposal).

Three UI states this device cycles through:

  1. AMBIENT     — default feed of completions; one event ~12% of screen height
  2. PUSH        — banner overlay 3-5s when an event just landed
  3. AWAITING    — full-screen takeover when one or more agents wait on user
                   (Anthropic's "Agent View" pattern, 2026-05)

Typography scaled per Apple Watch HIG:
  - Display text   ≥ 20pt  → I'm at 32-56pt for primary
  - Body text      ≥ 14pt  → I'm at 22-26pt
  - All sizes assume 50cm viewing distance + round AMOLED clipping
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT_DIR = Path("docs/img")
W = H = 466

# noir palette (docs/brand/palette.md)
BG          = (11, 10, 9)         # #0B0A09
SURFACE     = (28, 24, 20)        # #1C1814
TEXT        = (243, 238, 226)     # #F3EEE2
TEXT_DIM    = (138, 128, 122)     # #8A807A
INK_MUTE    = (90, 81, 74)        # #5A514A
TEAL        = (43, 179, 177)      # #2BB3B1
TEAL_DIM    = (14, 124, 123)
RUST        = (184, 67, 26)       # #B8431A
MOSS        = (52, 74, 54)
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


# ── feed data (used by ambient + push) ───────────────────────────────
FEED = [
    ("14:31", "cc",  "a3", "ok",  "Edit",   "src/auth.py"),
    ("14:30", "cx",  "b1", "ok",  "Grep",   "login (42)"),
    ("14:28", "cc",  "b2", "run", "Read",   "api.ts"),
    ("14:25", "cc",  "a3", "ok",  "Bash",   "git push"),
]


def draw_header(d, now="14:32"):
    # Bigger time — 56pt, centred in top arc of the round panel
    d.text((W // 2, 52), now, fill=TEXT, font=font(48, bold=True), anchor="mm")
    d.text((W // 2, 86), "Clawd", fill=TEXT_DIM, font=font(15), anchor="mm")


def draw_feed_row(d, y, t, kind, sid, status, verb, target, alpha=1.0):
    def mix(c):
        return tuple(int(c[i] * alpha + BG[i] * (1 - alpha)) for i in range(3))
    d.line((40, y - 28, W - 40, y - 28), fill=mix((24, 22, 18)), width=1)
    d.text((46, y), t, fill=mix(TEXT_DIM), font=font(20), anchor="lm")
    if status == "ok":
        d.text((120, y), "ok", fill=mix(MOSS_BRIGHT), font=font(20, bold=True), anchor="mm")
    elif status == "run":
        d.text((120, y), "...", fill=mix(TEAL), font=font(20, bold=True), anchor="mm")
    else:
        d.text((120, y), "x", fill=mix(GOLD), font=font(22, bold=True), anchor="mm")
    d.text((164, y), f"{kind} {sid}", fill=mix(INK_MUTE), font=font(16), anchor="lm")
    d.text((240, y), verb, fill=mix(TEXT), font=font(28, bold=True), anchor="lm")
    target_max = W - 350
    if d.textlength(target, font=font(22)) > target_max:
        while d.textlength(target + "…", font=font(22)) > target_max and len(target) > 4:
            target = target[:-1]
        target = target + "…"
    d.text((342, y), target, fill=mix(TEXT_DIM), font=font(22), anchor="lm")


def draw_footer(d, alpha=1.0):
    def mix(c):
        return tuple(int(c[i] * alpha + BG[i] * (1 - alpha)) for i in range(3))
    d.line((40, H - 78, W - 40, H - 78), fill=mix((24, 22, 18)), width=1)
    d.text((90, H - 50), "3", fill=mix(TEAL), font=font(28, bold=True), anchor="mm")
    d.text((90, H - 24), "active", fill=mix(TEXT_DIM), font=font(13), anchor="mm")
    d.text((W // 2, H - 50), "84.5k", fill=mix(TEXT), font=font(28, bold=True), anchor="mm")
    d.text((W // 2, H - 24), "tokens today", fill=mix(TEXT_DIM), font=font(13), anchor="mm")
    d.text((W - 90, H - 50), "v2.3", fill=mix(TEXT_DIM), font=font(22), anchor="mm")
    d.text((W - 90, H - 24), "Clawd", fill=mix(TEXT_DIM), font=font(13), anchor="mm")


# ── State 1: AMBIENT ─────────────────────────────────────────────────
def render_ambient():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    draw_header(d)
    y = 152
    for t, kind, sid, status, verb, target in FEED:
        draw_feed_row(d, y, t, kind, sid, status, verb, target)
        y += 54
    draw_footer(d)
    return draw_round_clip(img)


# ── State 2: PUSH (notification banner) ─────────────────────────────
def render_push():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    # Dim header + feed
    draw_header(d)
    y = 152
    for t, kind, sid, status, verb, target in FEED:
        draw_feed_row(d, y, t, kind, sid, status, verb, target, alpha=0.45)
        y += 54
    draw_footer(d, alpha=0.45)
    # Banner — bigger, dwell 3s
    x0, x1 = 28, W - 28
    y0, y1 = 110, 250
    d.rounded_rectangle((x0, y0, x1, y1), radius=22, fill=SURFACE)
    d.rounded_rectangle((x0, y0, x0 + 6, y1), radius=3, fill=MOSS_BRIGHT)
    d.text((x0 + 28, y0 + 38), "completed", fill=TEXT_DIM, font=font(15), anchor="lm")
    d.text((x0 + 28, y0 + 78), "Edit src/auth.py", fill=TEXT, font=font(32, bold=True), anchor="lm")
    d.text((x0 + 28, y0 + 112), "cc · a3   +8 -2   280ms", fill=TEXT_DIM, font=font(18), anchor="lm")
    return draw_round_clip(img)


# ── State 3: AWAITING (takeover) — the most important state ─────────
def render_awaiting():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    # Tiny eyebrow at top
    d.text((W // 2, 60), "14:32 · Clawd", fill=TEXT_DIM, font=font(15), anchor="mm")
    # Status badge with gentle pulse-ring look
    d.ellipse((W // 2 - 36, 96, W // 2 + 36, 168), outline=TEAL, width=2)
    d.ellipse((W // 2 - 8, 124, W // 2 + 8, 140), fill=TEAL)
    # Headline — 52pt
    d.text((W // 2, 220), "your turn", fill=TEXT, font=font(52, bold=True), anchor="mm")
    # Agent (subhead) — 30pt
    d.text((W // 2, 270), "cc · a3", fill=TEAL, font=font(28, bold=True), anchor="mm")
    # Reason — calm, what just happened
    d.text((W // 2, 312), "finished refactor", fill=TEXT_DIM, font=font(22), anchor="mm")
    d.text((W // 2, 340), "of src/auth.py", fill=TEXT_DIM, font=font(22), anchor="mm")
    # Duration footer
    d.text((W // 2, 400), "waiting 38s", fill=INK_MUTE, font=font(16), anchor="mm")
    # +N more pip
    d.ellipse((W // 2 - 78, 422, W // 2 - 70, 430), fill=TEXT_DIM)
    d.text((W // 2 - 56, 426), "+2 more waiting", fill=TEXT_DIM, font=font(13), anchor="lm")
    return draw_round_clip(img)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    a = render_ambient()
    p = render_push()
    aw = render_awaiting()
    a.save(OUT_DIR / "mock-ambient.png")
    p.save(OUT_DIR / "mock-push.png")
    aw.save(OUT_DIR / "mock-awaiting.png")
    print("wrote", OUT_DIR / "mock-ambient.png")
    print("wrote", OUT_DIR / "mock-push.png")
    print("wrote", OUT_DIR / "mock-awaiting.png")


if __name__ == "__main__":
    main()
