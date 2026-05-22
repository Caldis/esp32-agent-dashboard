#!/usr/bin/env python3
"""discover.py — find esp32-agent-dashboard devices on the LAN via mDNS.

Scans for `_aagentdash._tcp` advertisements and prints a JSON array of
discovered devices. Used by the bridge to auto-detect a device without
requiring `--port`.

v0.4.0 scaffold (TRANS1). The firmware advertiser (main/transport/
mdns_discovery.c) is stubbed pending F2's build integration, so this
script is exercised end-to-end only after the v0.4.0 firmware cycle.
For now it's useful for:
  • smoke-testing a hand-rolled mDNS advertiser
  • CI: stub the advertiser in tools/mock_device_v1.py and assert
    the JSON shape

Usage:
    python discover.py                       # 3s scan, JSON to stdout
    python discover.py --timeout 5           # longer scan
    python discover.py --once                # exit after first device
    python discover.py --filter Clawd        # only show devices whose
                                              # instance name matches

JSON shape (one element per device):
    {
      "instance_name": "Clawd",
      "hostname":      "Clawd.local",
      "addresses":     ["192.168.1.42"],
      "port":          7321,
      "txt": {
        "proto":     "v1,v2",
        "fw":        "v0.4.0",
        "transport": "wifi_tls",
        "agents":    "2"
      },
      "service_type": "_aagentdash._tcp.local.",
      "discovered_at": 1779600123.456,
      "transport":     "wifi_tls"
    }

Dependencies:
    pip install zeroconf
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Any

SERVICE_TYPE = "_aagentdash._tcp.local."


def _zeroconf_or_die():
    try:
        from zeroconf import Zeroconf, ServiceBrowser  # noqa: F401
        return True
    except ImportError:
        print(
            "ERROR: discover.py requires the `zeroconf` package.\n"
            "       pip install zeroconf",
            file=sys.stderr,
        )
        return False


class _DiscoveryListener:
    """ServiceBrowser callback target — accumulates devices into self.found."""

    def __init__(self, name_filter: str | None = None):
        self.found: dict[str, dict[str, Any]] = {}
        self.name_filter = name_filter

    def add_service(self, zc, type_, name):
        info = zc.get_service_info(type_, name, timeout=2000)
        if info is None:
            return
        instance = name.split(".")[0]
        if self.name_filter and self.name_filter.lower() not in instance.lower():
            return
        addresses = []
        for raw in info.parsed_addresses():
            addresses.append(raw)
        # TXT records arrive as bytes; decode tolerantly.
        txt: dict[str, str] = {}
        for k, v in (info.properties or {}).items():
            try:
                key = k.decode("utf-8") if isinstance(k, bytes) else str(k)
            except UnicodeDecodeError:
                key = repr(k)
            if v is None:
                txt[key] = ""
                continue
            try:
                txt[key] = v.decode("utf-8") if isinstance(v, bytes) else str(v)
            except UnicodeDecodeError:
                txt[key] = repr(v)

        self.found[name] = {
            "instance_name": instance,
            "hostname": info.server.rstrip(".") if info.server else f"{instance}.local",
            "addresses": addresses,
            "port": info.port,
            "txt": txt,
            "service_type": type_,
            "discovered_at": time.time(),
            "transport": txt.get("transport", "wifi_tls"),
        }

    def update_service(self, zc, type_, name):
        # mDNS records may refresh; re-resolve to pick up new TXT.
        self.add_service(zc, type_, name)

    def remove_service(self, zc, type_, name):
        self.found.pop(name, None)


def discover(timeout_s: float = 3.0,
             once: bool = False,
             name_filter: str | None = None) -> list[dict[str, Any]]:
    """Run a discovery scan; return list of device dicts."""
    if not _zeroconf_or_die():
        return []
    from zeroconf import Zeroconf, ServiceBrowser

    zc = Zeroconf()
    listener = _DiscoveryListener(name_filter=name_filter)
    browser = ServiceBrowser(zc, SERVICE_TYPE, listener)
    deadline = time.time() + timeout_s
    try:
        while time.time() < deadline:
            time.sleep(0.1)
            if once and listener.found:
                break
    finally:
        browser.cancel()
        zc.close()
    return list(listener.found.values())


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="discover.py",
        description="Find esp32-agent-dashboard devices via mDNS.",
    )
    p.add_argument("--timeout", type=float, default=3.0,
                   help="seconds to scan (default 3.0)")
    p.add_argument("--once", action="store_true",
                   help="exit after the first device is found")
    p.add_argument("--filter", dest="name_filter", default=None,
                   help="case-insensitive substring match on instance name")
    p.add_argument("--pretty", action="store_true",
                   help="pretty-print JSON")
    args = p.parse_args(argv)

    devices = discover(
        timeout_s=args.timeout,
        once=args.once,
        name_filter=args.name_filter,
    )
    indent = 2 if args.pretty else None
    json.dump(devices, sys.stdout, indent=indent, sort_keys=True)
    sys.stdout.write("\n")
    return 0 if devices else 1


if __name__ == "__main__":
    sys.exit(main())
