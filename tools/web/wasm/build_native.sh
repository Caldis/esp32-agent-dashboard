#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"   # 仓库根
OUT="$HERE/build"
mkdir -p "$OUT"

# 选编译器
CC="${CC:-}"
if [ -z "$CC" ]; then
  for c in cc gcc clang; do command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }; done
fi
[ -n "$CC" ] || { echo "ERROR: no C compiler (cc/gcc/clang). Install one (MinGW/clang on Windows)." >&2; exit 1; }

# 选产物扩展名
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXT="dll" ;;
  Darwin)               EXT="dylib" ;;
  *)                    EXT="so" ;;
esac
LIB="$OUT/libdash_datalayer.$EXT"

# -fPIC:MinGW/MSYS/CYGWIN 下会产生无害 warning,按平台条件加
FPIC=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) FPIC="" ;;
  *)                     FPIC="-fPIC" ;;
esac

"$CC" -shared $FPIC -std=c11 -g -O0 \
  -I "$HERE/shim_include" \
  -I "$ROOT/main" \
  -I "$ROOT/main/harness" \
  "$ROOT/main/agent_state.c" \
  "$ROOT/main/cjk_font.c" \
  "$ROOT/main/tiny_json.c" \
  "$ROOT/main/harness/agent_snapshot_apply.c" \
  "$ROOT/main/harness/agent_commands.c" \
  "$HERE/g7_tokenise.c" \
  "$HERE/wasm_shim.c" \
  "$HERE/wasm_api.c" \
  -o "$LIB"

echo "built $LIB"
