"""一致性测试:宿主原生编译的固件数据层,经 ctypes 喂 dash 命令、读 state_json。
不需要 emsdk / pyserial。运行:python tools/web/test_wasm_datalayer.py"""
import ctypes
import json
import platform
import subprocess
import sys
from pathlib import Path

AGENT_MSG_MAX = 128          # verbatim from main/agent_state.h
AGENT_SLOT_MAX = 4           # verbatim from main/agent_state.h

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
        handle = _loaded_handle
        # Release the DLL first; clear references only after successful release
        # to avoid a double-FreeLibrary if the call raises.
        if not _free(handle):
            raise OSError("FreeLibrary failed; .dll still locked")
        _loaded_handle = None
        _loaded_lib = None
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
    lib.drain_signals.restype = ctypes.c_char_p

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


def test_snapshot_two_agents():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    snap = (
        '{"agents":['
        '{"kind":"claude-code","session_id":"cc_abc","status":"running",'
        '"cwd":"D:\\\\Code\\\\x","msg":"editing main.c","tokens":84502,"tokens_today":21200},'
        '{"kind":"codex","session_id":"cx_xyz","status":"idle",'
        '"cwd":"D:\\\\Code\\\\y","msg":"(stop)","tokens":12300,"tokens_today":12300}'
        '],"totals":{"total":2,"running":1,"waiting":0,"tokens":96802,"tokens_today":33500}}'
    )
    rc = feed(lib, 'dash snapshot "' + snap + '"')
    assert rc == 0, rc
    s = state(lib)
    assert s["totals"]["total"] == 2, s
    assert len(s["slots"]) == 2, s
    kinds = {sl["kind"] for sl in s["slots"]}
    assert kinds == {"claude-code", "codex"}, kinds
    cc = next(sl for sl in s["slots"] if sl["kind"] == "claude-code")
    assert cc["session_id"] == "cc_abc", cc
    assert cc["status"] == "running", cc
    assert cc["msg"] == "editing main.c", cc
    assert cc["tokens"] == 84502, cc
    print("ok test_snapshot_two_agents")


def test_msg_truncation():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    long_msg = "x" * 300
    snap = ('{"agents":[{"kind":"claude-code","session_id":"cc1",'
            '"status":"running","msg":"' + long_msg + '"}],'
            '"totals":{"total":1,"running":1,"waiting":0}}')
    rc = feed(lib, 'dash snapshot "' + snap + '"')
    assert rc == 0, rc
    s = state(lib)
    msg = s["slots"][0]["msg"]
    assert len(msg) == AGENT_MSG_MAX - 1, (len(msg), AGENT_MSG_MAX)
    assert set(msg) == {"x"}, "truncated content should be all x"
    print("ok test_msg_truncation")


def test_slot_overflow():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    agents = ",".join(
        '{"kind":"other","session_id":"s%d","status":"running","msg":"m%d"}' % (i, i)
        for i in range(5)
    )
    snap = '{"agents":[' + agents + '],"totals":{"total":5,"running":5,"waiting":0}}'
    rc = feed(lib, 'dash snapshot "' + snap + '"')
    assert rc == 0, rc
    s = state(lib)
    assert len(s["slots"]) == AGENT_SLOT_MAX, len(s["slots"])
    assert b'"dropped":1' in lib.last_reply(), lib.last_reply()
    print("ok test_slot_overflow")


def test_signals():
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    snap = ('{"agents":[{"kind":"codex","session_id":"cx1",'
            '"status":"running","msg":"go"}],'
            '"totals":{"total":1,"running":1,"waiting":0}}')
    feed(lib, 'dash snapshot "' + snap + '"')
    sigs = json.loads(lib.drain_signals().decode())
    # shim 的 drain_signals 返回 JSON 字符串数组,故用子串检查("agent_added" in x);
    # 若将来 shim 改为对象数组,需将此处断言改为访问对象字段。
    assert any("agent_added" in x and "codex" in x for x in sigs), sigs
    # drain 清空
    assert json.loads(lib.drain_signals().decode()) == [], "signals should clear after drain"
    prompt = '{"id":"req1","tool":"Bash"}'
    feed(lib, 'dash prompt "' + prompt + '"')
    sigs = json.loads(lib.drain_signals().decode())
    assert any("scene_changed" in x and "prompt" in x for x in sigs), sigs
    print("ok test_signals")


def test_escaped_quote_snapshot_roundtrip():
    """End-to-end regression for the `\\" ` tokeniser bug: a snapshot whose
    value contains an escaped quote followed by whitespace must parse (not be
    rejected as malformed) and the quote must survive into state_json."""
    lib = load_lib()
    _decl_feed(lib)
    lib.dash_init()
    # msg has an escaped quote then a space — exactly what truncated the token.
    payload = {"agents": [{"kind": "claude-code", "session_id": "q1",
                           "status": "running", "msg": '$ echo "hi" world'}],
               "totals": {"total": 1, "running": 1, "waiting": 0}}
    feed(lib, 'dash snapshot "' + json.dumps(payload, separators=(",", ":")) + '"')
    s = state(lib)
    slots = s.get("slots", [])
    assert len(slots) == 1, s
    assert slots[0]["msg"] == '$ echo "hi" world', slots[0]
    print("ok test_escaped_quote_snapshot_roundtrip")


def test_initial_scene():
    lib = load_lib()
    _decl_feed(lib)            # 声明 current_scene 等
    lib.dash_init()
    assert lib.current_scene().decode() == "dashboard", lib.current_scene()
    print("ok test_initial_scene")


def test_tokenise_pathological():
    lib = load_lib()
    lib.g7_tokenise_join.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
    lib.g7_tokenise_join.restype = ctypes.c_int

    def tok(line: str):
        out = ctypes.create_string_buffer(1024)
        argc = lib.g7_tokenise_join(line.encode("utf-8"), out, 1024)
        parts = out.value.decode("utf-8").split("\x1f") if argc > 0 else []
        return argc, parts

    # 引号起始 token:闭合引号是「后跟空白/行尾」的那个,内层引号(后跟非空白)不收尾
    assert tok('dash snapshot "{"a":1}"') == (3, ["dash", "snapshot", '{"a":1}']), tok('dash snapshot "{"a":1}"')
    # 反斜杠感知:转义引号 \" 不收尾,即便其后是空格(否则带引号文本会截断快照)。
    # 回归 `$ echo "hi" world`-类内容导致 dash snapshot 被判 malformed JSON 的 bug。
    assert tok('dash snapshot "{"m":"a \\" b"}"') == (3, ["dash", "snapshot", '{"m":"a \\" b"}']), \
        tok('dash snapshot "{"m":"a \\" b"}"')
    # 普通命令
    assert tok("dash idle") == (2, ["dash", "idle"])
    # 非引号起始含引号:legacy 模式剥除所有引号
    assert tok('foo"bar"baz') == (1, ["foobarbaz"]), tok('foo"bar"baz')
    # 引号起始未闭合:取到行尾
    assert tok('"abc') == (1, ["abc"]), tok('"abc')
    # 引号内含空格保留
    assert tok('"a b c"') == (1, ["a b c"]), tok('"a b c"')
    print("ok test_tokenise_pathological")


if __name__ == "__main__":
    test_empty_state()
    test_dash_idle()
    test_snapshot_two_agents()
    test_msg_truncation()
    test_slot_overflow()
    test_signals()
    test_escaped_quote_snapshot_roundtrip()
    test_initial_scene()
    test_tokenise_pathological()
    print("ALL PASS")
