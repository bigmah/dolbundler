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

- ~~`hook_fb` was 63.7M guest instructions per 45s handed back to the chassis to
  interpret one at a time, and no change above touched it. Whether DolVM pays
  something similar is unmeasured.~~ Measured: it does not. The bytecode path's
  equivalent -- opcodes the IR builder does not lower, which become `fallback`
  -- did not appear in the executed opcode mix at all. What *was* there is
  below. (The 17 opcodes the DolIR builder does not lower -- the `o` overflow
  forms and `lsw*`/`stsw*` -- are simply not instructions this game executes.)
- ~~The interpreter's build flags are worth 10-20% together and are easy to lose
  silently.~~ Checked in `build-ios/DolBundlerIOS.xcodeproj`: `-mcpu=apple-a16`,
  `-mllvm -tail-dup-pred-size=256`, `-mllvm -tail-dup-succ-size=256`, `-flto`
  and `-fprofile-instr-use=.../dolvm.profdata` are all on the interpreter's
  `OTHER_CFLAGS`, and the tail-duplication flags reach the linker too. The
  device was not missing them.
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

---

# What was slow, and what was done about it (2026-08-22)

The section above was written before the bytecode path had been profiled against
this title at all. When it was, the answer was not the interpreter.

## Disney skate spent 45% of every frame in one wait loop

Profiled over a fixed gameplay window -- a savestate taken 82 billion guest
cycles into a run, so two builds measure the same scene rather than whichever
one they happened to reach:

- 16.16 billion interpreted opcodes over 12 billion guest cycles.
- One guest block ran **272,012,114** times. The next busiest ran 2.4 million.
- One hardware address, `0xCC000000` (the command processor's status
  register), accounted for **272,013,641** of 272,300,000 accesses that left
  RAM -- 99.97%.
- The four hottest opcode *pairs* were one chain:
  `load32 -> rotl32i -> andi -> store8`, together 46% of everything executed.

That chain is `GXGetGPStatus`, which reads the 16-bit CP status word and unpacks
five bits into five bools with `lwz; rlwinm; stb` five times over. Its caller:

    loop: bl   GXGetGPStatus
          lbz  r0, overhi
          cmpwi r0, 0
          beq  loop
          lwz  r0, -0x70C8(r13)
          cmpwi r0, 0
          beq  loop

`overhi` is CP status bit 0, the FIFO overflow high watermark, and only a FIFO
*write* can set it. Nothing in the loop writes the FIFO, so the bit cannot
change while the loop runs: this is the game's idle thread, waiting for an
interrupt. Emulating it faithfully cost ~57% of interpreted opcodes and ~26% of
wall clock, the second part because every iteration's `lhz` left RAM -- a homed
register flush and refill around a chassis MMIO round trip that runs the
emulated GPU.

DolVM already skips idle loops, but its test (Dolphin's `IsBusyWaitLoop`) wants
one load, no stores and no call. This loop has all three.

## The fix: recognise the wait at the poll, not at the loop

`DOLVM_POLL_SPIN` in `dolvm_interp.c`. The interpreter counts consecutive reads
the chassis had to service that come back with the same value, from the same
guest instruction, at the same address. Past sixteen, the next back edge is
treated exactly as a recognised idle loop: charge what is left of the slice,
leave at the loop head, poll once more next slice. A write the chassis services
resets the run, and a `poll_fresh` flag stops a run left behind by a loop that
has since exited from firing on the next loop's first back edge.

A wait that has once been proved is remembered by (guest pc, address, value) in
an eight-entry table, so the threshold is paid once rather than once per timing
slice. The value is part of the key, so the read that finally answers the wait
is not covered by the site that recorded the waiting.

## Also: `rlwinm` as one opcode

With the spin gone, the hottest remaining pair was `rotl32i -> andi` at 4.9% of
all opcodes, and *every* `rotl32i` was followed by one. That pair is `rlwinm`,
which is how PowerPC spells every bitfield extraction there is. Four fused
opcodes (`rotl32i.and`, `shl32i.and`, `lshr32i.and`, `ashr32i.and`) collapse it,
folded in the emitter the same way an address add folds into a load. Module ABI
went to 5.

## Numbers (M4 Pro, cpu time, gameplay savestate window)

| | throughput |
|---|---|
| before | 1.055x |
| + poll-spin skip | 2.607x |
| + remembered wait sites | 2.885x |
| + `rlwinm` fused | **2.88x** |

Each step measured on its own; the last two are within the 3% band the machine
drifts by under sustained load, so treat them as "about 2.9x" rather than as
three decimal places. The same title's *boot* window (first 6e9 cycles) went
1.46x -> 1.43x, which is to say it did not move: the wait loop does not run
until the game is playing, which is the whole argument for benching a scene.

The same 180-billion-cycle boot run, window by window, before and after: every
scene transition lands at the same guest time (152.3s, 156.4s, 168.7s into the
first attract demo, 197.5s back to the menu, 284.0s into the second demo), and
non-idle guest blocks execute the same number of times to within 0.001%. Only
the idle spin collapsed -- 272M iterations to 600k.

Other titles, first 6e9 cycles, against the figures in `src/vm/README.md`:
Mario Party 4 2.94x -> 3.80x, SpongeBob 1.81x -> 12.36x (its boot is almost all
hardware waiting), Melee 1.39x -> 1.41x. Luigi's Mansion, not previously
measured, 6.80x. `test_dolvm` and `test_dolvm_diff` unchanged.

## Two measurement notes for next time

**The desktop bench needs the throttle off.** Without `[Core] EmulationSpeed =
0.0000` in `<user-dir>/Config/Dolphin.ini` the runner sleeps to hold 100% and
only the cpu-time figure moves; the wall figure reads 1.0000x whatever the
interpreter does.

**A gameplay window needs a savestate, not a longer run.** Disney skate's
attract loop alternates menus (1.4x) and skating (1.06x before this work), and a
window that opens at the first dispatch measures twelve seconds of logos.
`DOLVM_BENCH_SKIP` charges past the boot, `DOLVM_BENCH_WINDOW` reports every so
many cycles so the scene a window landed in can be identified, and
`MODERNGEKKO_SAVE_STATE_AT_SKIP=<path>` writes a state at that exact guest
instant. Loading it makes the measurement 23 seconds instead of three minutes
and puts both builds in the same scene.

**Sustained benching moves the number by 3%.** Back-to-back runs spread 0.9%;
runs half an hour apart under continuous load spread 3%. A/B back to back, or
idle the machine first.


---

# The heavy-game case: Melee, and the locked cache (2026-08-22)

Disney skate's problem turned out to be specific to Disney skate. Profiling the
heaviest title to hand -- Melee, savestated in its slowest scene, 0.77x on an
M4 Pro -- found a different one, and a more general one.

Melee's hottest opcode is `fp.available` at 14% of everything executed, and its
opcode mix is nothing like Disney's. But the opcode mix was the wrong place to
look: sampling put a quarter of the time *outside* the dispatch loop, split
between the paired-single helpers (~10%) and Dolphin's MMU write path (~13%).

Counting every write the chassis had to service explained the second:
**220 million of them per twelve guest seconds, 99.7% to
0xE0000000-0xE0003FFF** -- the Gekko's locked cache. Games use it as their
fastest scratchpad and Melee stages every paired-single store there. Each one
was an indirect call, a guest-charge flush, an address translation, a
page-split test, a gather-pipe test and an MMIO test, to reach a `memcpy`.

The locked cache is memory: a 256 KB buffer, big-endian, same as MEM1. So the
guest now gets a pointer to it (`CPUState::l1cache`, published beside
`ram`/`exram`, checked last in `get_ram_ptr`). Melee's heavy scene went
**0.765x -> 0.932x, +22%**, measured back to back.

**Editing CPUState is a three-file operation.** GXRuntime's, the mirror in
`ModernGekko/include/moderngekko/cpu_state.h` that the C++ side sizes module
descriptors against, and DolRecomp's. The chassis rejects a module whose
descriptor disagrees about `sizeof(CPUState)` -- and on the bytecode path it
does so *without printing anything*, so the game silently falls back to
Dolphin's interpreter and simply runs slowly. `runtime_test.cpp` pins the size
to turn that into a build failure.

**This machine drifts 25% under sustained load.** Disney skate's gameplay
window measured 2.88x in the morning and 2.13x after several hours of
continuous benching -- same commit, same binary. Every conclusion here that
compares two builds was measured back to back within a minute of a rebuild.
Numbers taken hours apart are not comparable, and one of them nearly cost a
good change: the locked-cache work looked like a 26% Disney regression until
HEAD was re-measured beside it and read the same.
