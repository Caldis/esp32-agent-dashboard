"""hooks_admin — 可扩展的 agent hook 管理器(CC + Codex,核心 + adapter)。"""
from __future__ import annotations
from pathlib import Path

from . import base, state as state_mod
from .claude_code import ClaudeCodeAdapter


def build_agents(homes: dict | None = None) -> dict:
    """构造 adapter 注册表;homes 可注入(测试用临时目录),默认真实 ~。"""
    home = Path.home()
    homes = homes or {}
    cc_home = Path(homes.get("claude-code", home))
    agents: dict = {}
    agents["claude-code"] = ClaudeCodeAdapter(cc_home)
    return agents


def _command_for(adapter):
    hd = base.hook_dispatch_path()
    return lambda ev: base.command_line(hd, ev, adapter.kind)


def status(agents: dict, st: state_mod.State, scope: str = "user") -> dict:
    out: dict = {}
    for kind, ad in agents.items():
        in_cfg, found = ad.detect(scope)
        rec = st.get(kind)
        if in_cfg:
            installed, enabled, events = True, True, sorted(found.keys())
        elif rec is not None:
            installed, enabled = True, False
            events = sorted((rec.get("entries") or {}).keys())
        else:
            installed, enabled, events = False, False, []
        out[kind] = base.AgentHookStatus(
            kind=kind, display_name=ad.display_name, supported=ad.supported,
            installed=installed, enabled=enabled, events=events,
            config_path=str(ad.config_paths(scope)[0]),
        )
    return out


def install(agents: dict, st: state_mod.State, kind: str, scope: str = "user"):
    ad = agents[kind]
    written = ad.install(scope, _command_for(ad))
    st.set(kind, enabled=True, scope=scope, entries=written)
    st.save()
    return status(agents, st, scope)[kind]
