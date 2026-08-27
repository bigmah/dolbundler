#!/usr/bin/env bash
# Build the browser client: StrikersRecomp's generated code + host policy,
# GXRuntime::web instead of GXRuntime::aurora, emcc instead of clang.
#
#   ./build.sh                 # incremental
#   ./build.sh --release       # link with Binaryen's full pass (slow, smaller)
#   ./build.sh --generate ISO  # (re)produce generated/ from your own disc first
#
# Then:  python3 ../serve.py game --iso /path/to/your.iso
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CLIENT="$REPO/ModernGekko/vendor/dolphin/StrikersRecomp"
BUILD="$CLIENT/build-web"
FAST_LINK=ON

while [ $# -gt 0 ]; do
  case "$1" in
    --release) FAST_LINK=OFF; shift ;;
    --generate)
      shift
      python3 "$CLIENT/tools/generate.py" --iso "$1" \
        --dolrecomp "$REPO/ModernGekko/vendor/dolphin/DolRecomp"
      shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

command -v emcc >/dev/null || { echo "emscripten not found (brew install emscripten)" >&2; exit 1; }
[ -f "$CLIENT/generated/generated.h" ] || {
  echo "no recompiled sources: run $0 --generate /path/to/your.iso" >&2; exit 1; }

emcmake cmake -S "$CLIENT" -B "$BUILD" -G Ninja \
  -DSTRIKERSRECOMP_ENABLE_WEB=ON \
  -DSTRIKERSRECOMP_WEB_FAST_LINK="$FAST_LINK" \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release >/dev/null

cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 \
  | grep -E 'error|Linking CXX executable strikers' || true

# The page loads the module from its own directory; symlink rather than copy so
# a 100 MB artifact is not duplicated (and stays out of git either way).
ln -sf "$BUILD/strikers.js"   "$HERE/strikers.js"
ln -sf "$BUILD/strikers.wasm" "$HERE/strikers.wasm"
ls -lL "$HERE/strikers.wasm" | awk '{printf "module: %.1f MB\n", $5/1048576}'
echo "    python3 ../serve.py game --iso /path/to/your.iso"
