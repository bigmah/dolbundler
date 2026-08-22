# What is actually slow on iOS

Measured 2026-08-21 on an iPhone 15 Pro Max against Disney's Extreme Skate
Adventure (GEXE52). This is a record of an experiment that has since been
reverted: for a week the app could be built with games compiled to native arm64
code instead of DolVM bytecode, to find out how much of the speed on a phone is
the interpreter's fault. The answer turned out to be "less than expected", and
the parts worth keeping are written down here rather than in the build.

## How to measure

**Read `speed`, not `fps`.** A GameCube game's own frame rate varies by scene --
Disney skate runs at 30 in one place and 60 in another -- so fps alone cannot
tell a slow emulator from a game that renders 30. Speed is emulation rate
against real hardware; 100% is full speed whatever the fps says.

Two switches exist for this, both off unless asked for:

```sh
DEVICECTL_CHILD_DOLBUNDLER_AUTOPLAY=GEXE52 \
DEVICECTL_CHILD_DOLBUNDLER_PERF_LOG=1 \
DEVICECTL_CHILD_DOLBUNDLER_RUN_SECONDS=90 \
  xcrun devicectl device process launch --device <id> --terminate-existing \
  com.bigmah.dolbundler
```

`DOLBUNDLER_PERF_LOG=1` writes `perf: <fps> fps <n>% speed` into
`Documents/moderngekko/dolbundler-run.log` every two seconds.
`DOLBUNDLER_RUN_SECONDS=<n>` stops the game cleanly after n seconds, which
matters because a kill skips Core shutdown -- and shutdown is where
StaticRecomp prints what the CPU core actually did:

```
[staticrecomp] shutdown: native=... fallback=... hook_fb=... smc_failed=...
               verifications=... bursts=... cycles=...
```

**Align samples by time since the run started.** Autoplay walks logos → menu →
attract demo, so the tail of one log is not the same scene as the tail of
another. Comparing tails produced two confidently wrong readings during this
work. Even aligned, runs drift onto different scenes after ~35s, so only a
window where both are demonstrably in the same scene is comparable.

**The simulator cannot answer a timing question.** It runs on the Mac's CPU and
GPU and pins 100% in every configuration, including ones that are slow on a
phone. It *can* answer host-independent ones -- the shutdown counters above are
properties of the module and the game, not of the host -- and it is the right
place to prove a change does not break a game before it reaches a device.

## What moved the needle, and what did not

Disney skate, gameplay section, on the phone:

| build | speed |
|---|---|
| DolVM bytecode (what ships) | 26-31% |
| native AOT | 38-42% |
| native AOT + dual core | 39-42% |
| native AOT + direct cross-chunk calls | 45-55% |

**Compiling the game to native code was worth ~1.35x.** Less than the gap
between an interpreter and machine code suggests, because the interpreter was
never the whole cost.

**Direct cross-chunk calls were worth a further ~1.3x.** DolRecomp's C backend
emits `ctx->pc = target; return;` for any `bl` crossing a chunk boundary
(chunks are 0x4000 of guest code), so the game returned to the chassis every
~23 guest cycles: 922M dispatches over 21.3G cycles.
`DOLRECOMP_UNSAFE_DIRECT_CALLS=1` makes it call `func_<chunk>(ctx)` directly
instead -- 25,714 sites for this title -- cutting that to 154M, one per ~138
cycles. Chunk entries all begin `switch (ctx->pc)` over every address they
cover, so entering mid-chunk is safe; the flag is off by default because a
direct call skips the chassis's SMC revalidation.

**Dual core was worth nothing.** Dolphin enables `CPUThread` only under
`#if defined(ANDROID)` (`Core/Config/MainSettings.cpp:59`) and nothing on iOS
sets it, which looks like an obvious win and is not: it measured identical to
single core, twice. Do not spend time there again.

**Two other dead ends, already checked.** The DSP is HLE by default
(`MAIN_DSP_HLE` is true), so the DSP interpreter never runs. The graphics
settings are already at their fast values -- internal resolution 1x,
`EFBAccessEnable` off, `EFBToTextureEnable` on, `DeferEFBCopies` on.

## What this means for the bytecode path

Very little of it transfers, and that is the useful conclusion.

The one real win -- cutting chassis dispatches -- **DolVM already does far
better than the native path ever did.** Its dispatch gate takes Melee from 158M
dispatches per 6e9 guest cycles to 0.5M. The native module, even with direct
calls, sits around 43M per 6e9. The gate is published only for the bytecode
path (`dolphin_runtime.cpp` sets `publish_gate` for `BytecodePath` and not for
`DynamicPath`), and the C backend has no equivalent. So the headline finding of
the AOT experiment is that the native path was missing an optimisation the
interpreter already has.

**What is worth measuring next on the bytecode path:**

- `hook_fb` was 63.7M guest instructions per 45s handed back to the chassis to
  interpret one at a time, and no change above touched it. Whether DolVM pays
  something similar is unmeasured. The DolIR builder does not lower 17 opcodes
  at all (the `o` overflow forms, `lsw*`/`stsw*`), so there is a comparable
  path.
- The interpreter's build flags are worth 10-20% together and are easy to lose
  silently: PGO (a stale profile is dropped without a warning), one indirect
  branch per handler via `-mllvm -tail-dup-*-size`, LTO with the same flags
  passed to the linker, and `-mcpu=apple-a16`. All live in ModernGekko's
  `CMakeLists.txt` under `moderngekko_dolvm`. Confirm they are active in an iOS
  build before concluding anything about interpreter speed.
- `DolRecomp/src/vm/README.md` has the desktop breakdown of where interpreter
  time goes.

## Why the AOT build is not the answer anyway

Even setting speed aside, it could not ship. iOS will not map a page executable
unless it is backed by a valid code signature: generating code on the phone
needs the `dynamic-codesigning` entitlement, which is WebKit's, and a dylib
produced on the phone could not be signed by anything on it. Native code has to
be compiled *and signed* before install, so the app can only ever run games it
was built with -- which is neither the emulator that guideline 4.7 permits nor
something this project can distribute. The bytecode path exists for that reason
and remains the shipping architecture.
