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
# 128-instruction chunks, not the C backend's 4096 default.
#
# README.md documented a 256 recipe and this default was never moved to match
# it, so every browser build -- and every device figure in OVER-THE-LINE.md
# before 2026-08-31 -- was quietly taken on the 4096 module.
#
# 128 and not 256, and not the node sweep's answer either. Node (V8) ranks them
# 4096 193.6% < 128 194.8% < 512 209.6% < 256 218.4%, putting 128 next to worst.
# Simulator Safari, renderer on, input driven through the window, two runs each:
#
#     chunks   wall to guest 200   guest speed   samples under 70%
#     4096     245 s, 240 s        92%, 99%      3/45, 0/42
#     256      245 s, 205 s        100%, 100%    0/40, 0/40
#     128      205 s, 205 s        100%, 100%    0/40, 0/40
#
# 128 is the only one that both reproduces and holds 100% with no hitching, and
# its module is the smallest (86.2 MB against 91.6). A 4096-instruction chunk is
# one `switch (ctx->pc)` in one enormous wasm function and JSC is much less
# willing than V8 to optimise those -- so anything tuned in the node harness
# that changes the shape of the generated code has to be re-measured here.
#
# Do not try to rank these by frame rate: fps over the same guest window came
# back 32.8-47.1 with no relation to build. See OVER-THE-LINE.md section 0a.
# Generated with DOLRECOMP_DIRECT_CALLS=1. Without it the module emits **no**
# cross-chunk calls at all -- every transfer between chunks sets ctx->pc and
# returns to the chassis, 492,202 such sites -- and the chassis dispatches 8.6 M
# times a second. With it, gated so the chassis dispatch gate is still consulted
# and the SMC guard stays exact:
#
# DOLRECOMP_TAIL_CALLS=1 then does the same for a cross-chunk `b` -- a tail call,
# which has no continuation to resume into and so always went back to the chassis
# even with direct calls on. Dispatches per 1000 guest cycles, which is
# deterministic and so immune to the machine's drift:
#
#     stock module                18.2
#     + direct calls              11.34  11.34  11.31
#     + tail calls                 9.05   9.05   9.05     -50% against stock
#
# and guest ticks per sample, OGL gameplay: 881.4 -> 895.9 (+1.6%) for direct
# calls, measured while spreads were tight. The tail-call half read +0.9% with
# overlapping arms on a machine that had warmed up, so treat the timing as
# directional and the dispatch count as the result.
#
# Regenerate with:
#   DOLRECOMP_C_CHUNK_INSTRUCTIONS=128 DOLRECOMP_DIRECT_CALLS=1 \
#     DOLRECOMP_TAIL_CALLS=1 dolrecomp --gamecube -jN <main.dol> <outdir>
#
# Check the binary first: both dolrecomp builds on disk in August predated the
# feature and silently ignored the variable. `strings dolrecomp | grep
# DOLRECOMP_DIRECT_CALLS` should match. Note DOLRECOMP_C_MAX_CALL_DEPTH only
# means anything once this is on -- it is the ceiling on the host recursion these
# calls create, and on a module without them it governs nothing.
#
# 2026-09-01, same recipe, newer dolrecomp (gexe52-c128-tc): the fallthrough off
# a chunk's end and every bctr/bctrl are resolved in the module too, and the
# transfers with nothing to resume into are *real* wasm tail calls (musttail ->
# return_call; needs -mtail-call below, and Safari 18.2+). That took chassis
# dispatches from 9.05 to 0.94 per 1000 guest cycles, deterministic, and the OGL
# gameplay rate 923 -> 992 M ticks/sample (+7.4%, three interleaved pairs). The
# same regeneration also fixed the gate index every text-section call passed:
# it was computed from a per-section table and named the chunk four below.
#
# And 64, not 128, once boundary crossings became tail calls: the node sweep's
# reason to stop at 128 was that every crossing was a chassis dispatch, and it
# is not any more. 64-instruction chunks (7417 of them, DOLRECOMP_C_CHUNK_
# INSTRUCTIONS=64 -- the floor is 16 now) measured +2.0% in headless Chrome
# (993.4 -> 1013.3 M ticks/sample, two interleaved rounds, arms not overlapping)
# and +1.3%/+3.4% in the iOS Simulator's JSC, 90.4 -> 90.0 MB. 32 is untried.
MODULES="${DOLWEB_MODULES:-GEXE52=$ROOT/build-wasm/gexe52-c64-tc/generated}"
BUILD=""
OPT="${DOLWEB_MODULE_OPT:-3}"
# Cross-translation-unit inlining over a whole game is 65 MB of bitcode through
# one wasm-ld invocation, and it is OFF because it does not pay.
#
# This was briefly ON. On 2026-08-31 it measured +14% (127.8/123.8 against
# 144.7/143.4) and that reading is **withdrawn**: two runs an arm, in the
# simulator, on the boot-anchored instrument whose spread is +/-15%. +14% sits
# inside that noise.
#
# Re-measured 2026-09-01 from a savestate, three runs an arm interleaved, spread
# 0.3-0.4%, guest ticks per perf sample:
#
#     backend   LTO+IPO off              LTO+IPO on
#     OGL       883.1  891.2  890.2      879.7  874.3  878.4      -1.3%
#     Null     1076.4 1075.6 1080.4     1063.7 1061.8 1064.5      -1.2%
#
# Consistent, and the arms do not overlap in either backend. It also costs 11 MB
# of wasm (86.2 -> 97.5), which a phone pays for on every cold load.
#
# Caveat kept deliberately: both re-measurements are V8, and the withdrawn +14%
# was simulator Safari. JSC is not measured -- desktop Safari was attempted and
# yields ~4 samples a run, because safari-run.sh restarts the browser and each
# run re-downloads ~100 MB cold; it needs ~300 s runs. If someone measures JSC
# and LTO wins there, turn it back on with the numbers written down.
IPO="${DOLWEB_MODULE_IPO:-OFF}"
# Dolphin turns LTO on for Release, which over a whole game module is one
# wasm-ld invocation holding every chunk's bitcode. Off with the module's IPO --
# they only mean anything together, and the measurement above is of both.
LTO="${DOLWEB_LTO:-OFF}"
# wasm SIMD is off because it does not pay, not because it cannot be built.
#
# --simd puts -msimd128 in both C and C++ flags, and the C++ half is what breaks:
# it reaches xxhash.h, which takes its wasm SIMD path by including <arm_neon.h>
# -- SIMDe on this toolchain -- from inside its own `extern "C" {`, where SIMDe's
# C++ headers cannot be declared. But the C++ side is not the side that could use
# SIMD. Putting the flag in the C flags only reaches the generated chunks and
# GXRuntime and builds clean:
#
#     DOLWEB_CFLAGS="-msimd128 -DDOLRECOMP_C_MAX_CALL_DEPTH=64 ..." ./build.sh
#
# Measured 2026-09-01 in gameplay from a savestate, interleaved against an
# otherwise identical build: 885.7 against 877.6 M guest ticks per sample --
# nothing, with the arms overlapping. The reason is structural: the generated
# code is scalar per guest instruction with nothing to vectorise across them, and
# a paired single is two f32 in a four-wide vector. Do not retry this.
SIMD="${DOLWEB_SIMD:-}"
# Extra C flags, which reach the generated chunks as well as Dolphin's C. The
# knobs worth sweeping live there as #defines with -D overrides:
# DOLRECOMP_C_MAX_CALL_DEPTH (how deep cross-chunk calls nest before falling
# back to the dispatcher) and DOLRECOMP_C_LOOP_CYCLE_BUDGET (how long an inlined
# loop runs before side-exiting). Raising the budget past the chassis's
# LOOP_GUARD_YIELD_CYCLES would reintroduce the ARAM-init livelock, so keep it
# well under 4096.
# Measured on GEXE52, Null backend, throttle off, guest cycles over a fixed
# wall-clock window: call depth 24 -> 64 is +6.9%, and the loop budget 256 ->
# 1024 on top of it is +10.1% cumulative, with native dispatches down 10%. Both
# work the same way -- a dispatch is a switch and an indirect call and a charge
# flush, and there were five and a half million a second.
#
# 1024 and not more: the chassis rescues a spinning loop by charging
# LOOP_GUARD_YIELD_CYCLES (4096) so the guard trips, and a budget at or above
# that would bring back the ARAM-init livelock.
#
# -mtail-call: clang then defines __wasm_tail_call__, which is what turns the
# generated code's DOLRECOMP_TAIL_CALL into a musttail return_call instead of a
# host call bounded by the depth ceiling. Every engine this targets has had wasm
# tail calls since 2023-2024 (Safari 18.2); a module built with it will not
# instantiate on one that has not.
CFLAGS_EXTRA="${DOLWEB_CFLAGS:--mtail-call -DDOLRECOMP_C_MAX_CALL_DEPTH=64 -DDOLRECOMP_C_LOOP_CYCLE_BUDGET=1024}"
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
    --simd) SIMD="-msimd128"; shift ;;
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
  -DCMAKE_C_FLAGS="-pthread $SIMD $CFLAGS_EXTRA" \
  -DCMAKE_CXX_FLAGS="-pthread $SIMD" \
  -DENABLE_GENERIC=ON \
  -DENABLE_QT=OFF -DENABLE_NOGUI=OFF -DENABLE_TESTS=OFF \
  -DENABLE_VULKAN=OFF -DENABLE_SDL=OFF -DUSE_MGBA=OFF \
  -DENABLE_LTO="$LTO" \
  -DDOLWEB_TOTAL_MEMORY="${DOLWEB_TOTAL_MEMORY:-512MB}" \
  -DDOLWEB_NODE="$NODE" \
  -DDOLWEB_PROFILING_FUNCS="${DOLWEB_PROFILING_FUNCS:-OFF}" \
  -DRECOMPCORE_MODULE_OPT_LEVEL="$OPT" \
  -DRECOMPCORE_MODULE_ENABLE_IPO="$IPO" \
  -DDOLWEB_NATIVE_MODULES="$MODULES" \
  "${EXTRA[@]}"

cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

ls -lL "$BUILD/dolweb.wasm" | awk '{printf "dolweb.wasm: %.1f MB\n", $5/1048576}'
