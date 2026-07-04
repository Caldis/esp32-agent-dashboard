"""Fleet view mock — multi-agent dashboard redesign (v3.0 proposal).

Adaptive density on the 466x466 round AMOLED:

  1 agent   → ambient pulse (kept) + project + live activity line (new)
  2-4 agents → per-agent rows: status dot / project / activity / meta
               waiting rows carry the gold accent; running rows teal.

Rules encoded here (mirrored in firmware scene_dashboard):
  - rows live between the 48pt clock (bottom ~110) and the footer (~408)
  - row width 344 centered — safe against the round clip down to y≈390
  - row height adapts: 2 agents → 120, 3 → 86, 4 → 64
  - takeover (scene_awaiting) only fires when slot_count == 1; with 2+
    agents the fleet stays up and awaiting rows glow gold instead.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT_DIR = Path("docs/img")
W = H = 466

# noir palette (docs/brand/palette.md)
BG          = (11, 10, 9)         # #0B0A09
SURFACE     = (28, 24, 20)        # #1C1814
SURFACE_HI  = (38, 32, 26)        # waiting row surface
TEXT        = (243, 238, 226)     # #F3EEE2
TEXT_DIM    = (138, 128, 122)     # #8A807A
INK_MUTE    = (90, 81, 74)        # #5A514A
TEAL        = (43, 179, 177)      # #2BB3B1
TEAL_DIM    = (14, 124, 123)
GOLD        = (184, 144, 32)      # #B89020
GOLD_BRIGHT = (224, 180, 60)


def font(size, bold=False, cjk=False):
    candidates = (
        ("C:/Windows/Fonts/simhei.ttf",) if cjk else ()
    ) + (
        "C:/Windows/Fonts/seguisb.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
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


def draw_header(d, now="14:32"):
    d.text((W // 2, 76), now, fill=TEXT, font=font(48, bold=True), anchor="mm")


def draw_footer(d, active="3", tokens="48k"):
    d.text((124, 422), active, fill=TEAL, font=font(28, bold=True), anchor="lm")
    d.text((124, 448), "active", fill=TEXT_DIM, font=font(12), anchor="lm")
    d.text((284, 422), tokens, fill=TEXT, font=font(28, bold=True), anchor="lm")
    d.text((284, 448), "tokens today", fill=TEXT_DIM, font=font(12), anchor="lm")


# ── agent row ─────────────────────────────────────────────────────────
# state: "running" | "waiting" | "urgent" (approve/clarify)

ROW_W = 380
ROW_X = (W - ROW_W) // 2


def draw_row(d, y, h, *, state, kind, project, activity, meta, cjk=False):
    surface = SURFACE_HI if state != "running" else SURFACE
    accent = TEAL if state == "running" else (GOLD_BRIGHT if state == "urgent" else GOLD)
    d.rounded_rectangle((ROW_X, y, ROW_X + ROW_W, y + h), radius=14, fill=surface)
    if state != "running":
        d.rounded_rectangle((ROW_X, y, ROW_X + ROW_W, y + h), radius=14,
                            outline=accent, width=1)

    compact = h < 80
    pad = 16 if compact else 20
    line1_y = y + (h * 30 // 100 if compact else h * 28 // 100)
    line2_y = y + (h * 70 // 100 if compact else h * 66 // 100)

    # status dot
    r = 5 if compact else 7
    cx = ROW_X + pad + r
    d.ellipse((cx - r, line1_y - r, cx + r, line1_y + r), fill=accent)

    # kind chip + project name
    name_f = font(20 if compact else 24, bold=True, cjk=cjk)
    kind_f = font(13 if compact else 15)
    tx = cx + r + 10
    d.text((tx, line1_y), kind, fill=INK_MUTE, font=kind_f, anchor="lm")
    kw = d.textlength(kind, font=kind_f)

    # right meta (waiting duration gold / tokens dim)
    meta_col = accent if state != "running" else TEXT_DIM
    meta_f = font(15 if compact else 17)
    d.text((ROW_X + ROW_W - pad, line1_y), meta,
           fill=meta_col, font=meta_f, anchor="rm")
    meta_w = d.textlength(meta, font=meta_f)

    # project name — truncate so it never collides with the right meta
    name_x = tx + kw + 8
    name_max = (ROW_X + ROW_W - pad) - meta_w - 14 - name_x
    name = project
    while d.textlength(name, font=name_f) > name_max and len(name) > 1:
        name = name[:-1]
    if name != project:
        name = name[:-1] + "…"
    d.text((name_x, line1_y), name, fill=TEXT, font=name_f, anchor="lm")

    # activity line
    act_f = font(16 if compact else 19, cjk=True)
    act_col = accent if state != "running" else TEXT_DIM
    act = activity
    max_w = ROW_W - pad * 2 - 8
    while d.textlength(act, font=act_f) > max_w and len(act) > 1:
        act = act[:-1]
    if act != activity:
        act = act[:-1] + "…"
    d.text((ROW_X + pad + 4, line2_y), act, fill=act_col, font=act_f, anchor="lm")


ROW_AREA_TOP = 128
ROW_AREA_BOT = 396


def draw_fleet(agents, now, active, tokens, fname):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    draw_header(d, now)
    n = len(agents)
    gap = 10
    h = min(104, (ROW_AREA_BOT - ROW_AREA_TOP - (n - 1) * gap) // n)
    total = n * h + (n - 1) * gap
    y = ROW_AREA_TOP + (ROW_AREA_BOT - ROW_AREA_TOP - total) // 2
    for a in agents:
        draw_row(d, y, h, **a)
        y += h + gap
    draw_footer(d, active, tokens)
    out = OUT_DIR / fname
    draw_round_clip(img).save(out)
    print("wrote", out)


def draw_single(fname):
    """1 agent — ambient pulse kept, enriched with project + activity."""
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    draw_header(d, "14:32")
    # ring + dot
    cx, cy = W // 2, 210
    d.ellipse((cx - 48, cy - 48, cx + 48, cy + 48), outline=(43, 179, 177, 100), width=2)
    d.ellipse((cx - 13, cy - 13, cx + 13, cy + 13), fill=TEAL)
    d.text((cx, 300), "thinking", fill=TEXT, font=font(28, bold=True), anchor="mm")
    d.text((cx, 336), "cc  esp32-agent-dashboard", fill=TEXT_DIM, font=font(17), anchor="mm")
    d.text((cx, 366), "$ idf.py build", fill=INK_MUTE, font=font(16, cjk=True), anchor="mm")
    draw_footer(d, "1", "12k")
    out = OUT_DIR / fname
    draw_round_clip(img).save(out)
    print("wrote", out)


if __name__ == "__main__":
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    draw_single("mock-fleet-1.png")

    draw_fleet([
        dict(state="running", kind="cc", project="esp32-agent-dashboard",
             activity="$ idf.py build", meta="12k"),
        dict(state="urgent", kind="cc", project="united-memory",
             activity="approve?  rm -rf build/", meta="3m"),
    ], "14:32", "2", "31k", "mock-fleet-2.png")

    draw_fleet([
        dict(state="running", kind="cc", project="esp32-agent-dashboard",
             activity="$ idf.py build", meta="12k"),
        dict(state="waiting", kind="cx", project="ballru",
             activity="your turn — 关卡数值已调平", meta="6m", cjk=True),
        dict(state="running", kind="cc", project="united-memory",
             activity="Edit memory/sync.py", meta="8k"),
    ], "14:32", "3", "48k", "mock-fleet-3.png")

    draw_fleet([
        dict(state="running", kind="cc", project="esp32-agent-dashboard",
             activity="$ pytest tools/", meta="12k"),
        dict(state="waiting", kind="cc", project="修复登录页", cjk=True,
             activity="your turn — 两个方案二选一", meta="14m"),
        dict(state="running", kind="cx", project="ballru",
             activity="Read src/game/loop.ts", meta="8k"),
        dict(state="urgent", kind="cc", project="united-memory",
             activity="approve?  git push --force", meta="45s"),
    ], "14:32", "4", "71k", "mock-fleet-4.png")
