#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""trans_bench.py — 六种场景转场的渲染基准（v7.0 基线工具）。

在真机上驱动 `dash btn`（与物理按键同一条代码路径）逐一触发全部六种
有序场景对的转场，用 `?perf`（读取即清零的窗口统计）夹住每一次转场。

方法
----
每次按键：`?perf`（清窗）→ `dash btn <key>` → 静候 1.6 s → `?perf`
（读数）。窗口覆盖整个转场（约 1.1 s：出场 240 ms + 黑幕瞬切 + 入场
520 ms + 错峰/余量）外加一小段空闲尾巴；`frame_ms` 是【真正渲染了的
帧】的平均刷新周期成本，尾巴的稀释很小、且各次运行一致。`drawn` 兼作
运动采样数——v7.0 起动画计时器跟随刷新档（见 ui_motion.c），drawn 越
多代表运动越顺滑，而不是越贵。

行标签按【设备实测】的按键映射硬编码（v7.0 用 scene_trans 日志核对）：
BOOT=dashboard、PWR=clock、USER=weather。物理左→右排列是 BOOT,PWR,USER。

每次运行都检查的不变量（违反则 exit 2）：
  - 设备没有中途重启（uptime 单调）——这条曾经就是抓住 bake panic 的
    信号（frame_ms=0 的行 + 时间戳归零）；
  - `?ghost` 的 mismatched == 0（转场替身几何自检）;
  - 每种转场都真的画了帧（drawn 不为 0）;
  - 共享元素 held 计数与预期表一致（dashboard↔clock 钉住 4 个 footer
    演员，weather 配对为 0）——scene_trans 的 held 日志是 v6.2 连续性
    层唯一的漂移探测器，这里把它固化成被检查的不变量。

回归门禁（`--compare`，exit 1）：frame_ms 或 render_avg_us 比参考值
差 15%（--tolerance-pct）以上。

必须独占串口——务必套 with_port.ps1 跑，否则 hook 自启的 bridge 会中途
抢走 COM9（v6.3 就毁过三次采样）：

    & ./tools/with_port.ps1 { python tools/perf/trans_bench.py --label baseline-v7.0 }
    & ./tools/with_port.ps1 { python tools/perf/trans_bench.py --compare baseline }

输出：tools/perf/results/trans-<ts>-<label>.json（schema trans_bench.v1）
`--compare baseline` 取 results/ 里最新的 *baseline* 结果做参考。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    from esp_harness.core.console_session import ConsoleSession
except ImportError:
    sys.exit("esp_harness not importable — pip install -e D:/Code/esp-harness/tools/esp-harness")

THIS_DIR = Path(__file__).resolve().parent
REPO = THIS_DIR.parents[1]
RESULTS_DIR = THIS_DIR / "results"

# (key, transition, (expected outro held, expected intro held))
# 第一按只用来把设备归位到 dashboard（已在位时是一次辉光 ping），不计入。
PRESSES = [
    ("boot", None, None),
    ("pwr",  "dashboard->clock", (4, 4)),
    ("user", "clock->weather",   (0, 0)),
    ("boot", "weather->dashboard", (0, 0)),
    ("user", "dashboard->weather", (0, 0)),
    ("pwr",  "weather->clock",   (0, 0)),
    ("boot", "clock->dashboard", (4, 4)),
]

HELD_RE = re.compile(r"(outro|intro)\s+(\w+):\s+(\d+)/(\d+) held")


def device_port() -> str:
    try:
        return json.loads((REPO / "harness.json").read_text("utf-8")).get("port", "COM9")
    except Exception:
        return "COM9"


def git_describe() -> str:
    try:
        out = subprocess.run(["git", "describe", "--always", "--dirty"],
                             cwd=REPO, capture_output=True, text=True, timeout=10)
        return out.stdout.strip() or "?"
    except Exception:
        return "?"


def parse_stat(text: str) -> dict:
    try:
        return json.loads(text)
    except Exception:
        return {}


def measure(port: str, settle_s: float, bake: str) -> dict:
    rows, problems = [], []
    with ConsoleSession(port) as s:
        start_stat = parse_stat(s.send("?stat").text)
        bake_reply = parse_stat(s.send(f"?bake {bake}").text)
        s.send("?ghost")                       # 读掉历史计数，本次运行从 0 记
        for key, label, expect in PRESSES:
            s.send("?perf")                    # 清窗
            r = s.send(f"dash btn {key}")
            if not r.ok:
                problems.append(f"{label or 'normalize'}: dash btn {key} -> ERR {r.text}")
            time.sleep(settle_s)
            r = s.send("?perf")
            if label is None:
                continue                       # 归位按压，丢弃
            held = []
            for ln in r.other_lines:
                m = HELD_RE.search(ln)
                if m:
                    held.append({"phase": m.group(1), "scene": m.group(2),
                                 "held": int(m.group(3)), "actors": int(m.group(4))})
            try:
                perf = json.loads(r.text)
            except Exception:
                problems.append(f"{label}: unparseable ?perf reply {r.text!r}")
                continue
            if perf.get("drawn", 0) == 0:
                problems.append(f"{label}: drawn=0 — transition never rendered (reboot? wrong scene?)")
            if expect is not None:
                got = tuple(h["held"] for h in held if h["phase"] == "outro") + \
                      tuple(h["held"] for h in held if h["phase"] == "intro")
                if len(held) != 2:
                    problems.append(f"{label}: expected 2 held log lines, saw {len(held)}")
                elif got != expect:
                    problems.append(f"{label}: held drift — expected {expect}, got {got} "
                                    f"(shared-element pose no longer matches; see scene_trans.h v6.2)")
            rows.append({"transition": label, "key": key, "perf": perf, "held": held})
        ghost = parse_stat(s.send("?ghost").text)
        end_stat = parse_stat(s.send("?stat").text)

    rebooted = (end_stat.get("uptime_ms", 0) < start_stat.get("uptime_ms", 0))
    if rebooted:
        problems.append("device REBOOTED mid-run (uptime went backwards) — treat as crash")
    if ghost.get("mismatched", 0):
        problems.append(f"?ghost mismatched={ghost['mismatched']} — sprite geometry broken: {ghost.get('last')}")

    return {
        "schema": "trans_bench.v1",
        "started_local": datetime.now().isoformat(timespec="seconds"),
        "git": git_describe(),
        "port": port,
        "settle_s": settle_s,
        "bake": bake_reply,
        "start_stat": start_stat,
        "end_stat": end_stat,
        "rows": rows,
        "ghost": ghost,
        "rebooted": rebooted,
        "problems": problems,
    }


def print_table(res: dict) -> None:
    print(f"\ngit {res['git']}  bake={res['bake'].get('bake')}  settle={res['settle_s']}s  port={res['port']}")
    hdr = (f"{'transition':<22}{'frame_ms':>9}{'drawn':>7}{'overrun':>8}"
           f"{'render_avg':>11}{'wait_avg':>9}{'inval_px/f':>11}{'held':>6}")
    print(hdr)
    for row in res["rows"]:
        d = row["perf"]
        held = "+".join(str(h["held"]) for h in row["held"]) or "-"
        print(f"{row['transition']:<22}{d['frame_ms']:>9.1f}{d['drawn']:>7}{d['overrun']:>8}"
              f"{d['render_avg_us']/1000:>10.1f}m{d['wait_avg_us']/1000:>8.1f}m"
              f"{d['inval_px_per_frame']:>11}{held:>6}")
    for p in res["problems"]:
        print(f"  !! {p}")


def find_reference(spec: str) -> Path:
    if spec != "baseline":
        return Path(spec)
    cands = sorted(RESULTS_DIR.glob("trans-*baseline*.json"),
                   key=lambda p: p.stat().st_mtime)
    if not cands:
        sys.exit("no trans-*baseline*.json under tools/perf/results — run with --label baseline-<ver> first")
    return cands[-1]


def compare(res: dict, ref_path: Path, tol_pct: float) -> bool:
    ref = json.loads(ref_path.read_text("utf-8"))
    ref_rows = {r["transition"]: r["perf"] for r in ref["rows"]}
    print(f"\ncompare vs {ref_path.name} (git {ref.get('git')}), tolerance {tol_pct}%")
    print(f"{'transition':<22}{'frame_ms':>16}{'render_avg_ms':>18}{'drawn':>12}")
    regressed = False
    for row in res["rows"]:
        t = row["transition"]
        cur, old = row["perf"], ref_rows.get(t)
        if not old:
            print(f"{t:<22}{'(no reference)':>16}")
            continue

        def delta(field, scale=1.0):
            c, o = cur[field] * scale, old[field] * scale
            pct = (c - o) * 100.0 / o if o else 0.0
            return c, o, pct

        fc, fo, fp = delta("frame_ms")
        rc, ro, rp = delta("render_avg_us", 1e-3)
        dc, do_, _ = delta("drawn")
        bad = fp > tol_pct or rp > tol_pct
        regressed |= bad
        mark = "  <-- REGRESSION" if bad else ""
        print(f"{t:<22}{fo:>7.1f}->{fc:<5.1f}{fp:>+5.0f}%"
              f"{ro:>9.1f}->{rc:<5.1f}{rp:>+5.0f}%"
              f"{do_:>6.0f}->{dc:<4.0f}{mark}")
    return regressed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--label", default="run", help="结果文件名后缀（baseline-* 的会被 --compare baseline 找到）")
    ap.add_argument("--settle-s", type=float, default=1.6, help="每次按键后的窗口时长（方法学常数，改了就不可比）")
    ap.add_argument("--bake", choices=["0", "1"], default="0", help="精灵烘焙 A/B 臂（默认 0 = 项目默认路径）")
    ap.add_argument("--compare", metavar="PATH|baseline", help="与参考结果比对，超容差 exit 1")
    ap.add_argument("--tolerance-pct", type=float, default=15.0)
    ap.add_argument("--no-save", action="store_true", help="只测不落盘（试跑用）")
    args = ap.parse_args()

    res = measure(device_port(), args.settle_s, args.bake)
    print_table(res)

    if not args.no_save:
        RESULTS_DIR.mkdir(parents=True, exist_ok=True)
        ts = datetime.now().strftime("%Y-%m-%d-%H%M%S")
        out = RESULTS_DIR / f"trans-{ts}-{args.label}.json"
        out.write_text(json.dumps(res, ensure_ascii=False, indent=2), "utf-8")
        print(f"\nsaved: {out.relative_to(REPO)}")

    code = 0
    if args.compare:
        if compare(res, find_reference(args.compare), args.tolerance_pct):
            code = max(code, 1)
    if res["problems"]:
        code = 2
    return code


if __name__ == "__main__":
    sys.exit(main())
