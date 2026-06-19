"""一致性测试:宿主原生编译的固件数据层,经 ctypes 喂 dash 命令、读 state_json。
不需要 emsdk / pyserial。运行:python tools/web/test_wasm_datalayer.py"""
import ctypes
import json
import platform
import subprocess
import sys
from pathlib import Path

if platform.system() == "Windows":
    import ctypes.wintypes  # noqa: F401 — loads the submodule so ctypes.wintypes is usable

HERE = Path(__file__).resolve().parent
WASM = HERE / "wasm"
BUILD = WASM / "build"


def _lib_path() -> Path:
    ext = {"Windows": "dll", "Darwin": "dylib"}.get(platform.system(), "so")
    return BUILD / f"libdash_datalayer.{ext}"


# Windows: track both the lib object and its raw handle so we can release the DLL
# before a rebuild — the linker cannot overwrite a locked .dll file.
_loaded_lib = None    # previous ctypes.CDLL object (held to prevent GC before FreeLibrary)
_loaded_handle = None  # corresponding HANDLE value

def load_lib() -> ctypes.CDLL:
    global _loaded_lib, _loaded_handle
    lib_path = _lib_path()
    # Windows: release the previously loaded DLL before rebuilding so the linker
    # can overwrite the locked file.  Use proper HANDLE/BOOL types to avoid
    # sign-extension of high-bit addresses on 64-bit Windows.
    if platform.system() == "Windows" and _loaded_handle is not None:
        _free = ctypes.windll.kernel32.FreeLibrary
        _free.argtypes = [ctypes.wintypes.HANDLE]
        _free.restype  = ctypes.wintypes.BOOL
        # Drop the Python reference first so the object can be GC'd after release
        _loaded_lib = None
        if not _free(_loaded_handle):
            raise OSError("FreeLibrary failed; .dll still locked")
        _loaded_handle = None
    # Unconditional rebuild — ensures stale .dll/.so never produces false green/red
    subprocess.run(["bash", str(WASM / "build_native.sh")], check=True)
    lib = ctypes.CDLL(str(lib_path))
    if platform.system() == "Windows":
        _loaded_lib    = lib          # keep reference alive until next FreeLibrary
        _loaded_handle = lib._handle  # raw HANDLE for FreeLibrary
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


def _decl_feed(lib):
    lib.dash_feed_line.argtypes = [ctypes.c_char_p]
    lib.dash_feed_line.restype = ctypes.c_int
    lib.last_reply.restype = ctypes.c_char_p
    lib.last_reply_is_err.restype = ctypes.c_int
    lib.current_scene.restype = ctypes.c_char_p

def feed(lib, line: str) -> int:
    return lib.dash_feed_line(line.encode("utf-8"))

def test_dash_idle():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    rc = feed(lib, 'dash idle')
    assert rc == 0, rc
    assert lib.current_scene().decode() == "idle", lib.current_scene()
    assert b'"scene":"idle"' in lib.last_reply(), lib.last_reply()
    assert lib.last_reply_is_err() == 0, lib.last_reply_is_err()
    print("ok test_dash_idle")


if __name__ == "__main__":
    test_empty_state()
    test_dash_idle()
    print("ALL PASS")
