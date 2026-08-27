#!/usr/bin/env bash
# Build the guest-code benchmark both ways and run it.
#
#   ./build.sh            native arm64 + wasm, run both, print guest MIPS
#   ./build.sh --web      also emit web/bench.{js,wasm} for the browser harness
#
# Requires: emscripten (brew install emscripten), a clang with the PowerPC
# assembler (brew install llvm), and DolRecomp built (DolBundler/build.sh).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${OUT:-$HERE/build}"
DOLRECOMP="${DOLRECOMP:-$REPO/ModernGekko/vendor/dolphin/DolRecomp/build/dolrecomp}"
SRC="$REPO/ModernGekko/vendor/dolphin/DolRecomp/src"
PPC_CLANG="${PPC_CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
PPC_OBJCOPY="${PPC_OBJCOPY:-/opt/homebrew/opt/llvm/bin/llvm-objcopy}"
JSC="${JSC:-/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc}"

[ -x "$DOLRECOMP" ] || { echo "no dolrecomp at $DOLRECOMP -- run DolBundler/build.sh" >&2; exit 1; }
[ -x "$PPC_CLANG" ] || { echo "no PowerPC-capable clang at $PPC_CLANG" >&2; exit 1; }

mkdir -p "$OUT"

echo "==> assembling PowerPC kernels"
"$PPC_CLANG" --target=powerpc-unknown-eabi -c "$HERE/bench.s" -o "$OUT/bench.o"
"$PPC_OBJCOPY" -O binary --only-section=.text "$OUT/bench.o" "$OUT/bench.text.bin"
python3 "$HERE/make_dol.py" "$OUT/bench.text.bin" "$OUT/bench.dol"

echo "==> recompiling to C"
rm -rf "$OUT/gen"
"$DOLRECOMP" --gamecube "$OUT/bench.dol" "$OUT/gen" >/dev/null
GEN="$OUT/gen/generated"
CHUNK="$(echo "$GEN"/chunks/*.c)"

# Both arms get whole-module optimisation so guest memory helpers inline on
# each side; emcc always does this at link, so an -O2-only native build would
# be an unfair baseline.
COMMON=(-O2 -I"$SRC" -I"$GEN" "$HERE/bench_main.c" "$SRC/cpu/cpu.c" "$CHUNK")

echo "==> native arm64"
clang "${COMMON[@]}" -arch arm64 -flto -lm -o "$OUT/bench_native"
"$OUT/bench_native" "${ITERS:-5000000}" 5

echo "==> wasm32 (JavaScriptCore)"
emcc "${COMMON[@]}" -o "$OUT/bench_wasm.js" \
  -sENVIRONMENT=shell -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
(cd "$OUT" && "$JSC" bench_wasm.js -- "${ITERS:-5000000}" 5)

echo "==> the byteswap, isolated"
clang -O2 -arch arm64 "$HERE/swap.c" -o "$OUT/swap_native" && "$OUT/swap_native"
emcc -O2 "$HERE/swap.c" -o "$OUT/swap.js" -sENVIRONMENT=shell -sINITIAL_MEMORY=64MB
(cd "$OUT" && "$JSC" swap.js -- 50000000)

if [ "${1:-}" = "--web" ]; then
  echo "==> browser harness -> web/bench.{js,wasm}"
  emcc "${COMMON[@]}" -o "$HERE/web/bench.js" \
    -sENVIRONMENT=web -sMODULARIZE=1 -sEXPORT_NAME=BenchModule \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB \
    -sEXPORTED_FUNCTIONS='["_bench_run","_malloc","_free"]' \
    -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString"]'
  echo "    python3 ../serve.py bench   # then open the printed URL"
fi
