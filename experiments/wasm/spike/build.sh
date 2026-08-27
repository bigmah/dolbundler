#!/usr/bin/env bash
# Build GXRuntime for wasm32 and produce the browser spike.
#
#   ./build.sh        -> web/gxspike.{js,wasm}
#
# Then: python3 ../serve.py spike, and open the printed URL. The page drives
# gxcore with real GX register writes, takes the WGSL it generates, and renders
# it with WebGPU -- reading the pixels back off the GPU to prove it drew.
#
# Requires emscripten (brew install emscripten). Nothing else: GXRuntime's core
# and gxcore build for wasm with no source changes and no Aurora backend.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
GXR="$REPO/ModernGekko/vendor/dolphin/GXRuntime"
OUT="${OUT:-$HERE/build}"

[ -f "$GXR/graphics/gxcore/src/gxcore_shader.cpp" ] || {
  echo "GXRuntime not found at $GXR" >&2; exit 1; }

echo "==> configuring GXRuntime for wasm32"
# Aurora off: it is the native wgpu/windowing substrate, and the browser is
# already a wgpu host. AURORA_RECOMP on is what brings gxcore and the render
# sink, which is the half this spike needs.
emcmake cmake -S "$GXR" -B "$OUT/gxr" \
  -DGXRUNTIME_ENABLE_AURORA=OFF \
  -DGXRUNTIME_ENABLE_AURORA_RECOMP=ON \
  -DBUILD_TESTING=OFF >/dev/null

echo "==> building"
cmake --build "$OUT/gxr" -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 \
  | grep -E 'Built target|error' || true

echo "==> linking the spike"
em++ -O2 -std=c++20 \
  -I"$GXR/graphics/gxcore/include" \
  -I"$GXR/graphics/frontend/include" \
  -I"$GXR/include" \
  "$HERE/gxspike.cpp" \
  "$OUT/gxr/graphics/gxcore/libgxruntime_gxcore.a" \
  "$OUT/gxr/graphics/frontend/libgxruntime_aurora_render_sink.a" \
  "$OUT/gxr/libgxruntime.a" \
  -o "$HERE/web/gxspike.js" \
  -sENVIRONMENT=web -sMODULARIZE=1 -sEXPORT_NAME=GxSpike \
  -sALLOW_MEMORY_GROWTH=1 -sINVOKE_RUN=0 \
  -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","HEAPU8"]'

ls -l "$HERE/web/gxspike.wasm"
echo "    python3 ../serve.py spike   # then open the printed URL"
