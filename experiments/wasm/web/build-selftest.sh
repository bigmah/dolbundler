#!/usr/bin/env bash
# Build the backend self-test: GXRuntime's web backend with a hand-written GX
# FIFO stream instead of a game. Seconds to build, so it is the fast loop for
# anything in backends/web or dolweb.js.
#
#   ./build-selftest.sh   ->  web/selftest.{js,wasm}
#   python3 ../serve.py game     # then open /selftest.html
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
GXR="$REPO/ModernGekko/vendor/dolphin/GXRuntime"
BUILD="${BUILD:-$HERE/build-gxr}"

emcmake cmake -S "$GXR" -B "$BUILD" -G Ninja \
  -DGXRUNTIME_ENABLE_AURORA=OFF \
  -DGXRUNTIME_ENABLE_AURORA_RECOMP=ON \
  -DGXRUNTIME_ENABLE_WEB=ON \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release >/dev/null

cmake --build "$BUILD" --target gxruntime_web -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
  2>&1 | grep -E 'error' || true

em++ -O2 -std=c++20 \
  -I"$GXR/include" \
  -I"$GXR/graphics/gxcore/include" \
  -I"$GXR/graphics/frontend/include" \
  "$HERE/selftest.cpp" \
  "$BUILD/libgxruntime_web.a" \
  "$BUILD/graphics/gxcore/libgxruntime_gxcore.a" \
  "$BUILD/graphics/frontend/libgxruntime_retail_gx_frontend.a" \
  "$BUILD/graphics/frontend/libgxruntime_aurora_render_sink.a" \
  "$BUILD/libgxruntime.a" \
  -o "$HERE/selftest.js" \
  -sENVIRONMENT=web -sMODULARIZE=1 -sEXPORT_NAME=DolWebSelfTest \
  -sALLOW_MEMORY_GROWTH=1 -sINVOKE_RUN=0 \
  -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","HEAPU8","HEAPU32","HEAPF32"]'

ls -l "$HERE/selftest.wasm" | awk '{printf "selftest: %.0f KB\n", $5/1024}'
