#!/usr/bin/env python3
"""deco_audit.py — 装饰层的【负空间】验收门。

为什么需要它
------------
装饰的位置只在【特定姿态】下才成立：dashboard 的上下带按 fleet 的卡片
边界定位，可 ambient 把中间空了出来，同一个 y 就从"14 px 让位"变成
"贴着 chrome、离内容 52 px"。这类缺陷 golden diff 抓不到——两边都是
静止画面，像素也确实该不一样。

所以把人工验收固化成门：逐姿态截图、量出装饰墨带与内容墨带之间的每一段
间距、对照阈值。改了 AMBIENT_Y_CHIP 或挪了 footer 之后，负空间劣化会在
这里报出来，而不是等谁碰巧看见。

怎么量
------
装饰和内容同色（都是主题 text 色，只是亮度不同），行投影分不开。但
`?deco 0|1` 让它可分离：同一姿态截两张，**相减**就得到装饰层的纯图像。
两张各做行投影找"墨带"，交错排列即可算出每一段间距。

判据
----
  MIN_GAP    装饰墨带与相邻内容墨带的最小间距（呼吸下限）
  MAX_RATIO  同一条装饰带上下两侧间距之比（对称性上限）
上限不设：留白多不是缺陷，挤才是。

用法（必须独占串口）
  & ./tools/with_port.ps1 { python tools/perf/deco_audit.py }
  ...  --case dashboard-plain     只跑一个姿态
  ...  --keep                     保留中间截图便于人工复核
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：python -m pip install pillow")

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".harness" / "audit"
SHOT = ROOT / "screenshot.png"

# ── 判据 ────────────────────────────────────────────────────────────
MIN_GAP = 8         # px，装饰与内容之间的最小呼吸（0.8 m 处约 2.8 arcmin）
MAX_RATIO = 2.6     # 一条装饰带上下两侧间距之比
SETTLE_S = 4.0      # 推快照/切场景后的静置时间（姿态切换 + 簇滑动）

# 墨带判定。内容是 COVER 白，装饰只有 opa 26..190，两者阈值不同。
# CONTENT_TH 必须低到能吃下 fleet 卡片的底色（COL_SURFACE 0x1C1814，
# 灰度 ~26）——第一版设 40，卡片被整个滤掉，于是"上方最近的内容"越过
# 卡片找到了更远的东西，间距凭空多算 250 px。
CONTENT_TH = 18
DECO_TH = 10

# 屏幕最外圈属于 ui_glow（5 层 x step8+width7 + 运动外溢，见 CLAUDE.md）。
# 它在两次截图之间一直在呼吸，差分必然把它算成"装饰"。装饰本来就不许
# 进这一圈，所以直接把检测区夹到中间。
EDGE_SKIP = 46
MIN_BAND_H = 2      # 少于这么高的墨带当噪声丢掉
# 一行至少有这么多墨像素才算"墨带"。这是本脚本最关键的一个阈值：两张
# 截图之间内容本来就在动（呼吸环、秒位、辉光），差分里必然混进它们。
# 但那些是【局部】变化——呼吸环是圆的，任何一行最多切到两段边缘、几十
# 个像素；而要量的装饰横带宽 174..328 px。用计数一刀就分开了。
# 代价是竖直装饰（侧轴）检不到——它们本来也不参与垂直间距。
DECO_MIN_INK = 100
# 内容侧要低得多：呼吸环是【描边】，每行只切到两段各 ~3 px，用 60 会把
# 这个最重要的邻居整个滤掉（它正是上带下方要让位的对象）。
CONTENT_MIN_INK = 8

AWAIT = ('{"agents":[{"kind":"claude-code","session_id":"a1",'
         '"status":"waiting","cwd":"/x/alpha","awaiting_kind":"approve"}]}')
FLEET = ('{"agents":[{"kind":"claude-code","session_id":"a1","status":"running","cwd":"/x/a"},'
         '{"kind":"codex","session_id":"b2","status":"running","cwd":"/x/b"},'
         '{"kind":"cursor","session_id":"c3","status":"running","cwd":"/x/c"}]}')
EMPTY = '{"agents":[]}'

# (名字, 按键, 快照, 比例上限覆盖)
# 按键映射见 CLAUDE.md：BOOT=dashboard PWR=clock USER=weather
#
# clock 的顶带【刻意贴顶缘】（v7.5 的决定：它上移到 y64 才不再读作"悬在
# 半空的一横"），所以 33/95 的不对称是设计意图不是缺陷。豁免写在这里而
# 不是放宽全局阈值——一条豁免要有名字和理由，才不会变成默认。
CASES = [
    ("dashboard-plain", "boot", EMPTY, None),
    ("dashboard-chip",  "boot", AWAIT, None),
    ("dashboard-fleet", "boot", FLEET, None),
    ("clock",           "pwr",  EMPTY, 3.4),
    ("weather",         "user", EMPTY, None),
]


def console(cmd: str) -> dict:
    r = subprocess.run(
        ["esp-harness", "console", "--cmd", cmd, "--json"],
        capture_output=True, text=True, encoding="utf-8", timeout=60)
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except Exception:
        return {}


def shoot(dst: Path) -> Image.Image:
    subprocess.run(["esp-harness", "screenshot", "--size", "480"],
                   capture_output=True, text=True, encoding="utf-8", timeout=180)
    if not SHOT.exists():
        raise RuntimeError("screenshot 未产出")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(SHOT, dst)
    return Image.open(dst).convert("L")


def row_profile(img: Image.Image, th: int) -> list[int]:
    """每一行【超过阈值的像素个数】。不用最大亮度：那样任何一个抖动的
    像素都会让整行算成墨带。"""
    w, h = img.size
    px = img.tobytes()
    return [sum(1 for v in px[y * w:(y + 1) * w] if v >= th) for y in range(h)]


def band_x(img: Image.Image, y1: int, y2: int, th: int) -> tuple[int, int]:
    """墨带在 x 方向的跨度。**必须带上它**：行投影只看垂直方向，一条装饰
    带和一块内容落在同一行，可能是垂直挤压，也可能是左右并排——完全相反
    的两件事。dashboard 的上带就是与顶部时间同行分列的，没有这个检查会
    把"并排"误报成"距内容 3 px"。"""
    w = img.size[0]
    px = img.tobytes()
    lo, hi = w, -1
    for y in range(y1, y2 + 1):
        row = px[y * w:(y + 1) * w]
        for x in range(w):
            if row[x] >= th:
                if x < lo: lo = x
                if x > hi: hi = x
    return (lo, hi) if hi >= 0 else (0, w - 1)


def x_hit(a: tuple[int, int], b: tuple[int, int]) -> bool:
    return not (a[1] < b[0] or a[0] > b[1])


def bands(profile: list[int], min_ink: int) -> list[tuple[int, int]]:
    out, start = [], None
    for y, v in enumerate(profile):
        if v >= min_ink and start is None:
            start = y
        elif v < min_ink and start is not None:
            if y - start >= MIN_BAND_H:
                out.append((start, y - 1))
            start = None
    if start is not None and len(profile) - start >= MIN_BAND_H:
        out.append((start, len(profile) - 1))
    return out


def diff_image(full: Image.Image, content: Image.Image) -> Image.Image:
    """装饰层的纯图像。装饰画在内容【之下】，所以它只会让像素变亮，
    单向差分即可；反向的差值是内容自己的抖动（时钟秒进位等），丢掉。"""
    fp, cp = full.tobytes(), content.tobytes()
    return Image.frombytes("L", full.size,
                           bytes(max(0, a - b) for a, b in zip(fp, cp)))


def audit_case(name: str, key: str, snap: str, keep: bool,
               max_ratio: float | None = None) -> list[str]:
    max_ratio = max_ratio or MAX_RATIO
    console(f"dash snapshot {snap}")
    time.sleep(1.0)
    console(f"dash btn {key}")
    time.sleep(SETTLE_S)

    console("?deco 0")
    time.sleep(0.6)
    img_content = shoot(OUT / f"{name}-content.png")
    console("?deco 1")
    time.sleep(0.6)
    img_full = shoot(OUT / f"{name}-full.png")

    img_deco = diff_image(img_full, img_content)
    if keep:
        img_deco.save(OUT / f"{name}-deco.png")

    c_bands = bands(row_profile(img_content, CONTENT_TH), CONTENT_MIN_INK)
    d_bands = bands(row_profile(img_deco, DECO_TH), DECO_MIN_INK)
    # 掐掉 glow 占据的外圈，否则它的呼吸会被当成装饰带。
    lo, hi = EDGE_SKIP, img_full.size[1] - EDGE_SKIP
    d_bands = [b for b in d_bands if b[0] >= lo and b[1] <= hi]

    print(f"\n── {name} ─────────────────────────────")
    print(f"   内容墨带 {len(c_bands)}  装饰墨带 {len(d_bands)}")
    if not d_bands:
        return [f"{name}: 未检出任何装饰墨带（?deco 差分为空）"]

    cx = {b: band_x(img_content, b[0], b[1], CONTENT_TH) for b in c_bands}

    problems = []
    for (dy1, dy2) in d_bands:
        dx = band_x(img_deco, dy1, dy2, DECO_TH)
        # 只有 x 方向真的压在一起的内容才算"相邻"。
        above = [b for b in c_bands if b[1] < dy1 and x_hit(dx, cx[b])]
        below = [b for b in c_bands if b[0] > dy2 and x_hit(dx, cx[b])]
        g_up = dy1 - above[-1][1] if above else None
        g_dn = below[0][0] - dy2 if below else None
        up_s = f"{g_up:>4}" if g_up is not None else "   -"
        dn_s = f"{g_dn:>4}" if g_dn is not None else "   -"

        flag = ""
        for g, side in ((g_up, "上"), (g_dn, "下")):
            if g is not None and g < MIN_GAP:
                flag = " << 过近"
                problems.append(f"{name}: 装饰带 y{dy1}..{dy2} 距内容仅 {g} px（{side}）")
        if g_up and g_dn and g_up >= MIN_GAP and g_dn >= MIN_GAP:
            hi, lo = max(g_up, g_dn), min(g_up, g_dn)
            if lo and hi / lo > max_ratio:
                flag = " << 失衡"
                problems.append(
                    f"{name}: 装饰带 y{dy1}..{dy2} 两侧 {g_up}/{g_dn} 比 {hi/lo:.1f}")
        print(f"   y{dy1:>3}..{dy2:<3}  上{up_s}  下{dn_s}{flag}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", help="只跑这一个姿态")
    ap.add_argument("--keep", action="store_true", help="保留差分图")
    a = ap.parse_args()

    cases = [c for c in CASES if not a.case or c[0] == a.case]
    if not cases:
        return print(f"未知姿态：{a.case}") or 2

    all_problems = []
    for name, key, snap, ratio in cases:
        try:
            all_problems += audit_case(name, key, snap, a.keep, ratio)
        except Exception as e:                       # noqa: BLE001
            all_problems.append(f"{name}: 采集失败 {e}")

    console(f"dash snapshot {EMPTY}")
    print("\n" + "=" * 46)
    if all_problems:
        print(f"FAIL — {len(all_problems)} 处")
        for p in all_problems:
            print("  !!", p)
        return 1
    print(f"PASS — {len(cases)} 个姿态的负空间全部达标 "
          f"(min_gap>={MIN_GAP}px, ratio<={MAX_RATIO})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
