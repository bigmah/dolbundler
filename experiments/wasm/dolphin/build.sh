#!/usr/bin/env bash
# Build Dolphin + ModernGekko + a recompiled game module for WebAssembly.
#
#   ./build.sh                       # the browser build
#   ./build.sh --node                # the measurement build (NODERAWFS, no canvas)
#   ./build.sh --no-module           # interpreter only, for the baseline
#   ./build.sh --modules 'GEXE52=/path/to/generated'
#
# The generated directory is DolRecomp output plus main.dol, exactly as
# ios/README.md describes for the ARM64 build. Game-derived code never enters
# the repository, so it is supplied by path.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

NODE=OFF
MODULES="${DOLWEB_MODULES:-GEXE52=$ROOT/build-wasm/gexe52-c/generated}"
BUILD=""
OPT="${DOLWEB_MODULE_OPT:-3}"
# Cross-translation-unit inlining over a whole game is 65 MB of bitcode through
# one wasm-ld invocation. Worth having, but not worth blocking the first
# measurement on, so it is opt-in here and default-on for iOS.
IPO="${DOLWEB_MODULE_IPO:-OFF}"
# Dolphin turns LTO on for Release, which over a whole game module is one
# wasm-ld invocation holding every chunk's bitcode. Off by default here for the
# same reason as the module's own IPO: iterate first, then measure whether it
# is worth the link time.
LTO="${DOLWEB_LTO:-OFF}"
EXTRA=()

while [ $# -gt 0 ]; do
  case "$1" in
    --node) NODE=ON; shift ;;
    --no-module) MODULES=""; shift ;;
    --modules) MODULES="$2"; shift 2 ;;
    --build) BUILD="$2"; shift 2 ;;
    --opt) OPT="$2"; shift 2 ;;
    --ipo) IPO=ON; shift ;;
    --lto) LTO=ON; shift ;;
    -D*) EXTRA+=("$1"); shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$BUILD" ]; then
  if [ "$NODE" = ON ]; then BUILD="$ROOT/build-wasm/node"; else BUILD="$ROOT/build-wasm/web"; fi
fi

command -v emcmake >/dev/null || { echo "emscripten not found (brew install emscripten)" >&2; exit 1; }

# Two of Dolphin's Externals are nested git repositories, so their Emscripten
# fixes live here as patches rather than as edits git would never record. Both
# are idempotent: --check tells us whether the patch still applies, and a patch
# that no longer applies is already in.
apply_patch() {
  local repo="$1" patch="$2"
  [ -d "$repo/.git" ] || [ -f "$repo/.git" ] || return 0
  if git -C "$repo" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$repo" apply "$patch"
    echo "    applied $(basename "$patch") to $(basename "$repo")"
  fi
}
EXTERNALS="$ROOT/ModernGekko/vendor/dolphin/Externals"
apply_patch "$EXTERNALS/SFML/SFML" "$EXTERNALS/SFML/emscripten-config.patch"
apply_patch "$EXTERNALS/zlib-ng/zlib-ng" "$EXTERNALS/zlib-ng/emscripten-arch.patch"

# The module links thousands of chunk objects; the command line does not fit in
# ARG_MAX without response files.
export CMAKE_NINJA_FORCE_RESPONSE_FILE=1

# -pthread has to reach every translation unit, not just the link: wasm objects
# carry their feature set, and wasm-ld refuses to give shared memory to a module
# whose objects were not compiled with atomics and bulk-memory. One object built
# without it fails the whole link, and the message names the object rather than
# the cause.
emcmake cmake -S "$HERE" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-pthread" \
  -DCMAKE_CXX_FLAGS="-pthread" \
  -DENABLE_GENERIC=ON \
  -DENABLE_QT=OFF -DENABLE_NOGUI=OFF -DENABLE_TESTS=OFF \
  -DENABLE_VULKAN=OFF -DENABLE_SDL=OFF -DUSE_MGBA=OFF \
  -DENABLE_LTO="$LTO" \
  -DDOLWEB_TOTAL_MEMORY="${DOLWEB_TOTAL_MEMORY:-512MB}" \
  -DDOLWEB_NODE="$NODE" \
  -DRECOMPCORE_MODULE_OPT_LEVEL="$OPT" \
  -DRECOMPCORE_MODULE_ENABLE_IPO="$IPO" \
  -DDOLWEB_NATIVE_MODULES="$MODULES" \
  "${EXTRA[@]}"

cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

ls -lL "$BUILD/dolweb.wasm" | awk '{printf "dolweb.wasm: %.1f MB\n", $5/1048576}'
