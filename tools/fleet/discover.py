"""tools/fleet/discover.py — enumerate devices for the v1.2.0 fleet.

Combines three discovery sources into one JSON list:

  1. local COM ports speaking the dash protocol
  2. `_aagentdash._tcp` mDNS browse
  3. cached `~/.claude-buddy/known_devices.json`

Stub for v1.2.0. The real implementation extends esp_harness.core.ports
+ tools/transport/discover.py. For now we just emit what we can find
on the local machine.

Usage::

    python tools/fleet/discover.py
    python tools/fleet/discover.py --timeout 5 --json
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

CACHE_PATH = Path.home() / ".claude-buddy" / "known_devices.json"


def discover_local_serial() -> list[dict]:
    """Best-effort COM-port scan via pyserial.list_ports."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    devices = []
    for p in list_ports.comports():
        devices.append({
            "device_id": None,        # only known after `dash hello` reply
            "transport": "serial",
            "address": p.device,
            "description": p.description,
            "last_seen_unix": int(time.time()),
            "source": "list_ports",
        })
    return devices


def discover_mdns(timeout_s: float = 3.0) -> list[dict]:
    """Browse `_aagentdash._tcp` via zeroconf. Returns [] if zeroconf
    isn't available or no devices respond in the timeout window."""
    try:
        from zeroconf import Zeroconf, ServiceBrowser
    except ImportError:
        return []
    found = []
    class _Listener:
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name)
            if info:
                found.append({
                    "device_id": name.split(".")[0],
                    "transport": "wifi_tls",
                    "address": f"{info.parsed_addresses()[0]}:{info.port}",
                    "last_seen_unix": int(time.time()),
                    "source": "mdns",
                })
        def remove_service(self, *_): pass
        def update_service(self, *_): pass
    zc = Zeroconf()
    try:
        ServiceBrowser(zc, "_aagentdash._tcp.local.", _Listener())
        time.sleep(timeout_s)
    finally:
        zc.close()
    return found


def load_cache() -> list[dict]:
    if not CACHE_PATH.exists():
        return []
    try:
        return json.loads(CACHE_PATH.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return []


def save_cache(devices: list[dict]) -> None:
    CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
    CACHE_PATH.write_text(json.dumps(devices, indent=2), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--timeout", type=float, default=3.0,
                    help="mDNS browse timeout in seconds (default 3.0)")
    ap.add_argument("--json", action="store_true",
                    help="machine-readable JSON output")
    ap.add_argument("--no-cache", action="store_true",
                    help="skip persisted cache; live scan only")
    ap.add_argument("--no-mdns", action="store_true",
                    help="skip mDNS browse")
    ap.add_argument("--no-serial", action="store_true",
                    help="skip COM-port scan")
    args = ap.parse_args(argv)

    devices: list[dict] = []
    if not args.no_serial:
        devices.extend(discover_local_serial())
    if not args.no_mdns:
        devices.extend(discover_mdns(args.timeout))
    if not args.no_cache:
        cached = load_cache()
        # Merge: live wins on (device_id, transport) collision
        seen = {(d.get("device_id"), d.get("transport")) for d in devices}
        for c in cached:
            if (c.get("device_id"), c.get("transport")) not in seen:
                devices.append(c)

    if not args.no_cache and devices:
        save_cache(devices)

    if args.json:
        print(json.dumps({"devices": devices}, indent=2))
    else:
        if not devices:
            print("no devices found", file=sys.stderr)
            return 1
        for d in devices:
            print(f"  {d.get('transport',''):8} {d.get('address',''):30} "
                  f"{d.get('device_id','-') or '-':20} {d.get('source','')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
