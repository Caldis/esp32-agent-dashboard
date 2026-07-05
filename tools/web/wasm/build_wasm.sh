#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$ROOT/tools/web/static"
mkdir -p "$OUT"

# 自动激活 emsdk(对已在 PATH 的环境无副作用)
if ! command -v emcc >/dev/null 2>&1; then
  for env in "${EMSDK:+$EMSDK/emsdk_env.sh}" "$HOME/emsdk/emsdk_env.sh" /opt/emsdk/emsdk_env.sh /usr/local/emsdk/emsdk_env.sh /d/Code/emsdk/emsdk_env.sh; do
    [ -n "$env" ] && [ -f "$env" ] && . "$env" >/dev/null 2>&1 && break
  done
fi

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
  "$ROOT/main/cjk_font.c" \
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
