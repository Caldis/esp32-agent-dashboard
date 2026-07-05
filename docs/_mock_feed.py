"""Mockup renderer for the new feed-style dashboard (v2.3.0 proposal).

Produces two PNGs at 466x466 (the device's true panel size):
  - docs/img/mock-feed-permanent.png  — the default permanent view
  - docs/img/mock-feed-notification.png — same scene with a notification
                                          banner slid in on top

Colors mirror docs/brand/palette.md noir theme exactly. Typography
sizes are picked so that the feed body reads at 50cm viewing distance.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT_DIR = Path("docs/img")
W = H = 466

# noir theme tokens (mirror docs/brand/palette.md exactly)
BG          = (11, 10, 9)         # #0B0A09
SURFACE     = (28, 24, 20)        # #1C1814  card panel
TEXT        = (243, 238, 226)     # #F3EEE2  paper
TEXT_DIM    = (138, 128, 122)     # #8A807A  ink-fade
INK_MUTE    = (90, 81, 74)        # #5A514A
TEAL        = (43, 179, 177)      # #2BB3B1  teal-bright (noir accent)
TEAL_DIM    = (14, 124, 123)      # #0E7C7B
RUST        = (184, 67, 26)       # #B8431A  rust (single small accent)
MOSS        = (52, 74, 54)        # #344A36  ok
MOSS_BRIGHT = (88, 138, 92)
GOLD        = (184, 144, 32)      # #B89020  warning


def font(size, bold=False):
    # Try common Windows fonts; fall back to default
    candidates = (
        ("C:/Windows/Fonts/seguisb.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf"),
        ("C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf"),
        "C:/Windows/Fonts/arial.ttf",
    )
    for p in candidates:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()


FEED = [
    ("14:31", "cc",  "a3", "ok",  "Edit",   "src/auth.py +8 -2"),
    ("14:30", "cx",  "b1", "ok",  "Grep",   "login (42 hits)"),
    ("14:28", "cc",  "b2", "run", "Read",   "components/api.ts"),
    ("14:25", "cc",  "a3", "ok",  "Bash",   "git push origin master"),
    ("14:23", "cx",  "c1", "ok",  "Read",   "README.md (162 lines)"),
    ("14:20", "cc",  "a3", "ok",  "Write",  "CHANGELOG.md"),
]


def draw_round_clip(img):
    """Mask corners with bg colour to simulate the round AMOLED panel."""
    mask = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse((0, 0, W - 1, H - 1), fill=255)
    bg_img = Image.new("RGB", (W, H), BG)
    bg_img.paste(img, mask=mask)
    return bg_img


def draw_header(d, now="14:32"):
    # Time, prominent but not heavy
    d.text((W // 2, 38), now, fill=TEXT, font=font(36), anchor="mm")
    # device name, dim, tiny
    d.text((W // 2, 64), "Clawd", fill=TEXT_DIM, font=font(13), anchor="mm")


def draw_feed_row(d, y, t, kind, sid, status, verb, target):
    # Subtle divider above row
    d.line((48, y - 16, W - 48, y - 16), fill=(28, 24, 20), width=1)
    # Time (mono-ish font, dim)
    d.text((52, y), t, fill=TEXT_DIM, font=font(16), anchor="lm")
    # status glyph
    if status == "ok":
        d.text((110, y), "✓", fill=MOSS_BRIGHT, font=font(20), anchor="mm")
    elif status == "run":
        d.text((110, y), "·", fill=TEAL, font=font(28), anchor="mm")
    else:
        d.text((110, y), "✗", fill=GOLD, font=font(20), anchor="mm")
    # kind tag (small) — kind+sid as one quiet chip
    chip = f"{kind}·{sid}"
    chip_color = INK_MUTE if kind == "cc" else INK_MUTE  # same hue — kind is just a label
    d.text((130, y), chip, fill=chip_color, font=font(14), anchor="lm")
    # Verb (action) — the focus
    d.text((200, y), verb, fill=TEXT, font=font(20, bold=True), anchor="lm")
    # Target (truncated)
    if d.textlength(target, font=font(16)) > W - 280:
        target = target[:18] + "…"
    d.text((262, y), target, fill=TEXT_DIM, font=font(16), anchor="lm")


def draw_footer(d):
    # Bottom summary
    d.line((48, H - 70, W - 48, H - 70), fill=(28, 24, 20), width=1)
    # 3 active
    d.text((90, H - 50), "3", fill=TEAL, font=font(22, bold=True), anchor="mm")
    d.text((90, H - 28), "active", fill=TEXT_DIM, font=font(12), anchor="mm")
    # 84.5k tokens
    d.text((W // 2, H - 50), "84.5k", fill=TEXT, font=font(22, bold=True), anchor="mm")
    d.text((W // 2, H - 28), "tokens today", fill=TEXT_DIM, font=font(12), anchor="mm")
    # v0.5.0
    d.text((W - 90, H - 50), "v0.5", fill=TEXT_DIM, font=font(18), anchor="mm")
    d.text((W - 90, H - 28), "Clawd", fill=TEXT_DIM, font=font(12), anchor="mm")


def render_permanent():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    draw_header(d)
    y = 110
    for t, kind, sid, status, verb, target in FEED:
        draw_feed_row(d, y, t, kind, sid, status, verb, target)
        y += 38
    draw_footer(d)
    return draw_round_clip(img)


def draw_notification_banner(d, kind="ok", title="cc·a3 completed", body="Edit src/auth.py", sub="+8 -2 lines"):
    # Banner slid down from top — sits at y=20..160
    x0, x1 = 32, W - 32
    y0, y1 = 78, 178
    # Subtle frosted-glass-ish surface
    d.rounded_rectangle((x0, y0, x1, y1), radius=18, fill=SURFACE)
    # Left accent stripe in teal (or rust for warning, etc.)
    accent_color = MOSS_BRIGHT if kind == "ok" else (GOLD if kind == "warn" else RUST)
    d.rounded_rectangle((x0, y0, x0 + 6, y1), radius=3, fill=accent_color)
    # Glyph
    glyph = {"ok": "✓", "warn": "ⓘ", "err": "✗"}.get(kind, "·")
    d.text((x0 + 30, (y0 + y1) // 2), glyph, fill=accent_color, font=font(28), anchor="lm")
    # Title (eyebrow)
    d.text((x0 + 70, y0 + 28), title, fill=TEXT_DIM, font=font(14), anchor="lm")
    # Body — the main message
    d.text((x0 + 70, y0 + 52), body, fill=TEXT, font=font(22, bold=True), anchor="lm")
    # Sub
    d.text((x0 + 70, y0 + 80), sub, fill=TEXT_DIM, font=font(15), anchor="lm")


def render_notification():
    # Same permanent view but dimmed, with banner overlay
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    draw_header(d)
    # Feed rows — dimmed (lower alpha to ~55%)
    y = 110
    for t, kind, sid, status, verb, target in FEED:
        # Manually dim by mixing with BG
        def dim(c, amt=0.55):
            return tuple(int(c[i] * amt + BG[i] * (1 - amt)) for i in range(3))
        # Redraw with dimmed colors
        d.line((48, y - 16, W - 48, y - 16), fill=(20, 18, 15), width=1)
        d.text((52, y), t, fill=dim(TEXT_DIM, 0.6), font=font(16), anchor="lm")
        glyph_col = dim(MOSS_BRIGHT if status == "ok" else (TEAL if status == "run" else GOLD), 0.6)
        d.text((110, y), "✓" if status == "ok" else ("·" if status == "run" else "✗"),
               fill=glyph_col, font=font(20 if status != "run" else 28), anchor="mm")
        d.text((130, y), f"{kind}·{sid}", fill=dim(INK_MUTE, 0.6), font=font(14), anchor="lm")
        d.text((200, y), verb, fill=dim(TEXT, 0.55), font=font(20, bold=True), anchor="lm")
        tt = target[:18] + "…" if d.textlength(target, font=font(16)) > W - 280 else target
        d.text((262, y), tt, fill=dim(TEXT_DIM, 0.55), font=font(16), anchor="lm")
        y += 38
    draw_footer(d)
    # Notification banner on top
    draw_notification_banner(d, kind="ok",
                              title="cc·a3 completed",
                              body="Edit src/auth.py",
                              sub="+8 -2 lines · 280ms")
    return draw_round_clip(img)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    perm = render_permanent()
    notif = render_notification()
    perm.save(OUT_DIR / "mock-feed-permanent.png")
    notif.save(OUT_DIR / "mock-feed-notification.png")
    print(f"wrote {OUT_DIR / 'mock-feed-permanent.png'}")
    print(f"wrote {OUT_DIR / 'mock-feed-notification.png'}")


if __name__ == "__main__":
    main()
