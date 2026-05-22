"""
One-shot composite renderer for docs/img/hero.png and docs/img/scenes-strip.png.

Run from anywhere with PIL installed. Outputs are written next to the
existing dash-*.png placeholders in docs/img/.
"""
from __future__ import annotations
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import os
import math
import random

# Paths -----------------------------------------------------------------------
PROJECT_ROOT = "D:/Code/esp32-agent-dashboard"
IMG_DIR = f"{PROJECT_ROOT}/docs/img"
HERO_PATH = f"{IMG_DIR}/hero.png"
STRIP_PATH = f"{IMG_DIR}/scenes-strip.png"

# Brand palette ---------------------------------------------------------------
PAPER = (243, 238, 226)
PAPER_HI = (249, 244, 230)
INK = (28, 24, 20)
INK_MUTE = (90, 81, 74)
INK_FADE = (138, 128, 122)
TEAL = (14, 124, 123)
TEAL_BRIGHT = (43, 179, 177)
RUST = (184, 67, 26)
DUSK = (107, 122, 168)
MOSS = (52, 74, 54)
GOLD = (184, 144, 32)
NOIR_BG = (11, 10, 9)
NOIR_FG = PAPER

# Fonts -----------------------------------------------------------------------
F_SERIF = "C:/Windows/Fonts/georgia.ttf"
F_SERIF_B = "C:/Windows/Fonts/georgiab.ttf"
F_SERIF_I = "C:/Windows/Fonts/georgiai.ttf"
F_MONO = "C:/Windows/Fonts/consola.ttf"
F_MONO_B = "C:/Windows/Fonts/consolab.ttf"


def f(path, size):
    return ImageFont.truetype(path, size)


# Brand mark (logo) drawn directly with PIL primitives -----------------------
def draw_logo(img: Image.Image, cx: int, cy: int, scale: float = 1.0, dark: bool = False):
    """Draw the project mark centred at (cx, cy). The mark's natural box is 120 units."""
    d = ImageDraw.Draw(img)
    s = scale
    fg = PAPER if dark else INK
    pulse = TEAL_BRIGHT if dark else TEAL
    centre_fill = INK if dark else PAPER
    rust_dot = (216, 99, 46) if dark else RUST

    # dashed outer rect (low emphasis)
    rx0 = cx - 46 * s; ry0 = cy - 46 * s
    rx1 = cx + 46 * s; ry1 = cy + 46 * s
    radius = 18 * s
    # Approximate dashes with short segments along the rounded rect perimeter
    # Easier: stroke a solid rect at low alpha. PIL has no built-in dash; draw segments.
    dash_color = fg + (88,)  # alpha 88
    # We can't blend onto the base image easily without alpha; use a transparent overlay
    overlay = Image.new("RGBA", img.size, (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    # Build dashed rounded rectangle via small line segments
    def dashed_rrect(od, x0, y0, x1, y1, r, color, width, dash, gap):
        # top, right, bottom, left segments (skip corners — corner arcs solid-stroked thin)
        segs = []
        # top
        segs.append(((x0 + r, y0), (x1 - r, y0)))
        # right
        segs.append(((x1, y0 + r), (x1, y1 - r)))
        # bottom (right to left)
        segs.append(((x1 - r, y1), (x0 + r, y1)))
        # left (bottom to top)
        segs.append(((x0, y1 - r), (x0, y0 + r)))
        for (sx, sy), (ex, ey) in segs:
            dx = ex - sx; dy = ey - sy
            length = math.hypot(dx, dy)
            if length == 0:
                continue
            ux = dx / length; uy = dy / length
            t = 0
            while t < length:
                a = (sx + ux * t, sy + uy * t)
                t2 = min(t + dash, length)
                b = (sx + ux * t2, sy + uy * t2)
                od.line([a, b], fill=color, width=width)
                t = t2 + gap
        # corner arcs solid (small, low emphasis)
        od.arc((x0, y0, x0 + 2 * r, y0 + 2 * r), 180, 270, fill=color, width=width)
        od.arc((x1 - 2 * r, y0, x1, y0 + 2 * r), 270, 360, fill=color, width=width)
        od.arc((x1 - 2 * r, y1 - 2 * r, x1, y1), 0, 90, fill=color, width=width)
        od.arc((x0, y1 - 2 * r, x0 + 2 * r, y1), 90, 180, fill=color, width=width)

    dashed_rrect(od, rx0, ry0, rx1, ry1, radius, dash_color, max(1, int(2 * s)),
                 dash=int(5 * s), gap=int(4 * s))

    # screen ring
    r_screen = 30 * s
    od.ellipse((cx - r_screen, cy - r_screen, cx + r_screen, cy + r_screen),
               outline=fg + (255,), width=max(2, int(3.2 * s)))

    # pulse line: flat left + ECG bump + flat right
    pulse_w = max(2, int(3.2 * s))
    y0 = cy
    # left flat
    od.line([(cx - 28 * s, y0), (cx - 15 * s, y0)], fill=pulse + (255,), width=pulse_w)
    # ECG bump points (relative): from (-15,0) -> (-11,0) -> (-8.5,-10) -> (-6,10) -> (-3.5,-4) -> (0,0)
    bump = [
        (cx - 15 * s, y0),
        (cx - 11 * s, y0),
        (cx - 8.5 * s, y0 - 10 * s),
        (cx - 6 * s, y0 + 10 * s),
        (cx - 3.5 * s, y0 - 4 * s),
        (cx, y0),
    ]
    od.line(bump, fill=pulse + (255,), width=pulse_w, joint="curve")
    # right flat
    od.line([(cx, y0), (cx + 28 * s, y0)], fill=pulse + (255,), width=pulse_w)

    # centre dot
    rdot = 3.4 * s
    od.ellipse((cx - rdot, cy - rdot, cx + rdot, cy + rdot),
               fill=centre_fill + (255,), outline=fg + (255,), width=max(1, int(1.4 * s)))

    # rust micro-dot at far right of pulse
    rd_w = 3 * s; rd_h = 3.2 * s
    rdx = cx + 26 * s
    od.rectangle((rdx, y0 - rd_h / 2, rdx + rd_w, y0 + rd_h / 2),
                 fill=rust_dot + (255,))

    img.alpha_composite(overlay)


# Device illustration: a circular AMOLED in a square dev-board, on a desk -----
def draw_device(img: Image.Image, cx: int, cy: int, board_size: int, scene: Image.Image):
    """
    Draws a stylized top-down view of the Waveshare ESP32-S3-Touch-AMOLED-2.16:
    a rounded square dev board (slightly smaller than `board_size`) with the
    circular display centred. The `scene` image is composited inside the display.
    """
    d = ImageDraw.Draw(img, "RGBA")

    # Soft contact shadow under board
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.rounded_rectangle(
        (cx - board_size // 2 + 12, cy - board_size // 2 + 22,
         cx + board_size // 2 + 12, cy + board_size // 2 + 30),
        radius=int(board_size * 0.10), fill=(0, 0, 0, 120))
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))
    img.alpha_composite(shadow)

    # Board body — matte black PCB feel with subtle highlight
    board_bg = (24, 22, 20, 255)
    bx0 = cx - board_size // 2; by0 = cy - board_size // 2
    bx1 = cx + board_size // 2; by1 = cy + board_size // 2
    d.rounded_rectangle((bx0, by0, bx1, by1),
                        radius=int(board_size * 0.10),
                        fill=board_bg)

    # Subtle inner bevel
    d.rounded_rectangle((bx0 + 3, by0 + 3, bx1 - 3, by1 - 3),
                        radius=int(board_size * 0.095),
                        outline=(60, 56, 52, 255), width=1)

    # Silkscreen marks (corner labels) — quiet hardware character
    try:
        f_silk = f(F_MONO, max(10, board_size // 36))
    except Exception:
        f_silk = ImageFont.load_default()
    silk_color = (140, 130, 120, 200)
    pad = int(board_size * 0.055)
    d.text((bx0 + pad, by0 + pad), "ESP32-S3", font=f_silk, fill=silk_color)
    d.text((bx1 - pad, by0 + pad), "AMOLED", font=f_silk, fill=silk_color, anchor="ra")
    d.text((bx0 + pad, by1 - pad - f_silk.size), "USB", font=f_silk, fill=silk_color)
    d.text((bx1 - pad, by1 - pad - f_silk.size), "2.16\"", font=f_silk, fill=silk_color, anchor="ra")

    # Two small "screw" dots in opposite corners for hardware authenticity
    for (sx, sy) in [(bx0 + int(board_size * 0.045), by0 + int(board_size * 0.045)),
                     (bx1 - int(board_size * 0.045), by1 - int(board_size * 0.045))]:
        d.ellipse((sx - 4, sy - 4, sx + 4, sy + 4), fill=(50, 46, 42, 255),
                  outline=(80, 74, 68, 255), width=1)

    # USB-C cutout on bottom edge — quiet detail
    usb_w = int(board_size * 0.12); usb_h = 6
    d.rounded_rectangle(
        (cx - usb_w // 2, by1 - usb_h, cx + usb_w // 2, by1 + 2),
        radius=2, fill=(8, 8, 8, 255))

    # User button (BOOT) and another (RESET) — small pads
    btn_r = 4
    bx_btn = bx0 + int(board_size * 0.12)
    by_btn = by1 - int(board_size * 0.085)
    d.ellipse((bx_btn - btn_r, by_btn - btn_r, bx_btn + btn_r, by_btn + btn_r),
              fill=(140, 140, 140, 255))
    bx_btn2 = bx1 - int(board_size * 0.12)
    d.ellipse((bx_btn2 - btn_r, by_btn - btn_r, bx_btn2 + btn_r, by_btn + btn_r),
              fill=(140, 140, 140, 255))

    # Display bezel — a slightly raised dark ring around the circle
    disp_d = int(board_size * 0.78)
    bezel_d = disp_d + 14
    d.ellipse((cx - bezel_d // 2, cy - bezel_d // 2,
               cx + bezel_d // 2, cy + bezel_d // 2),
              fill=(8, 8, 8, 255))
    # Display glass — slightly inset, will be replaced by scene composite below
    d.ellipse((cx - disp_d // 2, cy - disp_d // 2,
               cx + disp_d // 2, cy + disp_d // 2),
              fill=(0, 0, 0, 255))

    # Composite the scene image into a circular mask of disp_d size
    scene_resized = scene.resize((disp_d, disp_d), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (disp_d, disp_d), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse((0, 0, disp_d, disp_d), fill=255)
    img.paste(scene_resized, (cx - disp_d // 2, cy - disp_d // 2), mask)

    # Subtle glass reflection — a soft elliptical highlight on the upper-left
    refl = Image.new("RGBA", img.size, (0, 0, 0, 0))
    rd = ImageDraw.Draw(refl)
    rx = cx - disp_d * 0.18
    ry = cy - disp_d * 0.28
    rw = disp_d * 0.55; rh = disp_d * 0.22
    rd.ellipse((rx - rw / 2, ry - rh / 2, rx + rw / 2, ry + rh / 2),
               fill=(255, 255, 255, 28))
    refl = refl.filter(ImageFilter.GaussianBlur(18))
    # Mask reflection to circle
    refl_mask = Image.new("L", img.size, 0)
    rmd = ImageDraw.Draw(refl_mask)
    rmd.ellipse((cx - disp_d // 2, cy - disp_d // 2,
                 cx + disp_d // 2, cy + disp_d // 2), fill=255)
    refl.putalpha(ImageChops_multiply(refl.split()[-1], refl_mask))
    img.alpha_composite(refl)


def ImageChops_multiply(a, b):
    from PIL import ImageChops
    return ImageChops.multiply(a, b)


# Scene renderers — drawn at 466x466 in the device's noir theme ---------------
def scene_canvas() -> Image.Image:
    im = Image.new("RGBA", (466, 466), NOIR_BG + (255,))
    return im


def scene_idle() -> Image.Image:
    im = scene_canvas()
    d = ImageDraw.Draw(im)
    # gentle dusk-coloured halo
    halo_r = 90
    halo = Image.new("RGBA", im.size, (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    for i in range(6, 0, -1):
        a = 12 + i * 4
        hd.ellipse((233 - halo_r - i * 8, 233 - halo_r - i * 8,
                    233 + halo_r + i * 8, 233 + halo_r + i * 8),
                   fill=DUSK + (a,))
    halo = halo.filter(ImageFilter.GaussianBlur(8))
    im.alpha_composite(halo)
    # inner dusk disc
    d.ellipse((233 - 70, 233 - 70, 233 + 70, 233 + 70), fill=DUSK + (210,))
    # zZz glyph
    font_z = f(F_SERIF_I, 48)
    d.text((233, 233), "z Z z", font=font_z, fill=PAPER + (235,), anchor="mm")
    # caption
    cap = f(F_MONO, 18)
    d.text((233, 350), "no active sessions", font=cap, fill=INK_FADE + (220,), anchor="mm")
    return im


def scene_sessions() -> Image.Image:
    im = scene_canvas()
    d = ImageDraw.Draw(im)
    # Title bar
    title = f(F_MONO_B, 16)
    d.text((233, 36), "SESSIONS", font=title, fill=INK_FADE + (220,), anchor="mm")

    # Header counters
    big = f(F_SERIF_B, 56)
    label = f(F_MONO, 13)
    # totals: 2 / running 1 / waiting 0
    MOSS_BRIGHT = (98, 158, 102)
    columns = [
        ("2", "total", TEAL_BRIGHT),
        ("1", "running", MOSS_BRIGHT),
        ("0", "waiting", GOLD),
    ]
    xs = [120, 233, 346]
    for x, (val, lab, col) in zip(xs, columns):
        d.text((x, 110), val, font=big, fill=col + (235,), anchor="mm")
        d.text((x, 152), lab.upper(), font=label, fill=INK_FADE + (220,), anchor="mm")

    # Divider
    d.line([(80, 188), (386, 188)], fill=INK_MUTE + (120,), width=1)

    # Recent entries — monospace log lines
    line_font = f(F_MONO, 14)
    line_font_b = f(F_MONO_B, 14)
    entries = [
        ("10:42", "Bash",  "git push origin main", RUST),
        ("10:41", "Edit",  "main.c  +8 -2",        RUST),
        ("10:39", "Read",  "main.c  120 lines",    RUST),
        ("10:30", "Grep",  "login  42 hits",       TEAL_BRIGHT),
    ]
    y = 210
    for t, tool, summary, col in entries:
        d.text((86, y), t, font=line_font, fill=INK_FADE + (200,))
        d.text((150, y), tool, font=line_font_b, fill=col + (235,))
        d.text((220, y), summary, font=line_font, fill=PAPER + (220,))
        y += 26

    # Footer: agent kind dots
    fy = 410
    d.ellipse((180 - 7, fy - 7, 180 + 7, fy + 7), fill=RUST + (235,))
    d.text((196, fy), "claude-code", font=f(F_MONO, 13), fill=PAPER + (200,), anchor="lm")
    d.ellipse((296 - 7, fy - 7, 296 + 7, fy + 7), fill=TEAL_BRIGHT + (235,))
    d.text((312, fy), "codex", font=f(F_MONO, 13), fill=PAPER + (200,), anchor="lm")

    return im


def scene_prompt() -> Image.Image:
    im = scene_canvas()
    d = ImageDraw.Draw(im)
    # Title
    d.text((233, 56), "PERMISSION", font=f(F_MONO_B, 16), fill=GOLD + (235,), anchor="mm")
    # Tool name big
    d.text((233, 130), "Bash", font=f(F_SERIF_B, 72), fill=PAPER + (240,), anchor="mm")
    # Hint
    d.text((233, 188), "rm -rf /tmp/foo", font=f(F_MONO, 18), fill=INK_FADE + (220,), anchor="mm")
    # Timer
    d.text((233, 232), "47s", font=f(F_MONO, 22), fill=GOLD + (200,), anchor="mm")

    # Two buttons (rounded rects)
    def btn(x_c, label_top, label_bottom, fill, fg):
        bw, bh = 140, 70
        x0 = x_c - bw // 2; y0 = 320
        d.rounded_rectangle((x0, y0, x0 + bw, y0 + bh), radius=14, fill=fill + (235,))
        d.text((x_c, y0 + 22), label_top, font=f(F_MONO_B, 18), fill=fg + (255,), anchor="mm")
        d.text((x_c, y0 + 48), label_bottom, font=f(F_MONO, 13), fill=fg + (200,), anchor="mm")

    btn(143, "BOOT", "approve", MOSS, PAPER)
    btn(323, "USER", "deny",   (199, 78, 96), PAPER)

    return im


def scene_tokens() -> Image.Image:
    im = scene_canvas()
    d = ImageDraw.Draw(im)
    d.text((233, 40), "TOKENS", font=f(F_MONO_B, 16), fill=INK_FADE + (220,), anchor="mm")

    # Two stat columns
    big = f(F_SERIF_B, 56)
    d.text((150, 120), "84.5k", font=big, fill=PAPER + (240,), anchor="mm")
    d.text((150, 160), "TOTAL", font=f(F_MONO, 13), fill=INK_FADE + (220,), anchor="mm")
    d.text((316, 120), "21.2k", font=big, fill=TEAL_BRIGHT + (235,), anchor="mm")
    d.text((316, 160), "TODAY", font=f(F_MONO, 13), fill=INK_FADE + (220,), anchor="mm")

    # Sparkline
    import random as _r
    _r.seed(7)
    pts = []
    base_y = 320
    amp = 50
    n = 60
    x0, x1 = 70, 396
    for i in range(n):
        x = x0 + (x1 - x0) * i / (n - 1)
        y = base_y - amp * (0.35 + 0.65 * abs(math.sin(i * 0.22 + i * 0.03 * _r.random())))
        pts.append((x, y))
    # Fill area under spark
    fill_pts = pts + [(x1, base_y + 10), (x0, base_y + 10)]
    overlay = Image.new("RGBA", im.size, (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    od.polygon(fill_pts, fill=TEAL + (60,))
    od.line(pts, fill=TEAL_BRIGHT + (235,), width=2, joint="curve")
    im.alpha_composite(overlay)

    # Baseline
    d.line([(70, base_y + 12), (396, base_y + 12)], fill=INK_MUTE + (180,), width=1)
    d.text((233, 400), "last 60 samples", font=f(F_MONO, 13), fill=INK_FADE + (200,), anchor="mm")
    return im


def scene_status() -> Image.Image:
    im = scene_canvas()
    d = ImageDraw.Draw(im)
    d.text((233, 40), "STATUS", font=f(F_MONO_B, 16), fill=INK_FADE + (220,), anchor="mm")
    # Two big columns
    big = f(F_SERIF_B, 48)
    d.text((150, 130), "7956 kB", font=big, fill=PAPER + (240,), anchor="mm")
    d.text((150, 172), "HEAP", font=f(F_MONO, 13), fill=INK_FADE + (220,), anchor="mm")
    d.text((316, 130), "147", font=big, fill=TEAL_BRIGHT + (235,), anchor="mm")
    d.text((316, 172), "UPTIME (m)", font=f(F_MONO, 13), fill=INK_FADE + (220,), anchor="mm")
    # Bottom row
    d.text((150, 290), "87 %",  font=big, fill=MOSS + (235,), anchor="mm")
    d.text((150, 332), "BATTERY", font=f(F_MONO, 13), fill=INK_FADE + (220,), anchor="mm")
    d.text((316, 290), "33 fps", font=big, fill=PAPER + (235,), anchor="mm")
    d.text((316, 332), "RENDER",  font=f(F_MONO, 13), fill=INK_FADE + (220,), anchor="mm")
    # Faint horizontal rule
    d.line([(90, 230), (376, 230)], fill=INK_MUTE + (140,), width=1)
    return im


# Hero composite -------------------------------------------------------------
def render_hero():
    W, H = 1600, 900
    bg = Image.new("RGBA", (W, H), PAPER + (255,))
    # warm vignette top-right
    vg = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    vd = ImageDraw.Draw(vg)
    for i in range(40):
        a = 5 - int(i * 0.1)
        if a <= 0:
            break
        vd.ellipse((W - 600 - i * 30, -200 - i * 20, W + 200 + i * 10, 500 + i * 20),
                   fill=(255, 245, 220, a))
    vg = vg.filter(ImageFilter.GaussianBlur(60))
    bg.alpha_composite(vg)

    # Desk surface — paper colour but a touch darker bottom band
    desk = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(desk)
    # subtle horizontal band gradient using stacked rects
    for i in range(120):
        a = int(i * 1.0)
        dd.rectangle((0, H - 240 + i * 2, W, H - 240 + i * 2 + 2),
                     fill=(60, 50, 42, min(60, a)))
    desk = desk.filter(ImageFilter.GaussianBlur(20))
    bg.alpha_composite(desk)

    # Decorative thin horizontal rule (teal+rust echo from social card)
    rd = ImageDraw.Draw(bg)
    rd.rectangle((120, H - 80, W - 120, H - 78), fill=TEAL + (220,))
    rd.rectangle((120, H - 80, 240, H - 78), fill=RUST + (255,))

    # Device — large, positioned right of centre
    scene = scene_prompt()  # the prompt is the most expressive scene
    draw_device(bg, cx=1080, cy=H // 2, board_size=620, scene=scene)

    # Mark + wordmark top-left
    draw_logo(bg, cx=160, cy=160, scale=1.5)
    # Wordmark — two-line, sized to clear the device on the right
    serif_big = f(F_SERIF, 68)
    serif_big_i = f(F_SERIF_I, 68)
    d = ImageDraw.Draw(bg)
    x0 = 250
    y1 = 125
    y2 = 200
    d.text((x0, y1), "esp32", font=serif_big, fill=INK + (255,), anchor="lm")
    w_esp32 = d.textlength("esp32", font=serif_big)
    d.text((x0 + w_esp32, y1), "-", font=serif_big_i, fill=TEAL + (255,), anchor="lm")
    w_hyph = d.textlength("-", font=serif_big_i)
    d.text((x0 + w_esp32 + w_hyph, y1), "agent", font=serif_big, fill=INK + (255,), anchor="lm")
    # second line — indent matches first
    d.text((x0, y2), "-", font=serif_big_i, fill=TEAL + (255,), anchor="lm")
    d.text((x0 + w_hyph, y2), "dashboard", font=serif_big, fill=INK + (255,), anchor="lm")

    # Tagline
    tag = f(F_SERIF_I, 32)
    d.text((100, 360),
           "Watch Claude Code and Codex work,",
           font=tag, fill=INK_MUTE + (255,), anchor="lm")
    d.text((100, 405),
           "approve permissions from a physical button.",
           font=tag, fill=INK_MUTE + (255,), anchor="lm")

    # Three feature lines, mono
    mono = f(F_MONO, 22)
    items = [
        ("466 × 466 AMOLED", "  on your desk"),
        ("Claude Code + Codex", "  side-by-side"),
        ("BOOT / USER buttons", "  approve, deny, 60s timeout"),
    ]
    y = 510
    for left, right in items:
        # teal tick
        d.rectangle((100, y + 10, 116, y + 12), fill=TEAL + (255,))
        d.text((130, y), left, font=mono, fill=INK + (255,))
        d.text((130 + d.textlength(left, font=mono), y), right, font=mono, fill=INK_FADE + (255,))
        y += 44

    # Bottom-left small URL
    d.text((120, H - 40), "github.com/Caldis/esp32-agent-dashboard",
           font=f(F_MONO, 18), fill=INK_FADE + (255,))

    bg.convert("RGB").save(HERO_PATH, "PNG", optimize=True)
    print(f"wrote {HERO_PATH} ({os.path.getsize(HERO_PATH)} bytes)")


# Scenes strip ---------------------------------------------------------------
def render_strip():
    W, H = 1600, 360
    bg = Image.new("RGBA", (W, H), PAPER + (255,))
    d = ImageDraw.Draw(bg)

    # Top thin rule
    d.rectangle((60, 18, W - 60, 20), fill=INK + (40,))

    # 5 scenes
    scenes = [
        ("idle",     scene_idle()),
        ("sessions", scene_sessions()),
        ("prompt",   scene_prompt()),
        ("tokens",   scene_tokens()),
        ("status",   scene_status()),
    ]

    n = len(scenes)
    margin = 60
    gutter = 24
    # Reserve ~78 px at the bottom for captions; disc diameter sized to fit
    caption_h = 78
    top_pad = 30
    avail_w = W - 2 * margin - (n - 1) * gutter
    avail_h = H - top_pad - caption_h - 16  # bezel breathing room
    disp = min(avail_w // n, avail_h)

    cy = top_pad + disp // 2
    # Column width is wider than disp; centre each disc in its column
    col_w = (W - 2 * margin - (n - 1) * gutter) // n
    x = margin
    label_font = f(F_MONO_B, 18)
    sub_font = f(F_MONO, 14)

    for name, scene in scenes:
        cx_col = x + col_w // 2
        # bezel
        bezel_d = disp + 14
        d.ellipse((cx_col - bezel_d // 2, cy - bezel_d // 2,
                   cx_col + bezel_d // 2, cy + bezel_d // 2),
                  fill=(20, 18, 16, 255))
        # disc
        d.ellipse((cx_col - disp // 2, cy - disp // 2,
                   cx_col + disp // 2, cy + disp // 2),
                  fill=NOIR_BG + (255,))
        # mask + composite
        scene_r = scene.resize((disp, disp), Image.LANCZOS).convert("RGBA")
        mask = Image.new("L", (disp, disp), 0)
        md = ImageDraw.Draw(mask)
        md.ellipse((0, 0, disp, disp), fill=255)
        bg.paste(scene_r, (cx_col - disp // 2, cy - disp // 2), mask)
        # Caption
        cap_y = cy + disp // 2 + 22
        d.text((cx_col, cap_y), name, font=label_font, fill=INK + (255,), anchor="mm")
        # Subtitle (one short descriptor)
        descs = {
            "idle":     "no active sessions",
            "sessions": "agents at a glance",
            "prompt":   "approve / deny tool use",
            "tokens":   "spend over time",
            "status":   "device health",
        }
        d.text((cx_col, cap_y + 22), descs[name], font=sub_font,
               fill=INK_FADE + (255,), anchor="mm")
        x += col_w + gutter

    # Bottom thin rule (teal w/ rust head, matches social card)
    d.rectangle((60, H - 14, W - 60, H - 12), fill=TEAL + (180,))
    d.rectangle((60, H - 14, 180, H - 12), fill=RUST + (255,))

    bg.convert("RGB").save(STRIP_PATH, "PNG", optimize=True)
    print(f"wrote {STRIP_PATH} ({os.path.getsize(STRIP_PATH)} bytes)")


if __name__ == "__main__":
    os.makedirs(IMG_DIR, exist_ok=True)
    render_hero()
    render_strip()
