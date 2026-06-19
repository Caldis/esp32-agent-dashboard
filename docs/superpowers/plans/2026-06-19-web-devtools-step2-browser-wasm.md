# Web Dev Tools — 第 2 步:浏览器 WASM(同源数据层进浏览器)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 Step 1 已验证的固件同源数据层用 emcc 编成浏览器 `.wasm`,并在浏览器里加载、feed 一条样本 dash 命令、读出 `state_json` 渲染到页面,证明"同源数据层"在浏览器运行时同样跑通;同时把 G-7 tokeniser 抽成 native/wasm 共用的单一 C 单元,消除两份实现漂移风险。

**Architecture:** 先把 `wasm_api.c` 里的 `tokenise` 抽到独立 `g7_tokenise.c/.h`,native 与 wasm 两条构建都编译同一份(单一真相)。`build_wasm.sh` 用 emcc(EXPORT_ES6+MODULARIZE)把 `g7_tokenise.c` + 四个固件数据层 `.c` + `wasm_shim.c` + `wasm_api.c` 编成 `tools/web/static/dash_datalayer.mjs`(+ `.wasm`)。一个 node ESM 测试加载该模块、调 `dash_init/dash_feed_line/state_json` 做断言(自动验证,无需浏览器)。最小 `index.html`+`app.js` 在浏览器里做同样的事并把结果渲染到页面。

**Tech Stack:** C11、emcc(emscripten,EXPORT_ES6/MODULARIZE)、node ≥18 ESM、Python `http.server`(静态托管)、Step 1 的 native cc 链路(回归)。

## Global Constraints

- **不修改 `main/` 下任何固件 C 源**(只引用)。如发现必须改,停下来上报。
- **不引入第三方运行时依赖**:测试用 node 内置 `node:assert` + Step 1 的标准库 ctypes;构建用 emcc。
- **emsdk/emcc 是 Task 2、3 的前置**(`emcc` 在 PATH);**Task 1 只需 native cc**(Step 1 已具备),可在 emsdk 就绪前执行。
- C 源**引用而非复制**;tokeniser 抽取后 native 与 wasm 共用同一份 `g7_tokenise.c`。
- 导出函数集(verbatim,来自 `tools/web/wasm/wasm_api.c`):`dash_init`、`dash_feed_line`、`state_json`、`current_scene`、`last_reply`、`last_reply_is_err`、`drain_signals`。
- 生成产物 `tools/web/static/dash_datalayer.mjs` + `dash_datalayer.wasm` **不入库**(加 `.gitignore`);`index.html`、`app.js` 入库。
- 字段上限以 `main/agent_state.h` 为准(verbatim);常量 `CONSOLE_MAX_ARGS=8`、`CONSOLE_MAX_LINE=1024`。

---

### Task 1: 抽取 G-7 tokeniser 为共享 C 单元 + 病态引号一致性测试

**Files:**
- Create: `tools/web/wasm/g7_tokenise.h`
- Create: `tools/web/wasm/g7_tokenise.c`
- Modify: `tools/web/wasm/wasm_api.c`(删除 static `tokenise`,改用 `g7_tokenise`)
- Modify: `tools/web/wasm/build_native.sh`(编译加入 `g7_tokenise.c`)
- Modify: `tools/web/test_wasm_datalayer.py`(加 `test_tokenise_pathological`)

**Interfaces:**
- Consumes: Step 1 的 `console_args_t`、`shim_find_cmd`、`dash_feed_line` 现有结构。
- Produces:
  - `int g7_tokenise(const char *line, char *buf, size_t bufcap, const char *argv[], int max_args)` — 与 Step 1 `tokenise` 逐字相同的实现(去 static、改名),native 与 wasm 共用。
  - `int g7_tokenise_join(const char *line, char *out, size_t outcap)` — 测试友好封装:tokenise 后把各 token 用 `\x1f` 连接写入 `out`,返回 argc;供 ctypes 直接断言切分结果。

- [ ] **Step 1: 创建 `g7_tokenise.h`**

`tools/web/wasm/g7_tokenise.h`:
```c
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* G-7 tokeniser:把一行切成 argv(各 token 以 NUL 分隔写入 buf)。
 * 返回 argc(≤ max_args)。与固件 console_protocol.c /
 * mock_device_v1.py._tokenise 语义等价:
 *  - '"' 起始 token:去前导 '"',累积到「后跟空白或行尾」的 '"' 收尾;
 *  - 非 '"' 起始 token:遇任意 '"' 切换 in_quote,所有 '"' 被剥除。 */
int g7_tokenise(const char *line, char *buf, size_t bufcap,
                const char *argv[], int max_args);

/* 测试友好封装:tokenise 后把各 token 用 '\x1f'(US)连接写入 out,
 * 返回 argc。便于 ctypes 直接断言切分结果。out 始终 NUL 终止。 */
int g7_tokenise_join(const char *line, char *out, size_t outcap);
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: 创建 `g7_tokenise.c`(移动现有逻辑 + 测试封装)**

把 `tools/web/wasm/wasm_api.c` 中现有的 `static int tokenise(...)` 函数体**逐字移动**到此文件并改名 `g7_tokenise`(去掉 `static`,签名不变),再加 `g7_tokenise_join`。

`tools/web/wasm/g7_tokenise.c`:
```c
#include "g7_tokenise.h"
#include <string.h>

int g7_tokenise(const char *line, char *buf, size_t bufcap,
                const char *argv[], int max_args) {
    int argc = 0; size_t w = 0; size_t n = strlen(line); size_t i = 0;
    while (i < n && argc < max_args) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= n) break;
        if (w >= bufcap) break;
        argv[argc] = &buf[w];
        if (line[i] == '"') {
            i++;                                  /* drop leading " */
            int close = -1;
            for (size_t j = i; j < n; ++j) {
                if (line[j] == '"' && (j + 1 == n || line[j+1] == ' ' || line[j+1] == '\t')) {
                    close = (int)j; break;
                }
            }
            size_t endp = (close == -1) ? n : (size_t)close;
            for (size_t j = i; j < endp && w + 1 < bufcap; ++j) buf[w++] = line[j];
            i = (close == -1) ? n : (size_t)close + 1;
        } else {
            int in_q = 0;
            while (i < n) {
                char ch = line[i];
                if (!in_q && (ch == ' ' || ch == '\t')) break;
                if (ch == '"') { in_q = !in_q; i++; continue; }
                if (w + 1 < bufcap) buf[w++] = ch;
                i++;
            }
        }
        if (w < bufcap) buf[w++] = 0;             /* NUL-terminate token */
        argc++;
    }
    return argc;
}

int g7_tokenise_join(const char *line, char *out, size_t outcap) {
    char buf[1024];
    const char *argv[8];
    int argc = g7_tokenise(line, buf, sizeof(buf), argv, 8);
    size_t w = 0;
    for (int k = 0; k < argc; ++k) {
        if (k > 0 && w + 1 < outcap) out[w++] = '\x1f';
        for (const char *p = argv[k]; *p && w + 1 < outcap; ++p) out[w++] = *p;
    }
    if (outcap) out[(w < outcap) ? w : (outcap - 1)] = '\0';
    return argc;
}
```

- [ ] **Step 3: 改 `wasm_api.c` 用 `g7_tokenise`**

在 `tools/web/wasm/wasm_api.c`:
1. 顶部 include 区加:`#include "g7_tokenise.h"`
2. **删除**整个 `static int tokenise(...) { ... }` 函数(连同其上方注释块,即现 20–58 行)。
3. `dash_feed_line` 内把 `tokenise(...)` 调用改为 `g7_tokenise(...)`(仅函数名变,实参不变):
```c
    args.argc = g7_tokenise(line, buf, sizeof(buf), argv, CONSOLE_MAX_ARGS);
```

- [ ] **Step 4: 改 `build_native.sh` 编译加入 `g7_tokenise.c`**

在 `tools/web/wasm/build_native.sh` 的 `cc` 编译命令源文件列表中,于 `"$HERE/wasm_shim.c"` 之前加入一行:
```bash
  "$HERE/g7_tokenise.c" \
```
(其余不变。)

- [ ] **Step 5: 写病态引号一致性测试(失败先行)**

在 `tools/web/test_wasm_datalayer.py` 增加(并把 `test_tokenise_pathological()` 加进 `__main__`,在 `ALL PASS` 前):
```python
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
    # 普通命令
    assert tok("dash idle") == (2, ["dash", "idle"])
    # 非引号起始含引号:legacy 模式剥除所有引号
    assert tok('foo"bar"baz') == (1, ["foobarbaz"]), tok('foo"bar"baz')
    # 引号起始未闭合:取到行尾
    assert tok('"abc') == (1, ["abc"]), tok('"abc')
    # 引号内含空格保留
    assert tok('"a b c"') == (1, ["a b c"]), tok('"a b c"')
    print("ok test_tokenise_pathological")
```

- [ ] **Step 6: 运行确认通过(且不回归)**

Run: `python tools/web/test_wasm_datalayer.py`
Expected: PASS —— 含 `ok test_tokenise_pathological`,且既有 6 个测试不回归(`ALL PASS`)。
说明:`load_lib` 每次重建会把新加的 `g7_tokenise.c` 编进库;若 `g7_tokenise_join` 符号找不到,检查 build_native.sh 是否已加入该源文件。若某条切分断言与实际不符,以固件 `console_protocol.c` / `mock_device_v1.py._tokenise` 的真实语义为准核对(这是 pin 行为的探针),不要改 `g7_tokenise` 逻辑迁就断言——除非确认 Step 1 移动时引入了偏差。

- [ ] **Step 7: Commit**

```bash
git add tools/web/wasm/g7_tokenise.h tools/web/wasm/g7_tokenise.c tools/web/wasm/wasm_api.c tools/web/wasm/build_native.sh tools/web/test_wasm_datalayer.py
git commit -m "refactor(web/wasm): extract shared G-7 tokeniser + pathological-quote parity test"
```

---

### Task 2: `build_wasm.sh`(emcc)+ node 验证 wasm 模块  ⚠️ 需 emsdk/emcc

**Files:**
- Create: `tools/web/wasm/build_wasm.sh`
- Create: `tools/web/test_wasm_node.mjs`
- Modify: `.gitignore`(忽略生成的 `tools/web/static/dash_datalayer.{mjs,wasm}`)

**Interfaces:**
- Consumes: Task 1 后的 C 源集(`g7_tokenise.c` + 四个数据层 `.c` + `wasm_shim.c` + `wasm_api.c`);导出函数集(见 Global Constraints)。
- Produces: `tools/web/static/dash_datalayer.mjs`(ES6 工厂,`EXPORT_NAME=DashDataLayer`)+ `dash_datalayer.wasm`,导出上述 7 个 C 函数及 `ccall/cwrap/UTF8ToString` 运行时方法,`ENVIRONMENT=web,node`。

- [ ] **Step 1: 创建 `build_wasm.sh`**

`tools/web/wasm/build_wasm.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$ROOT/tools/web/static"
mkdir -p "$OUT"

command -v emcc >/dev/null 2>&1 || {
  echo "ERROR: emcc not found. Install + activate emsdk first:" >&2
  echo "  git clone https://github.com/emscripten-core/emsdk && ./emsdk/emsdk install latest && ./emsdk/emsdk activate latest" >&2
  echo "  source ./emsdk/emsdk_env.sh   (or emsdk_env.bat on cmd)" >&2
  exit 1
}

emcc -O2 \
  -I "$HERE/shim_include" -I "$ROOT/main" -I "$ROOT/main/harness" \
  "$HERE/g7_tokenise.c" \
  "$ROOT/main/agent_state.c" \
  "$ROOT/main/tiny_json.c" \
  "$ROOT/main/harness/agent_snapshot_apply.c" \
  "$ROOT/main/harness/agent_commands.c" \
  "$HERE/wasm_shim.c" \
  "$HERE/wasm_api.c" \
  -o "$OUT/dash_datalayer.mjs" \
  -sEXPORT_ES6=1 -sMODULARIZE=1 -sEXPORT_NAME=DashDataLayer \
  -sEXPORTED_FUNCTIONS=_dash_init,_dash_feed_line,_state_json,_current_scene,_last_reply,_last_reply_is_err,_drain_signals,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sENVIRONMENT=web,node

echo "built $OUT/dash_datalayer.mjs (+ dash_datalayer.wasm)"
```

- [ ] **Step 2: 创建 node 验证测试**

`tools/web/test_wasm_node.mjs`:
```js
// node ESM smoke: 加载 emcc 出的 wasm 数据层,验证 JS 运行时下行为一致。
// 运行:bash tools/web/wasm/build_wasm.sh && node tools/web/test_wasm_node.mjs
import assert from 'node:assert';
import DashDataLayer from './static/dash_datalayer.mjs';

const M = await DashDataLayer();
const dash_init  = M.cwrap('dash_init', null, []);
const dash_feed  = M.cwrap('dash_feed_line', 'number', ['string']);
const state_json = M.cwrap('state_json', 'string', []);

dash_init();
let s = JSON.parse(state_json());
assert.strictEqual(s.device_name, 'DASHBOARD', 'default device_name');
assert.strictEqual(s.totals.total, 0, 'empty totals');
assert.deepStrictEqual(s.slots, [], 'empty slots');

const snap = '{"agents":[{"kind":"codex","session_id":"cx1","status":"running",'
           + '"msg":"go","tokens":42}],"totals":{"total":1,"running":1,"waiting":0}}';
const rc = dash_feed('dash snapshot "' + snap + '"');
assert.strictEqual(rc, 0, 'feed rc');
s = JSON.parse(state_json());
assert.strictEqual(s.totals.total, 1, 'one agent');
assert.strictEqual(s.slots.length, 1, 'one slot');
assert.strictEqual(s.slots[0].kind, 'codex', 'kind');
assert.strictEqual(s.slots[0].tokens, 42, 'tokens');

console.log('wasm node smoke: ALL PASS');
```

- [ ] **Step 3: 忽略生成产物**

在 `.gitignore` 末尾追加:
```
tools/web/static/dash_datalayer.mjs
tools/web/static/dash_datalayer.wasm
```

- [ ] **Step 4: 构建并运行 node 验证**

Run:
```bash
bash tools/web/wasm/build_wasm.sh && node tools/web/test_wasm_node.mjs
```
Expected: 先打印 `built .../dash_datalayer.mjs (+ dash_datalayer.wasm)`,再打印 `wasm node smoke: ALL PASS`,退出码 0。
排错:
- `emcc not found` → 未激活 emsdk,先 `source emsdk_env.sh`。
- node 报 ESM/`.wasm` 定位失败 → 确认用 node ≥18 且以 `.mjs` 引入;emscripten 的 ESM loader 用 `import.meta.url` 定位 `.wasm`,两文件须同目录(本脚本已保证)。
- 若某 `-s` flag 被你的 emcc 版本拒绝,按其报错改用 `-s KEY=VALUE` 空格形式或 `[]` 列表形式;不要改导出函数集本身。

- [ ] **Step 5: Commit**

```bash
git add tools/web/wasm/build_wasm.sh tools/web/test_wasm_node.mjs .gitignore
git commit -m "feat(web/wasm): emcc browser/node wasm build + node consistency smoke"
```

---

### Task 3: 浏览器最小页面(加载 wasm + feed 样本 + 渲染 state_json)  ⚠️ 需 emsdk/emcc

**Files:**
- Create: `tools/web/static/index.html`
- Create: `tools/web/static/app.js`

**Interfaces:**
- Consumes: Task 2 产出的 `dash_datalayer.mjs`(同目录),`cwrap('dash_init'|'dash_feed_line'|'state_json'|'current_scene', ...)`。
- Produces: 浏览器页面;`#out` 元素文本含 feed 结果 + `state_json` 的格式化输出。

- [ ] **Step 1: 创建 `index.html`**

`tools/web/static/index.html`:
```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>esp32-agent-dashboard — data layer (browser)</title>
<style>body{font-family:ui-monospace,monospace;background:#0b0a09;color:#f3eee2;padding:24px}
h1{color:#2BB3B1;font-size:1rem}pre{white-space:pre-wrap}</style>
</head>
<body>
<h1>WASM data layer — browser smoke</h1>
<pre id="out">loading…</pre>
<script type="module" src="./app.js"></script>
</body>
</html>
```

- [ ] **Step 2: 创建 `app.js`**

`tools/web/static/app.js`:
```js
// 在浏览器里加载固件同源数据层(wasm),feed 一条样本 snapshot,渲染 state_json。
import DashDataLayer from './dash_datalayer.mjs';

const out = document.getElementById('out');
try {
  const M = await DashDataLayer();
  const dash_init     = M.cwrap('dash_init', null, []);
  const dash_feed     = M.cwrap('dash_feed_line', 'number', ['string']);
  const state_json    = M.cwrap('state_json', 'string', []);
  const current_scene = M.cwrap('current_scene', 'string', []);

  dash_init();
  const snap = '{"agents":[{"kind":"claude-code","session_id":"cc_demo",'
             + '"status":"running","msg":"editing main.c","tokens":1234}],'
             + '"totals":{"total":1,"running":1,"waiting":0}}';
  const rc = dash_feed('dash snapshot "' + snap + '"');
  const state = JSON.parse(state_json());
  out.textContent = 'feed rc=' + rc + '\nscene=' + current_scene() + '\n'
                  + JSON.stringify(state, null, 2);
} catch (e) {
  out.textContent = 'ERROR: ' + (e && e.message ? e.message : e)
                  + '\n\n先运行 tools/web/wasm/build_wasm.sh 生成 dash_datalayer.mjs/.wasm';
}
```

- [ ] **Step 3: 起静态服务器并验证渲染**

Run(后台起服务器):
```bash
bash tools/web/wasm/build_wasm.sh
python -m http.server 8000 --directory tools/web/static
```
然后在浏览器打开 `http://127.0.0.1:8000/`,**预期**页面 `#out` 显示:
- `feed rc=0`
- `scene=` 一个场景 id(snapshot 含 1 个运行中 agent → 非 idle)
- 一段 JSON,含 `"totals":{"total":1,...}` 和 `"slots":[{"kind":"claude-code",...,"tokens":1234,...}]`

自动化验证(controller 用 Playwright):导航到该 URL,断言 `#out` 文本包含 `"total": 1` 与 `"kind": "claude-code"`,截图留档。若执行环境无浏览器自动化,则人工在浏览器确认上述三点。

- [ ] **Step 4: Commit**

```bash
git add tools/web/static/index.html tools/web/static/app.js
git commit -m "feat(web): minimal browser page loads wasm data layer + renders state_json"
```

---

## 验收(第 2 步)

- `python tools/web/test_wasm_datalayer.py` 全绿(含 tokeniser 病态用例 + 既有 6 个)。
- `bash tools/web/wasm/build_wasm.sh` 产出 `dash_datalayer.mjs`/`.wasm`;`node tools/web/test_wasm_node.mjs` → `ALL PASS`。
- 浏览器打开 `index.html` 渲染出含 `total:1` 与 `claude-code` slot 的 `state_json`。
- `main/` 零改动;native 与 wasm 共用同一份 `g7_tokenise.c`(无重复实现)。
- **结论**:同源数据层在浏览器/JS 运行时与 native 行为一致 → Step 3+(bridge fan-out / WebDeviceSink / SSE+POST / 前端渲染)可在此基础上推进。

## 后续步骤(不在本计划内)

spec §13 第 3–6 步:`DevicePusher` fan-out;`WebDeviceSink`(哑转发)+ `serve.py`(SSE/POST)+ `cmd_serve --web`;前端 7 场景 Canvas 渲染 + dev 面板 + 按键/注入;文档更新。每步另起计划。
