# LLVM ARM64 iOS AOT Plan

> **Status note (2026-08-26).** Milestones 1-4 are done and merged, and the
> DolVM bytecode path this plan was written alongside has since been removed
> from the tree -- native AOT is now the only way a game runs. References below
> to comparing against, or falling back to, DolVM are kept for the reasoning
> they record; the comparison baseline no longer exists in the tree, and
> Milestone 5's A/B is against an earlier commit or not at all.

## Objective

Make DolRecomp's existing LLVM backend emit relocatable Mach-O objects for
`arm64-apple-ios`, link those objects into DolBundler before code signing, and
prove on an iPhone that the selected game's generated ARM64 functions are the
code actually executing.

The first end-to-end target is Disney's Extreme Skate Adventure (`GEXE52`) on
an iPhone 15 Pro Max. This is a development build for a game supplied by the
developer. It is not an on-device compiler and is not a plan for distributing
game-derived native code.

## Current State

Most of the pipeline already exists:

- DolRecomp builds DolIR and has a real LLVM SSA backend in
  `ModernGekko/vendor/dolphin/DolRecomp/src/backend/llvm/`.
- The backend emits one relocatable object per guest-code chunk and a generated
  C header containing the dispatcher.
- The optimizer already runs `mem2reg`, CSE, GVN, loop optimization,
  vectorization, inlining, and dead-code elimination. LLVM performs final
  AArch64 instruction selection and register allocation.
- `module-template/CMakeLists.txt` already recognizes LLVM output, reads the
  object manifest, and accepts the emitted `.o` files as external objects.
- The current staged iOS integration can statically link one generated game
  module, select it by disc ID, and pass its attached descriptor to
  ModernGekko. This is the integration path to keep.
- LLVM-generated functions already make direct cross-chunk calls while carrying
  a shared execution budget. This avoids the worst dispatcher behavior of the
  portable C backend.

The blockers are:

1. `llvm_backend.cpp` only permits x86-64 Linux and Windows.
2. LLVM initialization and CMake components are expressed in terms of the host
   native target rather than an explicit AArch64 cross target.
3. Mach-O validation checks only the four-byte magic. It cannot distinguish an
   ARM64 iOS object from x86-64 macOS or ARM64 simulator output.
4. The emitter calculates raw `CPUState` offsets with the host compiler's
   `offsetof`. Those offsets must be proven identical to the GXRuntime layout
   used by the iOS app.
5. The native module has unresolved runtime-helper dependencies that must be
   verified against the exact GXRuntime objects linked into DolBundler.
6. Direct cross-chunk calls bypass the chassis entry point, including its SMC
   and mod checks. That is acceptable only for an explicitly unsafe first
   experiment; it is not a finished execution contract.
7. Selecting an attached module proves only that its descriptor was selected.
   The build needs positive evidence that LLVM-generated functions executed.

The Mac currently has Homebrew LLVM 22.1.8. DolRecomp deliberately accepts only
LLVM 19 or 20, so the initial implementation should pin a separate LLVM 20 host
toolchain instead of combining the AArch64 work with an LLVM 22 API migration.

## Intended Build Architecture

```text
Mac host
  main.dol
      |
      v
  host dolrecomp + LLVM 20
      target: arm64-apple-ios17.0
      cpu: apple-a16 for the current device build
      |
      +-- generated.h
      +-- generated_smc.txt
      +-- generated manifest
      +-- chunks/*.o       (Mach-O MH_OBJECT, CPU_TYPE_ARM64, platform iOS)
                              |
                              v
  Xcode/CMake iPhoneOS build
      module_export.c + module tables + chunks/*.o + GXRuntime helpers
                              |
                              v
  DolBundler executable     (all native code linked before signing)
                              |
                              v
  codesign -> install -> attached GEXE52 module -> generated ARM64 executes
```

LLVM is a Mac build-time dependency only. Do not enable or link `dr_llvm` in the
iOS target, and do not attempt to emit or load native code on the phone.

## Milestone 0: Reproducible Host Toolchain

1. Install or otherwise provide LLVM 20 with the AArch64 target enabled.
2. Build a separate host `dolrecomp` directory with:
   - `DOLRECOMP_ENABLE_LLVM=ON`
   - an explicit `LLVM_DIR` pointing at LLVM 20
   - Release mode
3. Record the following in the build log and generated metadata:
   - DolRecomp revision
   - LLVM version
   - target triple
   - target CPU and feature string
   - optimization level and codegen fingerprint
4. Keep this host build separate from `build-ios`. The former executes on
   macOS; the latter cross-compiles the app for iPhoneOS.
5. Add a small wrapper script or CMake preset so a build cannot silently use
   Homebrew's unversioned LLVM 22.

Acceptance:

- The host `dolrecomp --backend=llvm` is built with LLVM 20.
- LLVM reports AArch64 among its built targets.
- Re-running the same fixture with an empty object cache is deterministic, and
  the cache key changes when the target triple, CPU, features, or LLVM version
  changes.

## Milestone 1: Add AArch64 Darwin Code Generation

### Target support

Change `llvm_backend.cpp` to retain the existing x86-64 Linux/Windows targets
and additionally allow AArch64 Darwin targets. The shipping/dev target must be
specifically iOS, not simulator and not `arm64e`:

```text
arm64-apple-ios17.0
```

Supporting `arm64-apple-macos` as well is useful because it allows the existing
execution tests to run natively on the Apple Silicon Mac. It must remain a
distinct target and cache entry from iOS.

### LLVM target initialization

Initialize the AArch64 target, target info, target MC, and assembly printer
explicitly. Do not rely on `InitializeNativeTarget()` even though the current
Mac is ARM64; explicit initialization keeps cross-compilation correct on an
x86-64 build host too.

Update DolRecomp's LLVM component linkage to include AArch64 code generation.
Keep the linked component set narrow rather than initializing and linking every
LLVM backend.

### Target machine policy

- Use `Reloc::PIC_`; these objects become part of a position-independent iOS
  executable.
- Use LLVM's target data layout on every module before emitting IR.
- Encode the minimum OS in the triple so the object carries an iOS
  `LC_BUILD_VERSION` compatible with the app's deployment target.
- Use plain `arm64`, not `arm64e`.
- Initially match the existing iOS hot-code policy with `apple-a16` for the
  iPhone 15 Pro Max experiment. Before treating the build as portable, choose a
  CPU/features baseline valid for every supported device.
- Preserve strict guest floating-point semantics. Do not enable unsafe fast
  math or contraction as part of target bring-up.

### Object and cache validation

Replace magic-only Mach-O validation with parsing sufficient to require:

- 64-bit little-endian Mach-O
- file type `MH_OBJECT`
- CPU type `CPU_TYPE_ARM64`
- platform `PLATFORM_IOS`, not `PLATFORM_IOSSIMULATOR`
- minimum OS compatible with the app deployment target

Bump `DOLLLVM_CACHE_VERSION`. Include the normalized triple, CPU, features,
relocation model, code model, LLVM version, and deployment target in the
fingerprint. A macOS ARM64 or simulator object must never satisfy an iOS cache
lookup.

### Tests

Extend the LLVM backend tests to accept an explicit target triple and cover:

- native ARM64 macOS object emission and execution on the Mac
- ARM64 iOS object emission and structural inspection
- rejection of x86-64 Mach-O, ARM64 simulator, and wrong-minimum-OS objects
- cache separation between macOS, iOS device, and iOS simulator triples

For an emitted iOS fixture, verify with LLVM/Xcode object tools that:

```text
file:       Mach-O 64-bit object arm64
Mach header: CPU_TYPE_ARM64, MH_OBJECT
Build version: platform IOS, minos 17.0
Symbols:    func_<guest-pc> and func_<guest-pc>_budget are defined
Disassembly: contains AArch64 instructions, not bitcode or data-only sections
```

Acceptance:

- A tiny test DOL produces valid iPhoneOS ARM64 `.o` files.
- The same DolIR executes correctly through an ARM64 macOS object test.
- Invalid-platform objects fail before reaching the app linker.

## Milestone 2: Make the Generated ABI Explicit

This is a correctness gate, not cleanup. LLVM-generated code bakes raw struct
offsets and calls C helpers by ABI.

### CPUState layout

Create one native-code layout fingerprint covering every `CPUState` field read
by emitted LLVM code, including:

- all DolIR architectural state slots
- `ram`, `ram_size`, `exram`, and `exram_size`
- `external_read` and `external_write`
- `exception` and `downcount`
- reservation fields

Do not rely on `sizeof(CPUState)`. The DolRecomp build-time state and GXRuntime
state intentionally differ after their shared prefix, and equal size would not
prove equal offsets.

The emitter must write the fingerprint into generated metadata. The module
loader or generated module glue must compare it with a fingerprint compiled
against GXRuntime's `CPUState` and reject a mismatch before dispatch.
*(Done: `GXRuntime/include/core/native_state_layout.h`, whose
`dolnative_state_layout_hash()` is written into generated metadata by
`llvm_backend.cpp` and checked in `module_export.c`.)*

Also add build-time ABI fixtures compiled with:

- the host DolRecomp CPU header
- the iPhoneOS GXRuntime CPU header

They must agree on the complete emitted-code fingerprint.

### Helper call ABI

Inventory every undefined symbol in the emitted objects and classify it as:

- generated cross-chunk function
- GXRuntime CPU helper
- memory journal global/helper
- unexpected unresolved symbol

Generate a C fixture containing the same helper declarations and compile it to
LLVM IR for `arm64-apple-ios17.0` with Xcode clang. Compare its signatures with
the declarations created by `llvm_runtime_lowering.cpp`, especially `_Bool`,
`u8`, pointers, and functions returning `i1`. Add `zeroext` or other ABI
attributes where Clang's iOS ABI requires them.

At the final app link, fail on undefined symbols. Do not mask missing helpers
with dynamic lookup or weak imports.

Acceptance:

- Layout mismatch is a clear loader/build failure, not corrupted gameplay.
- Every undefined symbol from every generated object resolves to the intended
  generated function or linked GXRuntime implementation.
- Helper signature fixtures match Clang's ARM64 iOS ABI.

## Milestone 3: Link the Objects Into the Signed App

Use the existing embedded static-module path:

1. Run the host LLVM recompiler on GEXE52's `main.dol` with the explicit iOS
   triple and a dedicated cache.
2. Preserve the expected generated directory contract:
   - `generated.h`
   - object manifest
   - `chunks/*.o`
   - `generated_smc.txt`
   - `main.dol`
3. Point `DOLBUNDLER_NATIVE_GAME_ID=GEXE52` and
   `DOLBUNDLER_NATIVE_GENERATED_DIR=<generated-dir>` at that output.
4. Let `module-template` add the objects as external objects and build a static
   `gGEXE52_recomp` target.
5. Link that target into `DolBundler`; resolve helper calls against the
   `moderngekko_gxcpu` GXRuntime CPU implementation plus the module's exception
   source.
6. Ensure dead stripping does not discard generated chunks. The generated
   dispatcher should make every required chunk live; verify this in the final
   link map and symbol table rather than assuming it.
7. Build for `iphoneos`, then code sign the complete `.app`. No generated object
   may be added or changed after signing.

Add configure-time checks in the module template for the target metadata and
layout fingerprint before accepting the object manifest. Rename the current
`DOLBUNDLER_NATIVE_GENERATED_DIR` description so it is backend-neutral rather
than saying it contains C output.

Post-link checks on `DolBundler.app/DolBundler`:

- architecture is ARM64 iPhoneOS
- known `func_XXXXXXXX` symbols are present
- known functions disassemble as AArch64
- helper references are resolved
- no LLVM libraries or compiler backend are linked into the app
- no JIT or executable-memory allocation path has been introduced
- code signature verifies after all checks that do not modify the bundle

Acceptance:

- Xcode links the complete GEXE52 object set without platform warnings,
  duplicate helper definitions, missing symbols, or branch-range failures.
- The signed app installs and launches on the iPhone.
- A disc with no embedded module is reported as unplayable by the library
  rather than booted into a fallback.

## Milestone 4: Prove the LLVM Code Executes Correctly

### Positive backend identity

Add immutable generated metadata for:

- backend name (`llvm`)
- target triple
- LLVM version
- codegen fingerprint/build ID
- CPUState layout fingerprint

Log that metadata when the attached module is selected. Keep the existing
`using embedded native module for GEXE52` message, but do not treat it alone as
proof of execution.

Use all of the following as proof:

1. The final executable contains and disassembles known LLVM-generated chunk
   symbols.
2. The runtime logs the LLVM build ID from the attached module.
3. Shutdown reports a non-zero native dispatch count and zero unintended module
   rejection/fallback caused by ABI or SMC validation.
4. A device Time Profiler sample or symbolicated stack contains
   `func_XXXXXXXX_budget` frames from the embedded build.

Do not add a permanent counter to every generated function. It would perturb
the performance being measured. A build-only probe on one known hot function is
acceptable for initial bring-up and must be removable after sampling proves the
same fact.

### Correctness sequence

1. Run the small fixture module on device before linking the full game.
2. Boot GEXE52 through menus with rendering and audio enabled.
3. Run with `STATICRECOMP_LOCKSTEP=1` over a bounded window and require no real
   register, memory, exception, or cycle divergences.
4. Load the fixed Olliewood savestate and repeat lockstep on representative hot
   entry PCs.
5. Run a longer non-lockstep session and exercise savestate load, controller
   input, MMIO-heavy graphics paths, exceptions, and shutdown.
6. Record `native`, `fallback`, `native_exc`, `hook_fb`, `smc_failed`,
   verification, and charged-cycle counters.

### Cross-chunk safety

The LLVM backend currently turns known inter-chunk targets into direct calls.
Those calls skip the chassis's per-dispatch SMC/mod/timing gate. Resolve this
before calling the backend generally correct:

- Publish a native equivalent of the DolVM gate, or provide generated code a
  cheap target-open table plus live cycle/exception state. *(Done:
  `dolrecomp_native_gate` in `GXRuntime/include/core/dispatch_gate.h`.)*
- Check the gate at cross-chunk direct edges and indirect edges that can remain
  native.
- Preserve the current carried budget so this does not regress to one chassis
  dispatch per small chunk.
- Close a chunk immediately when SMC verification, an icache invalidation, or a
  mod makes it non-native.
- Retain the current side exit for unknown targets and exceptions.

For the first private GEXE52 experiment only, this can be placed behind a
clearly named unsafe build option if lockstep passes and `smc_failed=0`. The
unsafe option must not become the default.

Acceptance:

- Device sampling proves generated LLVM function bodies execute.
- The bounded lockstep run has no unexplained divergence.
- A normal run reaches and plays Olliewood from the known savestate.
- Safe cross-chunk entry honors SMC/mod closures without giving up direct-call
  amortization.

## Milestone 5: Measure Before Optimizing Further

Compare the LLVM build against a baseline build, with the same game data,
settings, and Olliewood savestate. The DolVM arm this was written for no longer
exists in the tree; a comparison now means an earlier commit, or an A/B of two
LLVM builds.

Measurement protocol:

- Use `% speed`, not guest FPS.
- Use the same 60-second window after savestate load.
- Cool the phone before the first run and interleave the two arms to control
  for the already observed thermal decline.
- Keep Metal, audio, internal resolution, and thread configuration identical.
- Capture shutdown counters and a device Time Profiler trace for each arm.
- Compare app size, link time, launch time, and peak memory as well as runtime.

Inspect optimized IR and AArch64 disassembly for several hot routines. Confirm
that guest state stays in registers across straight-line code, RAM accesses use
the intended inline fast path, and paired-single code vectorizes where its
semantics permit it.

Initial performance work explicitly deferred until this baseline exists:

- direct NEON lowering for exact paired-single helpers
- FPRF liveness/elision
- faster PSQ lowering
- gather-pipe specialization
- low-RAM exception-vector coverage
- device-collected LLVM instrumentation PGO
- per-title chunk-size tuning

Acceptance:

- Results identify how much time remains in generated code, exact FP helpers,
  chassis/fallback handling, rendering, and audio.
- Any claimed speedup is reproducible across interleaved runs and not based on
  a cold-phone versus hot-phone comparison.

## Implementation Order

Implement in this order so each step has a small, falsifiable result:

1. Pin and build LLVM 20 host tooling.
2. Emit and inspect one ARM64 iOS fixture object.
3. Run the same fixture as ARM64 macOS for semantic coverage.
4. Add layout and helper ABI validation.
5. Link the fixture objects into an unsigned iPhoneOS app.
6. Link and sign the full GEXE52 module.
7. Prove LLVM symbols execute on device.
8. Run lockstep and long-session correctness checks.
9. Implement safe cross-chunk gating.
10. Measure against the baseline on the fixed Olliewood workload.
11. Only then begin NEON/helper/PGO optimization.

## Likely Files to Change

- `ModernGekko/vendor/dolphin/DolRecomp/CMakeLists.txt`
- `ModernGekko/vendor/dolphin/DolRecomp/src/backend/llvm/llvm_backend.cpp`
- `ModernGekko/vendor/dolphin/DolRecomp/src/backend/llvm/llvm_backend.h`
- `ModernGekko/vendor/dolphin/DolRecomp/src/backend/llvm/llvm_function_emitter.cpp`
- `ModernGekko/vendor/dolphin/DolRecomp/src/backend/llvm/llvm_runtime_lowering.cpp`
- `ModernGekko/vendor/dolphin/DolRecomp/src/app/pipeline.c`
- `ModernGekko/vendor/dolphin/DolRecomp/tests/test_llvm_backend.cpp`
- `ModernGekko/vendor/dolphin/DolRecomp/tests/test_llvm_pipeline.c`
- `ModernGekko/vendor/dolphin/GXRuntime/include/core/native_state_layout.h`
- `ModernGekko/vendor/dolphin/module-template/CMakeLists.txt`
- `ModernGekko/vendor/dolphin/module-template/module_export.c`
- `ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompABI.h`
- `ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp`
- `ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h`
- `ModernGekko/src/runtime/dolphin_runtime.cpp`
- `ios/CMakeLists.txt`
- `ios/build.sh`
- `ios/bridge/dolbundler_run.mm`
- `ios/PERFORMANCE.md`

## Definition of Done

This project is complete when all of these are true:

- DolRecomp emits validated `MH_OBJECT` ARM64 iOS objects from the Mac.
- The objects carry the intended iOS platform/minimum-version metadata and have
  target-specific cache identities.
- CPUState offsets and helper ABIs are validated against the iOS GXRuntime.
- The complete GEXE52 object set links into DolBundler before code signing.
- The installed app identifies the LLVM build and device sampling shows
  generated `func_*_budget` frames executing.
- Lockstep and gameplay tests find no unexplained correctness divergence.
- Cross-chunk execution respects SMC/mod/timing closures without returning to
  per-chunk chassis dispatch.
- A controlled Olliewood comparison quantifies the speedup over the baseline.
- No compiler, JIT, unsigned module loading, or executable-memory generation is
  present in the iOS app.
