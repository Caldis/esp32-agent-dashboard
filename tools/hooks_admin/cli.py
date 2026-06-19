from __future__ import annotations
import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

from . import build_agents, status, install, enable, disable, base
from . import state as state_mod


def _agents_and_state(args):
    homes = {}
    if args.cc_home:
        homes["claude-code"] = args.cc_home
    if args.codex_home:
        homes["codex"] = args.codex_home
    agents = build_agents(homes)
    st_path = Path(args.state) if args.state else state_mod.default_state_path()
    return agents, state_mod.State(st_path)


def _targets(args, agents) -> list[str]:
    if args.agent in (None, "all"):
        return list(agents.keys())
    return [args.agent]


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="hooks_admin",
                                description="管理 CC/Codex 上报 hook 的安装与启用")
    p.add_argument("action", choices=["status", "install", "enable", "disable"])
    p.add_argument("--agent", choices=["claude-code", "codex", "all"], default="all")
    p.add_argument("--scope", choices=["user"], default="user")  # 本阶段仅 user
    p.add_argument("--json", action="store_true")
    p.add_argument("--cc-home", default=None, help="覆盖 CC home(测试用)")
    p.add_argument("--codex-home", default=None, help="覆盖 Codex home(测试用)")
    p.add_argument("--state", default=None, help="覆盖 state 文件路径(测试用)")
    args = p.parse_args(argv)

    agents, st = _agents_and_state(args)

    if args.action != "status":
        fn = {"install": install, "enable": enable, "disable": disable}[args.action]
        for kind in _targets(args, agents):
            fn(agents, st, kind, args.scope)

    result = {k: asdict(v) for k, v in status(agents, st, args.scope).items()}

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for k, v in result.items():
            state_str = ("未装" if not v["installed"]
                         else ("启用" if v["enabled"] else "禁用"))
            evs = ",".join(v["events"]) or "-"
            print(f"{v['display_name']:<12} {state_str:<4} events={evs}  {v['config_path']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
