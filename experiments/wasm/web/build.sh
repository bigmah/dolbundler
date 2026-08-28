#!/usr/bin/env bash
# Build the browser client: StrikersRecomp's generated code + host policy,
# GXRuntime::web instead of GXRuntime::aurora, emcc instead of clang.
#
#   ./build.sh                    # incremental
#   ./build.sh --release          # link with Binaryen's full pass (slow, smaller)
#   ./build.sh --generate ISO     # (re)produce generated/ from your own disc first
#   ./build.sh --game GEXE52      # a different disc: its own host policy header,
#                                 # its own generated dir, its own module name
#
# Then:  python3 ../serve.py game --iso /path/to/your.iso
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CLIENT="$REPO/ModernGekko/vendor/dolphin/StrikersRecomp"
GAME=G4QE01
FAST_LINK=ON

# --game has to be read before --generate, which writes into the generated dir
# the game selects.
prev_arg=""
for arg in "$@"; do
  case "$prev_arg" in --game) GAME="$arg" ;; esac
  prev_arg="$arg"
done
if [ "$GAME" = "G4QE01" ]; then
  BUILD="$CLIENT/build-web"
  GENERATED="$CLIENT/generated"
  MODULE=strikers
else
  BUILD="$CLIENT/build-web-$GAME"
  GENERATED="$CLIENT/generated-$GAME"
  MODULE="$(echo "$GAME" | tr 'A-Z' 'a-z')"
fi

while [ $# -gt 0 ]; do
  case "$1" in
    --release) FAST_LINK=OFF; shift ;;
    --game) GAME="$2"; shift 2 ;;
    --generate)
      shift
      python3 "$CLIENT/tools/generate.py" --iso "$1" \
        --dolrecomp "$REPO/ModernGekko/vendor/dolphin/DolRecomp" \
        --output "$GENERATED"
      shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

command -v emcc >/dev/null || { echo "emscripten not found (brew install emscripten)" >&2; exit 1; }
[ -f "$GENERATED/generated.h" ] || {
  echo "no recompiled sources for $GAME: run $0 --game $GAME --generate /path/to/your.iso" >&2
  exit 1; }
[ -f "$GENERATED/sdk_symbols.inc" ] || {
  echo "no SDK intercept table for $GAME: run tools/symbols.py (a decomp) or" >&2
  echo "tools/sdk_signatures.py (a reference game) to produce" >&2
  echo "  $GENERATED/sdk_symbols.inc" >&2
  exit 1; }

emcmake cmake -S "$CLIENT" -B "$BUILD" -G Ninja \
  -DSTRIKERSRECOMP_ENABLE_WEB=ON \
  -DSTRIKERSRECOMP_GAME="$GAME" \
  -DSTRIKERSRECOMP_WEB_FAST_LINK="$FAST_LINK" \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release >/dev/null

cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 \
  | grep -E 'error|Linking CXX executable' || true

# The page loads the module from its own directory; symlink rather than copy so
# a 100 MB artifact is not duplicated (and stays out of git either way).
ln -sf "$BUILD/$MODULE.js"   "$HERE/$MODULE.js"
ln -sf "$BUILD/$MODULE.wasm" "$HERE/$MODULE.wasm"
ls -lL "$HERE/$MODULE.wasm" | awk '{printf "module: %.1f MB\n", $5/1048576}'
echo "    python3 ../serve.py game --iso /path/to/your.iso"
