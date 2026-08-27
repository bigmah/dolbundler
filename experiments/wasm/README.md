# WebAssembly as a target

Measured 2026-08-26. Two questions, both with running code in this directory:
can DolRecomp emit WASM instead of native arm64, and if it did, could a phone
run it. Nothing here is wired into the shipping build; it is an experiment kept
because the answers were specific enough to act on.

**The short version.** Recompiling to WASM already works and costs about 40% of
your speed. That is not the interesting part. The interesting part is that
WebAssembly is the only executable format iOS lets an app *produce and run
after install* — which is the constraint `ios/PERFORMANCE.md` calls
disqualifying — and the only engine allowed to compile it lives inside WebKit.
So "WASM on iPhone" and "a browser version" are not alternatives. They are the
same project.

---

## 1. The guest code compiles to WASM today

DolRecomp's C backend emits ordinary portable C, and guest memory is a
software-translated offset into a plain buffer — no `mmap`, no signal-handler
fastmem. Those are the two things that usually make an emulator unportable to
WASM, and neither applies here. **Do not break either one.**

A whole game's recompiled DOL (474,048 guest instructions, 118 chunks, 119 MB
of generated C) built for `wasm32` with *zero* source changes and produced
bit-identical guest checksums against the native build.

| Stage | native arm64 | wasm32 |
|---|---|---|
| compile all chunks (10 jobs) | 24.1 s | 31.8 s |
| object bytes | 25 MB | 36 MB |
| link | 0.1 s | 373 s |
| final code | 17.6 MB `__text` | 29.2 MB `.wasm` |

The 373-second link is Binaryen's `wasm-opt` over a 29 MB module, not
`wasm-ld`, and it is skippable — LLVM already optimised each translation unit.

The LLVM backend (the one iOS ships) needs three things to target WASM:
register the WebAssembly target, add `wasm32` to `supportedTarget()` in
`src/backend/llvm/llvm_backend.cpp`, and link with `wasm-ld`. Its guest loads
are already an explicit range check plus a byteswap against a base pointer,
which is the shape WASM linear memory wants.

## 2. With a JIT it costs ~40%; without one it is hopeless

`bench/` assembles four PowerPC kernels, runs them through DolRecomp, and
executes the result natively and as WASM on the same machine. Both arms get
whole-module optimisation, so this is not a call-overhead artifact.

Guest MIPS on an M4 (`jsc`, Safari and a real WKWebView agreed within 10%):

| kernel | native | wasm | ratio |
|---|---|---|---|
| integer ALU | 7 956 | 7 950 | 1.00x |
| guest memory | 1 191 | 790 | 0.66x |
| floating point | 678 | 282 | 0.42x |
| calls | 2 626 | 2 027 | 0.77x |

Weighted by a realistic instruction mix that is **0.5–0.6x of native**. Whether
that still clears 100% on a phone is *unmeasured*: the titles holding 100%
today are frame-limited, so their headroom was never recorded.

**The whole memory gap is one missing instruction.** `swap.c` isolates it: a
raw WASM linear-memory load is as fast as native (0.129 vs 0.142 ns — bounds
checks are free), but `bswap32` is folded away on arm64 and costs +0.214 ns in
WASM (2.66x), because WASM has no byteswap opcode and JSC does not pattern-match
the shift-or chain. There is no workaround. Do not go looking elsewhere for the
0.66x.

**Without a JIT, forget it.** The same module through JSC's interpreter tier is
18x / 42x / 95x / 33x slower than native on those kernels — one to two orders of
magnitude below the DolVM bytecode interpreter. An in-app WASM runtime (wasm3,
WAMR, in-process JavaScriptCore) is not a slower option; it is not an option.

**Which means there is no hybrid.** WebKit's JIT lives in the out-of-process
`WebContent`, holding the `dynamic-codesigning` entitlement Apple grants only to
itself. Guest RAM in WASM linear memory cannot be shared with a native Metal
renderer. If the guest code is WASM, *everything* is WASM, inside one web view.

## 3. A GameCube frame draws in the browser today

`spike/` is the runtime half, and it is the result that changed the plan.

Pointed at `wasm32`, **all of GXRuntime built on the first attempt with no
source changes** — the hardware model (CPU, MMIO bus, DVD, EXI, SI, DI,
interrupts, VI clock, memory card, ARAM, HLE, savestates) *and* gxcore, the
render sink, the retail GX frontend, trace I/O and `dolgx_replay`.

**gxcore already emits WGSL.** It targets the wgpu substrate, and WGSL is the
browser's own shader language, so there is no shader translation layer to write
and no missing backend. `graphics/gxcore/src/gxcore_shader.cpp` emits
`@vertex` / `@fragment` / `@group(N) @binding(0)` directly.

So the spike runs it end to end: real GX register writes (genMode, zmode,
cmode0, VCD, VAT) plus a big-endian vertex payload → `build_draw_plan` →
`generate_wgsl` → WebGPU. The whole GX pipeline is **83 KB of WASM**; the shader
was 1,317 bytes; the pixels come back off the GPU as correct Gouraud
interpolation of the vertex colours.

![The rendered frame](spike/frame.png)

### Two traps this cost real time to learn

**A GX colour channel's `matsource` selects a register, not the vertex.**
`LitChannel` bit 0: 0 takes the *material register*, 1 takes the *vertex
colour*. Leaving colour0 (`chan_regs[5]`) at 0 renders the whole draw black.
And **alpha is a separate channel** (`chan_regs[7]`) with its own `matsource` —
leaving that at 0 renders the draw fully transparent, with geometry and colour
both already correct and nothing on screen. Set both, and let `chan_reg_mask`
cover slots 0..8.

**Sampling a WebGPU canvas races the compositor.** `createImageBitmap(canvas)`
returned an all-black frame twice while the draw was fine. Render to an
offscreen texture and `copyTextureToBuffer` — that is queue-ordered and
trustworthy. `spike/web/control.html` exists for the same reason: it only clears
the canvas to red, and when the spike showed nothing it is what proved WebGPU
was fine and the bug was mine.

## 4. What the browser actually offers

Checked against iOS 26.3 WebKit, in Safari over HTTP and in a WKWebView inside
a real third-party app:

| capability | needed for | Safari | WKWebView (custom scheme) |
|---|---|---|---|
| WebAssembly JIT | everything | yes | yes |
| wasm SIMD128 | paired singles, texture decode | yes | yes |
| WebGPU / WebGL2 | the GX renderer | yes | yes |
| AudioWorklet | DSP output | yes | yes |
| SharedArrayBuffer | threads | yes | **no** |
| crossOriginIsolated | gates the above | yes | **no** |

A `WKURLSchemeHandler` can send `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy` headers and WebKit ignores them for custom
schemes, so threads are off. Serving the same bytes from a loopback HTTP server
inside the app restores isolation.

A 29 MB module compiles in 48–81 ms (compilation is lazy), and linear memory
grew to 1,984 MB. The number to watch is a **~16x metadata blowup**: 29 MB of
module cost ~460 MB of engine structures. That against a WebContent process's
jetsam limit is the most likely hard stop, and it is *unmeasured on device*.

## 5. Where this leaves the project

The cost is not the recompiler, and it is not Dolphin either.

ModernGekko is a thin layer over Dolphin Core (480,659 lines, no WASM port, no
WebGPU backend). If the browser build had to be Dolphin, this would be a long
project. It does not have to be: GXRuntime's whole stack reaches the browser
today, renderer included. The cost becomes *finish a 76k-line runtime* rather
than *port a 480k-line one* — and GXRuntime's gap is maturity and speed (~11 fps
in-match, by its own README), not portability.

`PLAN.md` in this directory is the full plan for taking this all the way to a
game running in Safari on an iPhone. The short version of its order:

1. **Decide GXRuntime, not Dolphin, is the browser runtime.** That is the fork
   in the road this spike unlocks, and it trades a mature-but-unportable
   emulator for a portable-but-immature one.
2. **Replay a real title's GX trace**, not a synthetic draw. `dolgx_replay` and
   `trace_io` already build for wasm. Record a FIFO trace, replay it through
   this path, and count the draws gxcore skips — that is the honest
   completeness number, and it is about a day.
3. **Measure the memory ceiling on a device**: load progressively larger modules
   in Safari on a phone until the WebContent process dies.
4. **Then** add `wasm32` to the LLVM backend, which is the easy part.
5. **Keep the native arm64 path.** It is faster and it works. WASM is a
   distribution strategy, not a performance one — and if the runtime port
   stalls, nothing is lost.

On-device recompilation, the thing that would actually dissolve the
one-binary-per-game constraint, needs DolIR to emit WASM bytes *directly* — no
clang, no LLVM, neither of which fits in a browser. DolIR is 2,007 lines and
`src/backend/vm/dolvm_emit.c` (DolIR → bytecode) is 2,815, so a DolIR → WASM
emitter is the same shape and roughly the same size. Call it ~3k lines.

---

## Running it

Requires `brew install emscripten`, a clang with the PowerPC assembler
(`brew install llvm`), and DolRecomp built (`./DolBundler/build.sh`).

```sh
cd experiments/wasm

./bench/build.sh              # native vs wasm guest MIPS, plus the byteswap
./bench/build.sh --web        # also build the browser harness
./spike/build.sh              # GXRuntime -> wasm, and the WebGPU spike

python3 serve.py spike        # open the printed URL
python3 serve.py bench --lan  # reachable from a phone on the same network
```

Results the pages POST back land in `reports.jsonl`, and a rendered frame is
written to `frame.png`.

**No game data, as everywhere else in this repo.** The benchmark's guest code is
`bench/bench.s`, assembled here. The browser harness will also measure a
whole-game module if you drop your own at `bench/web/full.wasm`, and skips that
test when it is absent; build outputs and any such module are `.gitignore`d.

### Caveats on the numbers

- Everything ran on an M4 Mac. Three JIT engines agreed within ~10%, and modern
  iPhone single-core is within ~20% of an M4, so the figures should transfer —
  but they were **not confirmed on a device**, and sustained thermal behaviour
  is the open variable.
- The WebGPU spike ran in macOS Safari. The **iOS Simulator cannot host it**: it
  exposes `navigator.gpu` but `requestAdapter()` returns null.
