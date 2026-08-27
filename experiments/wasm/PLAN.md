# GXRuntime in the Browser Plan

*(The repository root's `PLAN.md` is the LLVM ARM64 iOS AOT plan, which shipped.
This is a separate line of work and does not touch it.)*

## Objective

Run a GameCube game the user supplies, in Safari on an iPhone, with the guest
code recompiled to WebAssembly and GXRuntime as the runtime — no Mac in the
loop after the first build, and one binary for every game.

The first end-to-end target is **Super Mario Strikers (`G4QE01`, NTSC)**, because
`StrikersRecomp` is the integration client GXRuntime is developed against and
is therefore the title with the least unknown runtime coverage. Disney skate
(GEXE52) is the second, because this repo already has the most measurement
history for it.

"Over the line" means: a disc the user owns, imported in the browser, boots to
gameplay at a frame rate a person would accept, on an iPhone, in Safari.

## Why this shape and not the obvious one

Not negotiable, and worth restating because it kills every simpler design:

- **WASM needs a JIT to be viable.** Interpreted, it is 18–95x slower than
  native — far below the DolVM interpreter that was already too slow. An in-app
  WASM runtime (wasm3, WAMR, in-process JavaScriptCore) is not an option.
- **The only JIT on iOS is WebKit's**, in the out-of-process `WebContent`.
- **Therefore there is no hybrid.** Guest RAM in WASM linear memory cannot be
  shared with a native Metal renderer. If the guest code is WASM, *everything*
  is WASM, inside one web view: CPU, GX, DSP, presentation.

So the browser build is not a port of the iOS app. It is the product, and any
future iOS app is a WKWebView shell around it.

## Current State

Measured 2026-08-26; see `README.md` here for the numbers and how to reproduce.

**Already true, verified:**

- DolRecomp's generated C compiles to `wasm32` unchanged and produces
  bit-identical guest results. A whole game is one 29 MB `.wasm`, which JSC
  compiles in 48–81 ms.
- **All of GXRuntime builds for `wasm32` with no source changes** — the hardware
  model (CPU, MMIO bus, DVD, EXI, SI, DI, interrupts, VI clock, memory card,
  ARAM, HLE, savestates) and gxcore, the render sink, the retail GX frontend,
  trace I/O and `dolgx_replay`.
- **gxcore already emits WGSL**, the browser's own shader language. There is no
  shader translation layer to write.
- Real GX register writes render correctly through WebGPU in Safari
  (`spike/`, 83 KB of wasm).
- iOS WebKit has WebGPU, WebGL2, wasm SIMD, AudioWorklet, SharedArrayBuffer and
  cross-origin isolation.

**The two facts that shape every milestone below:**

1. **The host boundary is one struct.** `include/gxruntime/platform.h` is 114
   lines and `DolPlatformOps` is 23 function pointers — present, `gx_write`,
   display lists, arrays, textures, TLUTs, copy destination, VI config, four
   pad entries, two audio entries. `src/headless_backend.c` implements the whole
   thing in 103 lines. A web backend is a third implementation of that struct,
   not a port.
2. **`StrikersRecomp` is the client template.** It compiles DolRecomp's
   `generated.c` + `chunks/` directly into an executable and links
   `GXRuntime::runtime` plus `GXRuntime::aurora`. The browser client is the same
   CMake shape with `GXRuntime::web` instead of Aurora, and `emcc` instead of
   `clang`.

**The number that decides whether this is worth doing:** GXRuntime's own README
puts in-match performance at **~11 fps**. The browser costs a further ~0.5–0.6x
on CPU-bound guest code. If that 11 fps is CPU-bound, the browser lands near 6,
and no amount of WASM work fixes it — GXRuntime has to reach roughly **2x
realtime natively** before the browser is playable. That is the project's
actual gate, and it is not in this repo's control.

## What is missing, honestly

gxcore keeps its own gap inventory (`GapCounters` in `gxcore.hpp`), which is the
best available list of what a real game will hit. Unimplemented or stubbed today:

| Gap | Counter | Matters because |
|---|---|---|
| CI/paletted textures (C4/C8/C14X2) | `tlut_texture` | common in GC titles |
| Lighting from channel control | `lighting_ignored` | any lit scene |
| EFB copy-to-texture | `efb_copy_ignored` | reflections, post, many effects |
| TEV stages beyond the slice | `tev_stages_over`, `tev_multi_texmap` | most real materials |
| Alpha compare without TEV | `alpha_compare_ignored` | cutouts, foliage |
| Fog on non-TEV draws | `fog_ignored` | outdoor scenes |
| Indirect texture stages | `indirect_ignored` | marked out of scope |
| Line/point primitives | skip reason | UI, particles, debug |
| Per-vertex texture matrices | `per_vertex_tex_mtx` | skinned/animated UVs |
| Emboss / SRTG texgen | `unsupported_texgen` | bump mapping |
| Logic op | `logic_op_ignored` | rare but real |

**Do not estimate this list from the outside.** Milestone 2 exists to turn it
into a per-title count, because "12 unimplemented features" and "12 features
that account for 0.3% of Strikers' draws" are different projects.

Outside gxcore: DSP audio is streamed rather than LLE, there is no browser disc
import, no touch input, and no WASM target in DolRecomp's LLVM backend.

---

## Milestone 0: Know where GXRuntime's 11 fps goes

Before any browser work. If the answer is "CPU", this plan's value depends
entirely on GXRuntime's own optimisation work and should be sequenced behind it.
If it is "renderer", WebGPU may behave differently enough that the number does
not transfer at all.

**Work.** Build `StrikersRecomp` natively with the Aurora backend, get to the
in-match scene, and profile it. Separate guest-CPU time from gxcore draw-plan
building from GPU submission from present/vsync.

**Exit criteria.**

- A breakdown of a frame in that scene into at least those four buckets.
- A statement of what realtime would require, as a multiple of today.
- A go/no-go recorded here: if in-match is CPU-bound and more than ~4x off
  realtime, stop and revisit, because the browser multiplies the gap.

## Milestone 1: The web backend

Implement `DolPlatformOps` against the browser, as `backends/web/`, alongside
`backends/aurora/`.

**Work.**

- `present`, `should_quit`, `configure_vi` — canvas sizing and the frame loop.
- `mark_gx_begin` / `gx_write` / `call_display_list` / `set_array*` — these
  already land in gxcore; the backend only forwards.
- The submission half: take gxcore's `DrawPlan` (WGSL string, decoded vertices
  in the fixed 120-byte layout, POD uniform blocks, viewport, pipeline key) and
  turn it into WebGPU pipelines, buffers and draws, with a pipeline cache keyed
  on `PipelineKey`. `spike/web/index.html` is the throwaway version of exactly
  this; the real one lives in C++ behind `emscripten/html5_webgpu.h` or in JS
  with a thin C shim, whichever keeps the per-draw crossing cheapest.
- `load_texture*` / `load_tlut*` — gxcore's `texture_decode.cpp` already decodes
  GX formats; the backend uploads and caches on `object_id` + `data_version`.
- `pad_*` and `audio_*` are stubs at this milestone.

**Design note.** Keep the JS/WASM boundary at *frame* granularity, not draw
granularity, if measurement says draws are expensive to cross. The draw plan is
already a flat POD; batching a frame's plans into one buffer and crossing once
is the fallback if per-draw `writeBuffer` dominates.

**Exit criteria.**

- `StrikersRecomp` built with `emcc` and `GXRuntime::web` renders its first
  frame in desktop Safari and Chrome.
- The frame is verified by GPU readback, not by eye — the spike's
  `copyTextureToBuffer` pattern, because sampling a WebGPU canvas races the
  compositor and will lie to you.
- A `control.html` equivalent is kept in tree so a black screen can always be
  bisected into "WebGPU is broken" vs "we are broken".

## Milestone 2: Replay a real title's GX trace, and count the gaps

The cheapest possible truth about coverage, and it needs no booting game.

**Work.** `dolgx_replay` and `trace_io` already build for wasm. Record a FIFO
trace from a title running natively, replay it through the web backend, and dump
`GapCounters` per frame.

**Exit criteria.**

- Per-title, per-scene table: draws planned, draws skipped, and the skip reason
  histogram.
- The gap table above re-sorted by **how many draws each gap actually costs**,
  which becomes the work queue for Milestone 3.
- Frames rendered from the trace compared against the native Aurora backend's
  output for the same trace — not pixel-exact necessarily, but close enough that
  divergence is a bug report rather than a shrug.

## Milestone 3: Close the gaps the trace says matter

Driven entirely by Milestone 2's histogram, in descending order. Expect CI
textures, lighting, EFB copies and TEV coverage to dominate; do not pre-commit
to that ordering before the measurement exists.

**Exit criteria.** Skipped draws under 1% of planned draws for the target
title's boot-to-gameplay path, and every remaining skip named and understood.

## Milestone 4: Boot a game in the browser

Everything before this replays or renders. This makes it a game.

**Work.**

- **Disc import and storage.** A file picker into OPFS, extraction with
  DolRecomp's `disc_extract_gamecube()` (already portable C), and `dvd.c` /
  `di.c` reading from OPFS. Watch iOS storage quotas and eviction — a 1.4 GB
  extracted disc is not something Safari guarantees to keep.
- **Input.** Touch controls into `DolPadState`, plus the Gamepad API for real
  pads. The existing on-screen pad in `ios/App/DBTouchPadView.mm` is the design
  reference, not the code.
- **Audio.** `audio_push` into an AudioWorklet ring buffer, with the
  user-gesture unlock that iOS requires before any audio starts.
- **The frame loop.** GXRuntime's `Run()` blocks; the browser cannot. Either
  drive it from `requestAnimationFrame` with a bounded slice per callback, or
  run it on a worker with `-pthread` and `SharedArrayBuffer`. **Start
  single-threaded** — threads need cross-origin isolation, which a WKWebView
  custom scheme silently does not provide.

**Exit criteria.** Target title boots from a user-supplied disc to a playable
scene in desktop Safari, with sound and working input, guest code still
interpreted.

## Milestone 5: The guest code as WebAssembly

Only now does the recompiler change, because until Milestone 4 there is nothing
to run it in.

**Work.**

- Register the WebAssembly target in DolRecomp's LLVM backend and add `wasm32`
  to `supportedTarget()` (`src/backend/llvm/llvm_backend.cpp`); link with
  `wasm-ld`. The guest load path is already a range check plus a byteswap
  against a base pointer, which is the shape WASM linear memory wants.
- Reconcile the module ABI. GXRuntime declares `GXRUNTIME_CPU_ABI_VERSION 3`
  and ModernGekko's descriptor is at version 4 — confirm which surface a
  browser client attaches to before assuming either.
- Skip Binaryen's `wasm-opt` at link. It cost 373 s on a 29 MB module and LLVM
  has already optimised each translation unit.

**Exit criteria.**

- Target title runs on recompiled WASM rather than the interpreter, with
  identical guest checksums against a native arm64 build of the same module.
- Measured speedup over the interpreted path recorded here.

## Milestone 6: iOS device bring-up

**This is where the plan's two unmeasured risks land**, and both are cheap to
retire early — consider pulling them forward if either looks likely to be fatal.

- **WebGPU on a real iPhone is unverified.** The iOS Simulator exposes
  `navigator.gpu` but `requestAdapter()` returns null, so it cannot answer this.
  A phone can, in ten minutes, with `serve.py spike --lan`. **Do this first.**
- **Memory.** A 29 MB module cost ~460 MB of engine metadata — a ~16x blowup —
  and that sits against the WebContent process's jetsam limit alongside guest
  RAM and textures. Load progressively larger modules in Safari on a device
  until it dies, and compare a Safari tab against a home-screen PWA, which may
  have a different limit.

**Work.** Beyond measurement: touch input tuning, thermal behaviour over a
sustained session, and deciding tab vs PWA vs WKWebView shell.

**Exit criteria.** Target title playable on an iPhone 15 Pro Max in Safari, with
a recorded speed figure and a recorded peak memory figure.

## Milestone 7: Recompile on the device

The milestone that makes this an emulator rather than a per-game binary, and
the only reason the whole approach beats the native AOT path on merit.

**Work.** Emit WASM bytes **directly from DolIR** — no clang, no LLVM, neither
of which fits in a browser. DolIR is 2,007 lines and
`DolRecomp/src/backend/vm/dolvm_emit.c` (DolIR → bytecode) is 2,815, so a
DolIR → WASM emitter is the same shape and roughly the same size. Call it ~3k
lines targeting a stack machine simpler than the bytecode it replaces.

**Exit criteria.** A disc the build has never seen, imported on the phone,
recompiled on the phone, and played — with no rebuild and no re-signing.

## Milestone 8: Make it fast enough

Deliberately last, and deliberately not optional.

**Start by finding out where the gap is, because two obvious answers are wrong.**
Measured against the `mem` kernel: deleting the byteswap entirely moves the
wasm/native ratio 0.62 → 0.68, inside the noise; replacing the branchy
`resolve_addr` with one unchecked path nearly doubles native (1295 → 2408 guest
MIPS) and leaves wasm flat (805 → 817). Neither the endianness swap nor the
range checks are the bottleneck — the wasm memory path sits at a ~800 MIPS floor
that neither change moves, and the cause is unidentified. **Attribute it before
optimising anything**, ideally against the LLVM backend's inlined memory path
rather than the C backend's helper calls.

- **Build the whole thing with `-msimd128`.** It is one flag, Safari supports
  it, and it takes bulk byteswap loops from 9.4 to 49.3 GB/s — native parity
  (48.8). It does nothing for scalar guest loads (818 → 818 MIPS on the `mem`
  kernel), so it is a texture-decode and vertex-loader win, not a CPU win. Do
  not hand-write intrinsics: LLVM's auto-vectoriser beat a hand-written
  `i8x16.shuffle` version (49.3 vs 44.2 GB/s).
- **Paired-single work maps onto `f64x2`** if the FP path needs it.
- **Floating point was the worst kernel (0.42x)** in the C backend's helper-call
  shape. The LLVM backend inlines the common FP path; confirm that survives the
  WASM retarget rather than assuming it.
- **Expect the ratio to worsen as the native side improves.** The `resolve_addr`
  result above shows native has headroom wasm cannot reach, so the 0.5–0.6x
  figure measured against the C backend is likely optimistic for the shipping
  LLVM backend.

**Exit criteria.** A speed figure on device for the target title, and an honest
statement of what is left.

---

## Risks

| Risk | Kills the plan? | Retire it by |
|---|---|---|
| GXRuntime's 11 fps is CPU-bound and far off realtime | Yes | Milestone 0, before anything else |
| WebGPU absent or broken on real iOS devices | Yes | Ten minutes with a phone; do it in week one |
| WebContent memory ceiling below a real game's needs | Yes | Progressive module loading on a device |
| gxcore coverage gaps are pervasive rather than long-tail | No, but re-scopes | Milestone 2's histogram |
| iOS evicts a 1.4 GB extracted disc from OPFS | No, annoying | Measure quota behaviour in Milestone 4 |
| Apple rejects a WKWebView-hosted emulator | No — browser build is unaffected | Only matters if an App Store build is wanted |

## What this plan does not do

- **It does not touch the native arm64 path.** That path is faster, it works,
  and Disney skate and Metroid Prime 2 hold 100% on it today. This is a
  distribution strategy, not a performance one, and if it stalls nothing is lost.
- **It does not port Dolphin.** ModernGekko's runtime is Dolphin Core at 480,659
  lines with no WASM port and no WebGPU backend. Choosing GXRuntime is the
  decision that makes this tractable, and it is a real trade: a
  mature-but-unportable emulator for a portable-but-immature one.
- **It ships no game data**, in the browser or anywhere else. The user supplies
  the disc; nothing game-derived enters this repository.
