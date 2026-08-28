# Recompiled guest code as Dolphin's CPU, in WebAssembly

*Written 2026-08-27 as a plan. **Answered 2026-08-28: it works.** Disney skate
plays at 60 fps in a browser on its recompiled code, with sound. The route is
built in `dolphin/`, and that directory's README carries the numbers and the
instruments. What follows is the plan as written, kept because the questions it
asked are the ones that turned out to matter — with the answers marked.*

## The answers, in the order the plan asked

| the plan's worry | what happened |
|---|---|
| module size | 91.6 MB, 12.8 MB gzipped. Not the wall; `-Os` is 6% smaller and no faster. |
| "the wasm tax on everything that is not the CPU" | Real and then not: the renderer cost 5x until one `glBufferSubData` was changed to `glBufferData`, after which Null and OpenGL measure the same. |
| coverage | `fallback_steps=32` over a ten-minute run. Coverage was never the problem. |
| indirect-call cost | Not measurable next to what was. |
| C backend versus LLVM | The C backend got there. LLVM is blocked on a real obstacle the plan did not see: it computes CPUState offsets with the host's `offsetof`, and wasm32 is 32-bit. |

And one the plan did not ask, which was the whole first day: **the chassis
livelocked.** A generated loop that reads a hardware register every iteration
cannot side-exit, because the read hook zeroes the charge its loop guard tests.

## The objective

**Disney skate at full speed in Safari on the iPhone, with nothing installed,
by doing to the wasm target what was already done to the ARM64 one.**

The two targets have the same defining constraint -- *no JIT allowed* -- and
that constraint is what shaped the iOS solution, so the shape transfers:

| | iOS | wasm |
|---|---|---|
| guest CPU | PPC recompiled ahead of time to ARM64 | PPC recompiled ahead of time to wasm |
| everything else | Dolphin, `ENABLE_GENERIC` (no JIT) | the same, now proven to build and run |
| how the module attaches | statically linked before signing, `AttachedDescriptor` | statically linked into the binary, `AttachedDescriptor` |
| guest memory | software-translated offset into a plain buffer | the same -- there is no fastmem on either |
| fallback for uncovered code | the plain interpreter | the same |

The last row is the same in a way that hurts more here: see "What the fallback
becomes".

**One correction to the received story.** The PGO in `ios/PERFORMANCE.md` was
applied to the *DolVM interpreter* (`-fprofile-instr-use=.../dolvm.profdata`,
`MODERNGEKKO_DOLVM_PGO_GENERATE`), and DolVM was deleted from the tree on
2026-08-26. It is not what made Disney skate fast. What did: the LLVM AOT
backend, plus **chunk size** (1024 -> 64 was +60% throughput and -66% .text,
and on GEXE52 specifically took a matched run from 71.8% to 105.4-106.9% of
realtime), plus inlining FP arithmetic that used to go through `cpu.c` helper
calls. So the lever to port is *codegen shape*, not interpreter PGO.

PGO does become interesting again here, but pointed somewhere new: at the
generated module and at Dolphin itself, both of which clang can instrument for
wasm. That is a later step, after the module is attached and measured.

## The question

Dolphin now runs in WebAssembly, but on its **PowerPC interpreter** — and that
costs ~16x native. The architecture's answer to that is not to speed up the
interpreter but to remove it: guest code arrives as a statically recompiled
module and Dolphin's `StaticRecomp` CPU core jumps into it. That is exactly
what the iOS app does, and it is why the same emulator holds 60 fps there.

So: can the wasm build use the recompiled module too?

**Yes, and the mechanism needs nothing new.** The reason is that iOS already
had this problem. iOS forbids JIT, so the module has to be *statically linked
before signing* and attached as a descriptor rather than `dlopen`ed. wasm has
the same constraint for the same practical reason, and the same path serves it.

## Why the ABI fits wasm

`ModernGekkoModuleDesc` (`ModernGekko/include/moderngekko/module_abi.h`) is a
plain C struct:

```c
int  (*dispatch)(CPUState* state, uint32_t address);
void (*on_state_loaded)(CPUState* state);
const ModernGekkoRange* code_ranges;   /* + smc_ranges, chunk_ranges, hashes */
```

Two function pointers and some range tables. Nothing in it is native code,
executable memory, or a dynamic symbol lookup:

- **`dispatch` is an ordinary indirect call** — a `call_indirect` through the
  wasm function table. Legal, and the same thing the emitted chunks already do
  among themselves.
- **`RECOMPCORE_MODULE_EMBEDDED=ON` builds a STATIC library**
  (`module-template/CMakeLists.txt:190`), named `g<GAMEID>_recomp`, exporting
  `<prefix>staticrecomp_get_module()`. No `dlopen`, no shared object, no
  `PROT_EXEC` page. This is the only module kind wasm can host, and it is the
  one iOS already uses.
- **The chunks are plain C.** `module-template` globs
  `${GENERATED_DIR}/chunks/*.c`. That is the same DolRecomp C output that
  already compiles to wasm32 unchanged and produces bit-identical guest results
  — an 82.7 MB GEXE52 module exists today for the GXRuntime client.
- **The layout check passes by construction.** `moderngekko_validate_module`
  compares `cpu_state_size` and `cpu_state_layout_hash`. Module and runtime are
  compiled by the same emcc for the same target, so they agree — again, exactly
  the iOS situation.

## What the fallback becomes

`StaticRecompCore` falls back to a JIT for anything the module does not cover
(`m_fallback_jit = JitArm64/Jit64`). A generic build has neither, so the
fallback is the plain interpreter. The source already says so, and calls out
that this makes desktop measurements misleading:

> the fallback counter only counts interpreted steps, so a desktop run reports
> zero fallback while a phone …

wasm inherits the iOS behaviour exactly: recompiled code for covered ranges,
interpreter for the remainder. **So module coverage is a performance
correctness issue, not just a speed one** — every uncovered range runs at the
16x-slower interpreter rate, and `STATICRECOMP_FALLBACK_RANGES` /
the shutdown counters (`native_dispatches`, `fallback_steps`) are how to see it.

## The steps

1. **Generate a module for ModernGekko, not for GXRuntime.** The existing
   `StrikersRecomp/generated-GEXE52` was produced with
   `DOLRECOMP_HLE_LOCAL_CALLS=1`, which is a StrikersRecomp/GXRuntime emitter
   mode and wrong here. Regenerate without it, into its own directory.
2. **Build `module-template` with emcc**, `RECOMPCORE_MODULE_EMBEDDED=ON`,
   `GAME_ID=GEXE52`, `GENERATED_DIR=<that directory>`,
   `GXRUNTIME_DIR=ModernGekko/vendor/dolphin/GXRuntime`. Expect the same
   `elseif(EMSCRIPTEN)` gaps this port has hit everywhere else, and expect
   `-mcpu`-style host flags to need the same treatment as
   `MODERNGEKKO_ENABLE_FRONTEND` did.
3. **Attach it.** Declare `extern "C" const ModernGekkoModuleDesc*
   g<ID>staticrecomp_get_module(void);`, call it, and pass
   `ModuleSource::AttachedDescriptor(desc)` in `RuntimeConfig`. `ios/CMakeLists.txt`
   generates precisely this registry (`kDolBundlerNativeModules`) and is the
   template.
4. **Re-run the harness** used for the interpreter number — same disc, same Null
   backend, `Start of main()` to the Bink intro — and compare against native
   1.61 s and interpreter-wasm 26.2 s.

## What could go wrong, in the order it matters

1. **Module size.** GEXE52 is 82.7 MB of wasm on its own; Dolphin adds ~8 MB.
   ~90 MB is in the range a phone already loads (106 MB in 1.5 s, measured), so
   this is a startup-latency question rather than a fatal one — but it is
   per-game, and it is why on-device recompilation stays interesting.
2. **The wasm tax on everything that is not the CPU.** Replacing the
   interpreter fixes the interpreter. It does nothing for VideoCommon's texture
   cache, vertex loaders and shader generation, or for the hardware models. None
   of those is interpreter-shaped, so none should cost 16x — but none has been
   measured, and if the renderer costs 5x the CPU fix will not reach 60.
   **Measure the frame loop separately from boot before drawing conclusions.**
3. **Coverage.** Anything the module does not cover runs interpreted at 16x, so
   a title that looks fine natively (where the fallback is a JIT) can be far
   worse in wasm. Watch `fallback_steps`.
4. **Indirect-call cost.** wasm `call_indirect` is bounds- and type-checked.
   The emitted code crosses that boundary per dispatch; on native it is a plain
   jump. Unknown magnitude, and it applies to the recompiled path specifically.
5. **The C backend versus LLVM.** iOS uses `--backend llvm`. The C backend is
   what compiles to wasm today. Adding `wasm32` to the LLVM backend's
   `supportedTarget()` is reportedly about one line plus `wasm-ld`, and would
   likely give a smaller and faster module — worth doing only after the C-backend
   number says the route is alive.

## Already true (do not re-derive)

- Dolphin's whole core, all 108 VideoCommon objects and the OpenGL backend
  compile for wasm32 — `bd71d02`.
- It runs: Disney's Extreme Skate Adventure boots through the GameCube SDK to
  `Start of main()`, loads its scripts and starts its Bink intro, **with no
  per-title knowledge at all** — `a6e653b`. That commit also documents the four
  fixes it took, of which only `MemArenaEmscripten.cpp` was interesting.
- Interpreter cost: native 1.61 s, wasm 26.2 s, ~16x, and -O1/-O3 links measure
  the same.
- Build recipe: `MODERNGEKKO_ENABLE_FRONTEND=OFF`, `ENABLE_GENERIC=ON`,
  `-pthread`, `em++` (not `emcc`), `-std=c++23`, `-sPROXY_TO_PTHREAD`. Dolphin
  logs to stdout with `<user>/Config/Logger.ini` → `WriteToConsole = True`,
  which is the only way to see where a boot stops.

## The decision this feeds

If the module brings the CPU to somewhere near the 0.5–1.0x that recompiled
guest code measured in isolation, and the renderer is not the next wall, then
this route replaces the GXRuntime browser client outright — because it needs no
per-title symbol table, no per-title audio middleware stand-in, and no per-title
debugging. That is the trade being tested: one large port against a per-game
integration for every game, forever.
