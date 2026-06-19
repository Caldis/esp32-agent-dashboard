"""一致性测试:宿主原生编译的固件数据层,经 ctypes 喂 dash 命令、读 state_json。
不需要 emsdk / pyserial。运行:python tools/web/test_wasm_datalayer.py"""
import ctypes
import json
import platform
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WASM = HERE / "wasm"
BUILD = WASM / "build"


def _lib_path() -> Path:
    ext = {"Windows": "dll", "Darwin": "dylib"}.get(platform.system(), "so")
    return BUILD / f"libdash_datalayer.{ext}"


def load_lib() -> ctypes.CDLL:
    lib_path = _lib_path()
    if not lib_path.exists():
        subprocess.run(["bash", str(WASM / "build_native.sh")], check=True)
    lib = ctypes.CDLL(str(lib_path))
    lib.dash_init.restype = None
    lib.state_json.restype = ctypes.c_char_p
    return lib


def state(lib) -> dict:
    return json.loads(lib.state_json().decode("utf-8"))


def test_empty_state():
    lib = load_lib()
    lib.dash_init()
    s = state(lib)
    assert s["device_name"] == "DASHBOARD", s
    assert s["totals"]["total"] == 0, s
    assert s["slots"] == [], s
    assert s["prompt"]["active"] is False, s
    print("ok test_empty_state")


if __name__ == "__main__":
    test_empty_state()
    print("ALL PASS")
