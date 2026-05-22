#!/usr/bin/env python3
"""ble_smoke.py — BLE NUS smoke test for esp32-agent-dashboard.

Connects to a BLE-advertised dashboard device, sends a v2 `dash hello`,
and asserts the device reply matches the v2 negotiation contract from
`docs/PROTOCOL_v2.md`.

This is a HOST-SIDE script — runs on a laptop / desktop with a BLE
adapter. Skipped in CI if no adapter is present (see _has_ble_adapter).

v0.4.0 scaffold (TRANS1). The firmware BLE NUS path is stubbed pending
F2's build integration. Once F2 lands the GATT registration this script
becomes the end-to-end gate.

Usage:
    python ble_smoke.py                     # scan + first agentdash-* found
    python ble_smoke.py --name Clawd        # match by instance name
    python ble_smoke.py --address AA:BB:CC:DD:EE:FF   # direct connect
    python ble_smoke.py --timeout 10        # scan timeout (default 5s)
    python ble_smoke.py --json              # JSON output (one line)
    python ble_smoke.py --skip-if-no-adapter

Exit codes:
    0  smoke passed (or skipped via --skip-if-no-adapter)
    1  device not found
    2  hello reply missing / malformed
    3  hello reply OK but `negotiated` not v2
    4  no BLE adapter (and --skip-if-no-adapter not set)
    5  bleak not installed

Dependencies:
    pip install bleak    (BlueZ on Linux / CoreBluetooth on macOS /
                          WinRT on Windows; works on all three)
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
import time
from typing import Any

# Nordic UART Service UUIDs (same as the firmware advertises).
NUS_SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_CHAR  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host writes here
NUS_TX_CHAR  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device notifies here

DEFAULT_INSTANCE_PREFIX = "agentdash-"


def _has_ble_adapter() -> bool:
    """Best-effort check for a usable BLE adapter on this host.

    bleak doesn't expose this cleanly per-platform; we treat the import
    as the adapter check and rely on the discovery call to fail-fast if
    nothing is actually attached.
    """
    try:
        import bleak  # noqa: F401
        return True
    except ImportError:
        return False


def _hello_payload(transport: str = "ble_nus") -> dict[str, Any]:
    return {
        "transport": transport,
        "protocol_version": "v2",
        "capabilities": [
            "LINES", "EVT_STREAM", "CONFIG_NVS",
            "MTU_HINT", "FARE_WELL", "LINK_QUALITY",
        ],
        "bridge": "ble_smoke.py/v0.4.0",
    }


def _validate_hello_reply(line: str) -> tuple[int, str, dict[str, Any] | None]:
    """Parse the OK: reply to a hello. Returns (exit_code, msg, parsed_obj_or_None)."""
    if not line.startswith("OK:"):
        return 2, f"reply did not start with OK: ({line!r})", None
    body = line[len("OK:"):].strip()
    try:
        obj = json.loads(body)
    except json.JSONDecodeError as e:
        return 2, f"reply body not JSON: {e}; line={line!r}", None
    negotiated = obj.get("negotiated")
    if negotiated != "v2":
        return 3, f"negotiated != v2 (got {negotiated!r})", obj
    if "v2" not in obj.get("compat", []):
        return 3, "compat array missing v2", obj
    if not obj.get("device_capabilities"):
        return 2, "device_capabilities missing or empty", obj
    return 0, "ok", obj


async def _smoke(name_match: str | None,
                 address: str | None,
                 scan_timeout: float,
                 read_timeout: float) -> tuple[int, dict[str, Any]]:
    from bleak import BleakClient, BleakScanner

    target_address = address
    discovered: dict[str, Any] = {}

    if not target_address:
        # Discovery phase
        devices = await BleakScanner.discover(timeout=scan_timeout)
        candidates = []
        for d in devices:
            name = d.name or ""
            if name_match:
                if name_match.lower() in name.lower():
                    candidates.append(d)
            elif name.startswith(DEFAULT_INSTANCE_PREFIX) or name.lower().startswith("agentdash"):
                candidates.append(d)
        if not candidates:
            return 1, {
                "error": "no matching BLE device found",
                "scanned": [
                    {"name": d.name, "address": d.address}
                    for d in devices
                ],
            }
        target = candidates[0]
        target_address = target.address
        discovered["matched"] = {"name": target.name, "address": target.address}

    # Connect + hello round-trip
    rx_lines: list[str] = []
    rx_buf = bytearray()
    done = asyncio.Event()

    def on_notify(_handle, data: bytearray):
        rx_buf.extend(data)
        while b"\n" in rx_buf:
            idx = rx_buf.index(b"\n")
            line = bytes(rx_buf[:idx]).decode("utf-8", errors="replace").rstrip("\r")
            del rx_buf[:idx + 1]
            if line:
                rx_lines.append(line)
                if line.startswith("OK:") or line.startswith("ERR:"):
                    done.set()

    async with BleakClient(target_address) as client:
        discovered["connected"] = True
        discovered["mtu"] = getattr(client, "mtu_size", None)
        await client.start_notify(NUS_TX_CHAR, on_notify)

        payload = _hello_payload(transport="ble_nus")
        line = f'dash hello {json.dumps(payload, separators=(",", ":"))}\n'
        raw = line.encode("utf-8")
        # MTU-fragment if needed.
        mtu = getattr(client, "mtu_size", 247) - 3
        if mtu < 20:
            mtu = 20
        for i in range(0, len(raw), mtu):
            await client.write_gatt_char(NUS_RX_CHAR, raw[i:i + mtu], response=False)

        try:
            await asyncio.wait_for(done.wait(), timeout=read_timeout)
        except asyncio.TimeoutError:
            return 2, {**discovered,
                       "error": "timed out waiting for reply",
                       "received": rx_lines}

        await client.stop_notify(NUS_TX_CHAR)

    # Pick the first OK:/ERR: line and validate
    reply_line = next((l for l in rx_lines if l.startswith(("OK:", "ERR:"))), None)
    if reply_line is None:
        return 2, {**discovered, "error": "no OK/ERR reply",
                   "received": rx_lines}
    code, msg, parsed = _validate_hello_reply(reply_line)
    discovered.update({
        "reply_line": reply_line,
        "reply_parsed": parsed,
        "validation": {"code": code, "msg": msg},
        "all_lines": rx_lines,
    })
    return code, discovered


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ble_smoke.py")
    p.add_argument("--name", help="match BLE local-name substring (case-insensitive)")
    p.add_argument("--address", help="connect directly to this BLE address (skips scan)")
    p.add_argument("--timeout", type=float, default=5.0,
                   help="scan timeout seconds (default 5)")
    p.add_argument("--read-timeout", type=float, default=5.0,
                   help="seconds to wait for hello reply (default 5)")
    p.add_argument("--json", action="store_true",
                   help="emit single-line JSON result instead of human text")
    p.add_argument("--skip-if-no-adapter", action="store_true",
                   help="exit 0 with skip message if bleak isn't installed")
    args = p.parse_args(argv)

    if not _has_ble_adapter():
        if args.skip_if_no_adapter:
            msg = {"skipped": True, "reason": "bleak not installed / no BLE adapter"}
            if args.json:
                print(json.dumps(msg))
            else:
                print("ble_smoke: SKIPPED (no BLE adapter / bleak missing)")
            return 0
        print("ERROR: bleak not installed. pip install bleak", file=sys.stderr)
        return 5

    started = time.time()
    try:
        code, result = asyncio.run(_smoke(
            name_match=args.name,
            address=args.address,
            scan_timeout=args.timeout,
            read_timeout=args.read_timeout,
        ))
    except Exception as e:
        code, result = 2, {"error": f"exception: {type(e).__name__}: {e}"}

    result["elapsed_s"] = round(time.time() - started, 3)
    result["exit_code"] = code

    if args.json:
        print(json.dumps(result, default=str))
    else:
        verdict = {0: "OK", 1: "NOT_FOUND", 2: "BAD_REPLY",
                   3: "WRONG_VERSION"}.get(code, f"FAIL({code})")
        print(f"ble_smoke: {verdict} ({result.get('elapsed_s')}s)")
        if code != 0:
            for k, v in result.items():
                print(f"  {k}: {v}")
        else:
            parsed = result.get("reply_parsed") or {}
            print(f"  device:    {parsed.get('device_name','?')}  fw={parsed.get('fw','?')}")
            print(f"  negotiated:{parsed.get('negotiated')}  caps={parsed.get('capability_bitmap')}")
    return code


if __name__ == "__main__":
    sys.exit(main())
