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


---

# Star Fox Assault, and where this pass ended up (2026-08-22)

Star Fox Assault was profiled last, as the heaviest title available. It ran at
0.45x, and sampling put **76% of the time outside the dispatch loop** -- 55% in
`__write_nocancel` and 12% in fmt's formatter.

It was logging. The game stores through an address that does not translate
about two million times per second of guest time, and Dolphin panics on each
one; a panic alert formats its message and writes it whether or not alerts are
enabled. **42 million of them in a minute.** Dolphin's own interpreter produces
the identical storm, so this is the emulator's reaction to the game, not a
DolVM bug -- and `GenerateDSIException` now reports eight and then says once
that it has stopped.

What was left was the quantized paired-single path: `psq_store_value` at 11%
and `ldexpf` at 2.7%, because each scaled lane asked libm for a power of two.
The GQR scale is a sign-extended six-bit field, so the exponent is always
normal and can be assembled directly -- bit-identical, verified by both titles
retiring exactly the same number of opcodes either way.

## Where the three titles ended up

Measured back to back against the commit this session started from, same
machine, same thermal state, same savestated scene:

| scene | before | after | |
|---|---|---|---|
| Disney skate, attract-demo gameplay | 1.023x | **2.693x** | 2.6x |
| Melee, its slowest scene | 0.974x | **1.341x** | 1.4x |
| Star Fox Assault, its heavy scene | 0.451x | **1.203x** | 2.7x |

Boot windows (first 6e9 cycles): Mario Party 4 2.94x -> 4.06x, SpongeBob
1.81x -> 11.89x, Melee 1.39x -> 1.62x, Luigi's Mansion 6.05x, Disney skate
1.46x -> 1.39x (unchanged; its wait loop does not run during boot).

Two more followed, both in the paired-single path that heavy titles live in.
The GQR scale was fetched from libm (`ldexpf` per stored lane); it is a
sign-extended six-bit field, so the exponent is always normal and can be
assembled directly -- bit-identical, and worth 4% on Star Fox. And a v2f64 lane
operation allocated a fresh register pair and moved the lanes into it, when the
value was already in registers that die at that instruction; letting the result
take them is worth 10% on Star Fox and removes what was the single hottest
instruction in the interpreter.

## What the wins have in common

None of them were in the interpreter. Every one came from asking what the
*game* was doing and what the *chassis* was being asked to do for it:

1. A wait loop the idle-loop test could not see, because it was behind a call.
2. Guest memory the chassis was servicing by hand -- the locked cache -- when
   it could have handed over a pointer.
3. An emulator diagnostic with no rate limit.

The tooling that found them is in `MODERNGEKKO_DOLVM_PROFILE`: hottest guest
blocks, and hottest guest addresses read and written outside RAM. On all three
titles the *opcode* histogram pointed somewhere else.

The interpreter itself is near its structural limit: ~5.9 host cycles per
bytecode op, and removing an opcode buys about 0.4% of wall clock per 1% of
opcodes removed. Forcing the plain-switch dispatch instead of 269 threaded
handlers costs only 1.8%, so the cost is the register-file round trip per
opcode, not branch prediction. Fusing `rlwinm` -- 9% of Disney's opcodes, and
every one of them on a dependency chain -- bought 1.8%. That is the going rate;
budget against it before writing another fused form. The remaining candidates
(`lfs`/`stfs` as single opcodes, indexed load/store, deduplicating
`fp.available`) are together worth about 5%.


## Where the desktop work ended, and why the rest is on the device

Disney skate's profile is now flat: 82% inside the dispatch loop, no guest
block above 2.4M executions in a 12-billion-cycle window, and 601 thousand
accesses leaving RAM where there were 272 million. The cost is spread evenly
across the common integer opcodes -- `load32` 9.6% of run time, `add32i` 5.0%,
`store32` 5.0%, `trunc` 4.9% -- which is what an interpreter looks like when
there is nothing left to find in the *game*.

The remaining catalogue is fusion, and its exchange rate is measured: about
0.4% of wall clock per 1% of opcodes removed. `lfs` as one opcode instead of
four is worth ~4.6% of Disney's opcodes, `stfs` ~2%, indexed load/store ~3.7%.
Together about 5% of wall clock.

**The device gap is not something the Mac can reproduce.** Running the same
savestated scene with a real graphics backend instead of Null costs 3.9% here
(2.733x -> 2.627x), so the CPU-side video work is small on this machine. Yet
the phone runs this title at roughly a quarter of the Mac's rate, against the
three quarters the single-thread CPU ratio predicts. Whatever accounts for the
rest -- the Metal backend on a mobile GPU, memory bandwidth, sustained thermal
behaviour -- has to be measured there.

`DOLBUNDLER_LOAD_STATE` exists so that measurement is 30 seconds rather than
eight minutes: push the same savestate the desktop bench uses and the phone
times the same scene.


---

# The device profile, at last (2026-08-22)

The DolVM sampler ran on the phone against the same savestated scene the
desktop bench uses. It answers the question the Mac could not.

Disney skate, iPhone 15 Pro Max, gameplay, sustained **41% speed** (24-61%),
against **269%** for the identical scene on an M4 Pro. Where the phone's time
goes:

| | share |
|---|---|
| `dolvm_dispatch` -- the interpreter | **48%** |
| `__semwait_signal` + `swtch_pri` + `semaphore_timedwait_trap` | **32%** |
| Dolphin's own interpreter (`SingleStepInner`, `GetOpInfo`, `ReadInstruction`, `NI_madd_msub`, `ps_madds0/1`, ...) | **~8-10%** |
| chassis, video, texture cache | the rest |

**Less than half of it is the interpreter.** Two things account for most of the
rest, and neither is visible on a Mac.

## The emulation thread is blocked a third of the time

Nearly a third of the samples are in `__semwait_signal`, `swtch_pri` and
`semaphore_timedwait_trap` -- the thread is not computing, it is waiting. In
single-core mode the video work runs on this same thread, so presentation
back-pressure (a blocking `nextDrawable` on the `CAMetalLayer`) or the audio
stream would both look exactly like this. Nothing on the desktop bench does
this: it runs headless with a null backend.

This is the largest single item in the profile and it is not interpreter work
at all.

## The Mac has a JIT and the phone does not, which hid a real cost

The shutdown counters, same scene, same savestate, byte-identical `main.dol`:

| | native dispatches | fallback instructions |
|---|---|---|
| Mac | 4,710,750 | **0** |
| iPhone | 2,222,896 | **173,351,892** |

That zero is not "no fallback". `m_fallback_steps` only counts *interpreted*
instructions, and the desktop build links `JitArm64` (264 symbols in the
binary), so anything the DolVM module does not cover gets JIT-compiled there
and never touches the counter. The iOS build has zero JIT symbols -- that is
the whole point of the port -- so the same code runs on Dolphin's plain
interpreter, one instruction at a time. 173 million of them, about 8-10% of the
phone's run time.

Disney skate ships no `.rel` files, so this is not dynamically linked game
code: it is whatever the guest executes that is not in `main.dol` -- the OS and
interrupt-vector code the game installs into low RAM at boot.

**Every desktop measurement in this file understates the phone for this
reason.** Module *coverage* is worth something on a phone and worth nothing on
a Mac, and no amount of desktop benchmarking will ever show it.

## What that means for what to do next

On the phone, roughly half the time is not the interpreter. Before writing
another fused opcode -- the remaining catalogue is worth about 5% of the
interpreter's half, so ~2.5% of the whole -- the two items above are worth far
more:

1. Find what the emulation thread waits on and stop it waiting. 32%.
2. Cover the fallback code, or make that path cheaper than one-instruction-at-
   a-time interpretation. 8-10%.

Both need the device to measure, which is now possible:
`MODERNGEKKO_DOLVM_SAMPLE=ON` plus `DOLBUNDLER_LOAD_STATE` and
`DOLBUNDLER_RUN_SECONDS` puts the whole breakdown in the run log.


## The largest single win, and it was not the interpreter

The device profile said 8-10% of the phone's time was Dolphin's own
interpreter. That undercounted it badly. Reproducing the phone's CPU path on
the Mac -- `MODERNGEKKO_NO_FALLBACK_JIT=1`, which simply declines to construct
the fallback JIT -- showed what it really costs:

| Disney skate, gameplay scene | throughput |
|---|---|
| desktop, fallback handled by JitArm64 | 2.70x |
| **same build, same module, no JIT** | **1.35x** |

Half the emulator's speed, and structurally invisible to every desktop
measurement ever made on this project, because `m_fallback_steps` only counts
*interpreted* instructions and a desktop always had a JIT to hand them to.

`MODERNGEKKO_FALLBACK_TRACE=1` names the addresses. On Disney skate, 53% of
them were one place: **0x80003724, inside `data[0]`** -- a 1,984-byte section
at 0x800032E0 sitting between text[0] and text[1]. That is where the linker
puts the exception handlers, and the recompiler had only ever decoded *text*
sections.

Covering data sections that lie between text sections took it from
**1.354x to 2.411x on the phone's path, +78%**, for 17 KB of module. Fallback
instructions went 335,254,697 -> 37,229,885. What is left is the exception
vectors at 0x00000500 and 0x00000C00, copies the OS writes into low RAM that
cannot be covered statically -- and they turn out to cost nothing measurable:
with data[0] covered, the JIT-less path and the JIT path are within 0.5% of
each other, so there is nothing left for a JIT to help with.

**Every desktop figure in this file for a title with hidden code in a data
section was flattered by a JIT the product cannot ship.** Use
`MODERNGEKKO_NO_FALLBACK_JIT=1` for anything that is supposed to predict the
phone.

## The "emulation thread is blocked a third of the time" was the frame limiter

Worth recording because it was wrong, and wrong in a way that would have cost
somebody a week.

The device profile put 23.6% in `__semwait_signal`, 4.7% in `swtch_pri` and
3.5% in `semaphore_timedwait_trap`, and this file said that was the largest
item in the profile and the thing to fix next. It was the frame limiter. The
sampled run averaged 68% speed with **38% of its samples at or above 98%** --
the emulator was ahead of real time for over a third of the window and slept,
exactly as it is supposed to.

What settled it: the simulator holds 100% and throttles, so its idle time looks
the same. Dropping `[Core] EmulationSpeed = 0.0000` into
`Documents/moderngekko/Config/Dolphin.ini` turns the limiter off there too, and
the profile changes completely -- **90.95% in `dolvm_dispatch`, 9.0% outside
it, and not one semaphore sample.**

So the simulator *can* answer this class of question after all, as long as the
limiter is off. That is the same trick the desktop bench needs, and it should
have been the first thing tried on the device profile rather than the last.

`DOLBUNDLER_NULL_AUDIO=1` and `DOLBUNDLER_NULL_VIDEO=1` exist now anyway; they
are the right tools if presentation or audio ever *does* look like it is
blocking.

With the limiter accounted for and the data-section fallback covered, the
phone's profile is the simulator's: the interpreter is the cost, and further
gains have to come from the opcode count.


## A lead that looked certain and was worth nothing

Recorded so nobody spends a day on it twice.

With the data-section fallback covered, `fp.available` is the second most
executed opcode in Disney skate -- **8.34% of everything the interpreter
runs**, 591 million of them, and on Melee's heavy scene 14%. The DolIR builder
puts one in front of every floating-point instruction, because the guest's OS
clears MSR[FP] on a context switch and expects the first FP instruction of a
thread to trap.

It cannot be hoisted out of the block: every guest instruction is a possible
mid-block entry, and moving the check earlier would fault before instructions
that should already have run. But it *can* be folded forward into the
instruction it guards, which keeps the trap at exactly the same program point
-- an entry naming the check lands on the fused opcode. On Disney skate 100% of
`exact.paired` is immediately preceded by one, so two guarded opcodes
(`exact.float.fp`, `exact.paired.fp`) collapse the pair.

It works, and it removes **4.5% of every opcode executed** -- 7.093G to 6.776G.

Measured back to back, with the fusion gated in the *emitter* so both arms run
the same interpreter binary against the same PGO profile:

| | throughput |
|---|---|
| without | 2.3536x |
| with | 2.3530x |

**Nothing. Not a slow win, not noise around a small win -- zero.**
`fp.available` is one MSR test with a perfectly predicted branch, and the
out-of-order machine absorbs the dispatch entirely. The bandwidth argument does
not rescue it either: 317 million eight-byte instructions not fetched is about
85 MB/s against a phone's ~50 GB/s.

Reverted. The lesson generalises: **opcode count is not a proxy for time on
this interpreter, and the cheapest opcodes are free.** The rate measured on the
`rlwinm` fusion -- 0.4% of wall clock per 1% of opcodes -- is an *average* over
opcodes that do real work. For a trivial one it is zero. Measure the specific
fusion; do not budget from the average.


## And a second one: `lfs` as a single opcode is also worth nothing

The other lead in the catalogue, and a better-looking one than the FP check,
because unlike that check it removes *work* rather than just a dispatch.

The Gekko fills both halves of a paired register from a single-precision load,
so the builder writes `lfs frD,d(rA)` as four instructions -- the guest load,
the widening to double, and a store to each half. One opcode
(`load.float.single`) does all of it: the load, the conversion, and both state
writes, with the slots written only after the load succeeds, exactly as the
four did.

It works. `store.statef` went 223M -> 29.8M, `fpext` disappeared entirely, and
the executed opcode count fell 7.093G -> 6.803G, **-4.1%**.

Measured the same controlled way -- fusion gated in the emitter, so both arms
ran one interpreter binary against one PGO profile:

| | throughput |
|---|---|
| without | 2.4354x |
| with | 2.4306x |

Nothing again. Reverted.

## The conclusion those two negatives add up to

**This interpreter is no longer dispatch-bound, and opcode count has stopped
predicting time.**

Two fusions, measured carefully, removing 4.5% and 4.1% of every opcode
executed -- one of them removing a float conversion and a state store, not just
a dispatch -- both returned zero. That is not noise around a small win; it is
the out-of-order core absorbing the work.

The `rlwinm` fusion earlier in the session did pay, 1.8% for 4.6% of opcodes,
and the rate quoted in this file (0.4% of wall clock per 1% of opcodes) came
from it. **That rate should not be believed any more.** It was measured on a
build that still had the wait loop's cost, the data-section fallback and a
different bottleneck; as the workload got faster, dispatch stopped being the
limiter.

Anyone picking this up should measure a specific fusion in a controlled A/B
before writing it, and should expect zero. The remaining catalogue -- indexed
load/store, `stfs` -- is not worth writing on current evidence.

Where the time actually goes now, on the phone-equivalent path: `load32` 9.6%,
`store32` 5.0%, `add32i` 5.0%, `trunc` 4.9% -- guest memory access and plain
integer work, spread flat, with no single item worth attacking. The interpreter
is at its structural floor for this design.

# The data-section win is also a miscompile (2026-08-22)

The largest speed win of the pass and the graphics corruption reported on the
phone are the same change, and they are the same 147 instructions.

## What the change was

`emit_dol_split` used to hand the recompiler only the DOL's *text* sections. A
GameCube DOL has 7 text and 11 data sections, and linkers do not sort code and
data neatly between them: this game's `data[0]` (0x800032E0..0x80003AA0, 1984
bytes) sits *between* text[0] and text[1] and is mostly code. The change adds
any data section fully bracketed by text sections, so that code compiles instead
of falling back to the interpreter.

On the desktop that reads as a **10% loss** and on the phone as an **84% win**:

| configuration | desktop (fallback JIT) | phone-equivalent (no JIT) |
|---|---|---|
| data[0] covered | 2.45x | **2.41x** |
| data[0] not covered | **2.71x** | 1.38x |

The desktop links `JitArm64`, so anything the module does not cover is compiled
by the JIT anyway and covering it badly only adds dispatches. iOS is
`ENABLE_GENERIC=ON` with no JIT at all, so the same code falls to the plain
interpreter. **A change to module coverage cannot be evaluated on the desktop
without `MODERNGEKKO_NO_FALLBACK_JIT=1`** -- it inverts the sign of the answer.

## What is in there, and why it is worth 84%

Five functions, all paired-single: `ps_madds0/1`, `ps_muls0/1`, `psq_l`,
`psq_lu`, `psq_stu`, `ps_div`. This is the SDK's paired-single matrix library
(`PSMTX*`), and the fourth function -- **0x80003704..0x80003950, 147
instructions** -- is on its own worth the entire win. Forcing just that one
function back to the interpreter costs everything:

| | phone-equivalent |
|---|---|
| all native | 2.41x |
| function 4 interpreted | 1.39x |
| all of data[0] interpreted | 1.38x |

The first 8 words of "function 4" are not code at all: 0x3F000000 and
0x3F800000, four of each -- a constant pool of 0.5f and 1.0f that the decoder
renders as `lis r24, 0`. The real entry is 0x80003724.

## The bug

With that function compiled, the game's main menu draws its 3D background as a
handful of flat untextured polygons. Everything else -- the menu panel, gameplay,
the attract demos, the intro FMV -- renders correctly, which is why a gameplay
savestate was not enough to catch it and the desktop looked fine for hours.

Established so far, all reproducible on the desktop:

- Forcing 0x80003704..0x80003950 to the interpreter fixes the picture.
- No sub-range of it does. Partial ranges leave the rest of the function in the
  module, and the module is re-entered mid-function, so a partial fallback
  isolates nothing.
- Not lane coalescing, not shift/AND fusion, not superblock formation, not CR
  fusion, not the entry-recipe liveness filter (`DOLVM_ALL_RECIPES`) -- each was
  gated in the emitter and rebuilt, and the corruption survives all of them.
- Not the locked cache, not the polled-wait skip: both have runtime switches and
  neither changes it.

What did find it was bisecting by *opcode*: `DOLVM_FALLBACK_OPS=<names>` sends
named opcodes down the path an unlowered opcode takes, which is the reference
interpreter. Four psq opcodes -> correct. The two update forms -> correct.
`psq_lu` alone -> correct.

## The bug: a flag nobody wrote

`psq_lu` and `psq_stu` write the effective address back to rA only if the access
succeeded, and `lower_psq` expresses that as a select:

    if (update)
        set_gpr(b, i->rA, select_value(b, DOLIR_TYPE_I32, success,
                                       address, gpr(b, i->rA)));

`success` is the result of the helper call. The bytecode interpreter's
`PSQ_LOAD`/`PSQ_STORE` handler never wrote it. It leaves the dispatch on
failure -- `if (!ok) goto leave;` -- and on success it fell through to `NEXT()`
without producing the value, so the select read a register the stream had never
written: whatever the last block left in that slot of the register file.

`DOLVM_OP_SELECT` tests its condition for non-zero, so roughly half the time the
leftover was truthy and the pointer advanced correctly. The other half it did
not, and a paired-single pointer walk that sometimes fails to advance turns a
matrix into rubbish. That is the flat-polygon menu.

The fix is one guarded store in the interpreter: control only reaches the end of
the handler on success, so the flag is 1 -- the point is that it has to be
*written*. `0xFF` is the emitter's "no register", now passed explicitly when
nothing consumes the flag. It costs nothing measurable: 2.35x/2.39x against
2.38x/2.32x for the broken build, interleaved.

## Why 46 passing tests did not catch it

`test_dolvm_diff` runs every opcode the decoder knows through both backends and
compares field by field, and `psq_lu` is in its table with a real update
encoding (rA=4, simm=8). It passed anyway, because the register file is stack
memory and whatever was in that slot happened to be non-zero -- the select took
the correct arm by luck, every run.

So the suite now poisons the register file: `g_dolvm_poison_registers` fills it
at each dispatch, and `DOLVM_POISON_BYTE` chooses the pattern. Both polarities
are needed and both are run (`dolvm_diff_poison`, `dolvm_diff_poison_zero`): a
condition is tested for non-zero, so an all-zero fill makes an unwritten flag
read false while a non-zero fill makes it read true, and a bug that survives one
is caught by the other. With the fix reverted, the zero-fill run reports 94
mismatches. **Any read-before-write in generated bytecode is now a test
failure rather than a coin toss.**

An audit of the same shape found no second instance: only two helper calls
return `DOLIR_TYPE_I1`, and `FP_AVAILABLE`'s result is discarded by the builder.

The `DOLVM_NO_*` switches used to get here are still in the tree
(`dolvm_emit.c`, `dolvm_opt.c`, `pipeline.c`, `dolir_builder.c`); each reads its
environment once rather than per instruction.

## Tooling this needed, which is worth keeping

- **`STATICRECOMP_LOCKSTEP` now works on the bytecode backend.** It looked up
  `ppc_set_mem_write_journal` with `GetSymbolAddress` and a `.dvm` is not a
  shared library, so lockstep was available only on the backend least likely to
  need it. It now falls back to the linked-in symbol.
- **`MODERNGEKKO_SAVE_STATE_AFTER=<seconds>:<path>`** writes a savestate on a
  wall clock, for scenes no bench window names.
- **`MODERNGEKKO_EMULATION_SPEED=<x>`** pins the frame limiter, so two builds
  are at the same guest instant when a screenshot is taken.

## Two traps this cost real time to learn

**Lockstep on a loop header compares different iterations.** The check runs the
interpreter until it reaches the module's exit PC; when that PC is a loop header
the module reached it by the back-edge and the interpreter reaches it by falling
through the prologue. The harness compensates by matching *charge*, and DolVM's
charge model is not the interpreter's, so a one-iteration offset -- `ctr:N=0x2a,
I=0x2b`, every pointer and FPR different -- is expected noise, not a finding.
Function 4's dispatch ends at its loop header every single call.

**Do not judge a picture by a screenshot at a fixed time.** The attract loop
moves, and the emulator window takes ~10s to appear after a state load, so an
early shutter photographs the desktop behind it and scores as a *pass*. The
working method is: sweep the whole attract loop, identify menu frames by
template-matching the menu panel (which is drawn correctly either way), and
judge only the background. `menutest.sh` + `menucheck.py` do this in ~4 minutes
and separate cleanly -- 320967 colours when correct, 17921 when not.

---

# Olliewood, the level that is still slow (2026-08-23)

The report was "Disney skate plays a lot better, but Olliewood still slows
down on my phone." Olliewood is the kid skater's hub -- `hub.prg`, the
Hollywood street with the fountain -- and it is not a Disney world, so nothing
before this had ever measured it. Every figure in this file above was taken on
the attract-demo gameplay scene, which turns out to be one of the *lighter*
things this game does.

## Getting to it, and what it costs

Reaching Olliewood by hand is: exit the demo with START, `D_LEFT` three times
to a kid skater (Mallie Ann or Ryan -- the Disney characters go to Disney
levels), A, A on PLAY GAME. Driving that needs Dolphin's named-pipe controller;
see [[driving-a-game-without-hands]]. The state is
`scratchpad/olliewood_plaza.sav`, standing in the plaza by the fountain, which
is representative rather than worst-case.

Measured back to back on the phone-equivalent path
(`MODERNGEKKO_NO_FALLBACK_JIT=1`, Null graphics, M4 Pro):

| scene | throughput |
|---|---|
| attract-demo gameplay (everything above was tuned on this) | 2.46x |
| **Olliewood plaza** | **1.32x** |

**Olliewood is 1.85x heavier than the scene the whole project was tuned on.**
That is the entire complaint, quantified: a phone that holds full speed in the
Disney levels is at a bit over half speed here.

## Why: three times the floating point, and 60fps instead of 30

Same window, same length, `MODERNGEKKO_DOLVM_PROFILE=ON`:

| | attract demo | Olliewood |
|---|---|---|
| bytecode ops | 7.09G | 10.06G (+42%) |
| `fp.available` (= guest FP instructions) | 591M | **1843M (3.1x)** |
| `exact.paired` | 230M | **644M (2.8x)** |
| `store.statef` | 224M | **768M (3.4x)** |
| `load32` | 734M | 806M (+10%) |

The integer work barely moves. What moves is floating point, and there are two
reasons. Olliewood is a large open street with far more geometry and more
pedestrians, and -- visible in the window title -- **it renders at 60fps where
the Disney levels render at 30**, so its per-second guest cost is roughly
doubled before the level's own size is counted.

Nothing in the *guest* is wasted the way `GXGetGPStatus` was: no block runs
more than 4.5M times in a 12-billion-cycle window, and the only wait loop
leaving the dispatch loop is the one already skipped. This level is simply
doing more work.

## What was done, and what each was worth

### The paired-single and float fast paths: +1%

`sample` put 7.8% of the whole scene in `ni_madd_msub` and another 4% in the
paired helpers. Each `ps_madd` ran two scalar lanes through an out-of-line call
that spends nearly all its time proving nothing unusual happened.

Three things are unusual: a denormal C operand (which `force_25bit_c`
normalises by hand), a result landing exactly on a round-to-even tie, and a
result leaving the finite range -- and an infinite *operand* always makes a
non-finite *result*, so the last test covers the operands too. All three are
properties of the bit patterns, so both lanes can be computed straight through
and all three tested together afterwards. Clang turns that into one `ccmp`
chain with a single branch. `ni_madd_msub` is now inlined and has disappeared
from the profile.

The same shape covers `fadds`/`fsubs`/`fmuls`/`fadd`/`fsub`/`fmul`/`fmadd`.

**One thing nearly made all of it worthless.** The first version declined the
fast path when FPSCR[NI] -- flush-to-zero -- was set, because `force_single`
implements the flush and the fast path did not. Counters said the fast path ran
**zero** times in a twelve-billion-cycle scene: Gekko titles run with NI set,
always. The fast path has to *implement* flush-to-zero, which is four
instructions, not decline it.

Measured back to back, both arms with a freshly regenerated PGO profile:

| scene | HEAD | with the fast paths |
|---|---|---|
| Olliewood | 1.307x | **1.321x** |
| attract demo | 2.458x | **2.476x** |

**+1.1% and +0.7%.** Against a ceiling of +12.7%, measured by stripping the
same helpers down to plain arithmetic with no exactness at all. So nearly all
of that ceiling is in work that cannot be removed -- the 25-bit rounding, the
double/single conversions, the register writes, FPRF -- and not in the
bookkeeping that was removed. Consistent with everything else in this file:
**cheap, well-predicted work is free on this core.**

Correctness: `moderngekko_paired_single_test` runs 48 million comparisons of
each fast path against the exact code it replaces -- every rounding mode, NI
both ways, VE set, operands drawn from denormals, ties, overflows, signalling
NaNs -- and requires the destination pair *and* every FPSCR bit to match. A
one-bit mutation of the 25-bit rounding constant produces 12,952 failures, so
the test bites. Both benched scenes also execute a bit-identical op count
(7,091,894,557 and 10,060,222,569), so the guest takes exactly the same path.

### Giving the GPU its own thread: +18% here, +58% on the attract demo

The much bigger one, and it is not in the interpreter at all.

**This file already says "dual core was worth nothing -- do not spend time
there again."** That reading was taken on 2026-08-21, on the *native AOT*
build, on a device, by aligning autoplay logs by wall time -- the method this
same file goes on to say "produced two confidently wrong readings during this
work". It is also a different CPU path, from before the wait-loop skip, the
locked cache and the data-section coverage, and there is no record of the
device having been checked to confirm the setting actually took. So it is
contradicted below rather than ignored, and **the contradiction is not settled
until it is measured on a phone again** -- which is the first item in the list
after this.

Dolphin's `CPUThread` -- run the emulated GPU on its own thread -- defaults to
off everywhere except Android, where upstream turns it on with the comment
"because the performance boost is really needed". iOS was running single-core,
which means every texture decode, vertex load, FIFO command and Metal command
buffer was built in time the interpreter was not running.

Measured with a real graphics backend (Metal), same savestates, same 6e9-cycle
window, wall clock not just cpu time:

| scene | single core | dual core | |
|---|---|---|---|
| Olliewood plaza | 1.227x | **1.444x** | +17.7% |
| attract-demo gameplay | 2.480x | **3.926x** | +58% |

Reproducible across alternating runs (1.4553 / 1.2272 / 1.4334 / 1.2275), and
the guest cycle count is identical, so the game is not taking a different path.
Disney skate boots, renders and plays correctly with it on -- attract demo,
menu with text, and Olliewood all check out -- and Melee boots too.

**And on the phone it is a 15% loss. See the device section below** -- this
subsection is kept because the desktop numbers are real and reproducible, and
because "the desktop said +19%" is the whole reason the device had to be asked.
iOS ships single core; `DOLBUNDLER_DUAL_CORE=1` turns it on for experiments.
The desktop runner is left alone so every figure recorded in this file stays
comparable, and `[Core] CPUThread = True` in `Dolphin.ini` turns it on there.

Not at 3x internal resolution either -- the desktop config renders at 3x and
the phone at 1x, which looked like the obvious confound and is not: at 1x the
same A/B reads 1.463x against 1.229x, **+19%**.

**The simulator cannot arbitrate this one.** Running the iOS app there against
the same state, dual core and single core both settle at ~185% speed, and so
does `DOLBUNDLER_NULL_VIDEO=1` -- three configurations, one number. The reason
is in the log: `perf: 0.0 fps` in every sample. The simulator is not presenting
frames, so there is no video work on the thread to move off it, and every
question about that work reads zero there. (`CPUThread = True` does land in the
config, so the setting itself takes.) This is a second, different reason for
the rule at the top of this file that the simulator cannot answer a timing
question.

**Two more things to check on a device before believing the number.** Dual core
spends more total CPU (two threads busy), so a phone may give some of it back
as thermal throttling over a long session; and the balance between CPU and
video work is different on a mobile GPU, so the split could be larger or
smaller than the Mac's.

## What is left, in order of size

1. **Measure Olliewood on the device.** Single core versus dual core --
   `DOLBUNDLER_SINGLE_CORE=1` is the other arm -- and a
   `MODERNGEKKO_DOLVM_SAMPLE=ON` profile of the level. Everything above is a
   Mac's opinion of a phone's problem, and on dual core the Mac and the older
   device reading disagree outright.
2. **`DSPThread` is False on the device** and True on the desktop, so audio DSP
   work is on the emulation thread there. Same family as the item above and
   untested.
3. **FPRF.** Every FP result classifies its value and writes the FPRF field of
   FPSCR, and nothing in a game ever reads it. Making `set_fprf` a no-op is
   worth **3.3%** of this scene. Doing it properly means either Dolphin's
   `wantsFPRF` analysis (a backward scan per block, plumbed through every
   helper) or a lazy FPRF that materialises when FPSCR is read -- and the
   readers include the chassis handoff, so a missed one is silent. Priced, not
   built.
4. **Homing the FP register file.** GPRs live in the VM register file; `fpr[]`
   and `ps1[]` are still read and written in CPUState, and this scene does
   1.0G `store.statef`/`load.statef`. Homing the GPRs was worth 7-13%. The
   obstacle is that the exact helpers take `CPUState*` and index it directly,
   so they would all need to take values instead.
5. **The gather pipe.** 26.5M guest writes to 0xCC008000 in this window, each
   one a homes flush plus a chassis round trip: about 2% together
   (`dolvm_guest_store_slow` 1.37%, `HookExternalWrite` 0.27%,
   `FlushGuestCharge` 0.30%). A pointer to the gather-pipe buffer with a call
   only when it fills would take most of that, the way `CPUState::l1cache`
   did for the locked cache.

## And one thing the player can choose

Olliewood renders at 60fps; the Disney levels render at 30. Half the emulator's
per-second cost in this level is that difference. Nothing here forces the game
to 30 -- the frame rate is the guest's own decision, made in guest code -- but
if a patch were ever wanted, halving it would be worth more than every
interpreter optimisation in this file put together.

---

# What the phone said about Olliewood (2026-08-23)

Everything above this line is a Mac's opinion. Here is the device: iPhone 15
Pro Max, the same `olliewood_plaza.sav` pushed to `Documents/`, booted straight
into it with `DOLBUNDLER_LOAD_STATE`, 60-second runs, `DOLBUNDLER_PERF_LOG=1`.
Read the median of the samples after the first four (those are the state load).

## The number

| run | median | mean |
|---|---|---|
| shipping build, run 1 (phone idle beforehand) | **88%** | 84% |
| shipping build, run 2 | 75% | 79% |
| shipping build, run 3 | 71% | 73% |

**Olliewood runs at roughly 70-88% on an A17 Pro, and the spread is thermal.**
Three identical back-to-back runs decline monotonically, so the first run after
the phone has been idle is the fastest and nothing measured late in a session
is comparable to anything measured early. Interleave arms; never compare two
runs by their position in a list.

That is the reported "slow downs", and it is a genuine one: the game asks for
60fps in this level and the phone delivers about three quarters of it.

## Where the time goes on the device

`MODERNGEKKO_DOLVM_SAMPLE=ON`, built for the device, same scene:

| | share |
|---|---|
| `dolvm_dispatch` -- the interpreter | **48.2%** |
| `__semwait_signal` + `swtch_pri` + `semaphore_timedwait_trap` | **31.8%** |
| Dolphin's own interpreter (`GetOpInfo`, `ReadInstruction`, `SingleStepInner`, `Cache::GetCache`, `TryReplaceFunction`, `MMU::TranslateAddress`, `NI_madd_msub`, `ps_madds0/1`, `ClassifyFloat`) | **~9%** |
| chassis (`ResolveNativeAddress`, `Run`, `RefreshRelSections`, `DispatchableAt`, `GetMurmurHash3`) | ~3% |

The waiting is not the frame limiter this time -- the emulator is below full
speed for most of the window, so it has nothing to sleep off. Turning each
consumer off in turn says what it is:

| configuration | median |
|---|---|
| shipping | 70-88% |
| `DOLBUNDLER_NULL_VIDEO=1` | **88%** |
| `DOLBUNDLER_NULL_VIDEO=1` + `DOLBUNDLER_NULL_AUDIO=1` | **94%** (mean 86%) |

**Rendering costs about 15% and audio about 6%**, both on the emulation thread,
and **with both gone the interpreter alone only just reaches full speed.** That
is the shape of this problem: there is no single thing to fix. Olliewood needs
several percent from several places.

The ~9% in Dolphin's own interpreter has a specific cause the shutdown counters
name: `native_exc=164564` guest exceptions over 24e9 cycles, and
`fallback=3542290` interpreted instructions -- about 21 per exception. Those are
the OS's exception vectors in low RAM, which the recompiler cannot cover
statically because the OS copies them there at boot. ~3,200 exceptions a second,
each running its handler one instruction at a time through `Read_Opcode` ->
MMU translate -> `GetOpInfo` -> HLE check.

## Four things that did not work, all measured on the device

| lead | median | verdict |
|---|---|---|
| GPU on its own thread (`CPUThread`) | 57-62% | **-15%**, the opposite of the desktop's +19% |
| ...plus `QOS_CLASS_USER_INTERACTIVE` on both threads | 57% | unchanged, so it is not thread placement |
| VSync off | 66% | nothing |
| specialised shaders instead of ubershaders (`ShaderCompilationMode = 0`) | 70% | nothing |
| DSP on its own thread (`DSPThread = True`) | 72% | nothing |

**Dual core is the one that matters, because the desktop was so confident.**
With a real graphics backend the Mac gains 19% in this scene and 58% in the
attract demo, reproducibly, in wall clock. The phone loses 15%, reproducibly,
and giving both threads user-interactive QoS does not move it -- which rules
out the obvious explanation (the GPU thread parked on an efficiency core while
the CPU thread waits on it). The remaining explanation is that two threads busy
at once on a phone do not run at the clock one thread gets, so the emulator
pays sync overhead for parallelism the silicon declines to give it. This is now
the second independent measurement saying dual core loses on iOS; the first is
at the top of this file, and it was right.

**The rule this establishes: for anything that touches threads or the video
backend, the desktop does not predict the phone, and can invert.** The
interpreter's own numbers do transfer -- the same opcode mix runs on both.

## So what would actually fix Olliewood

In descending size, with what is known about each:

1. **Make the game render 30fps instead of 60.** Half the emulator's
   per-second cost in this level is that the level asks for 60 where the Disney
   levels ask for 30. This is a guest-side decision and would need a patch;
   `DolBundler/patches` is where one would go. Worth more than everything else
   on this list combined, at the cost of a frame rate the game itself chooses.
2. **The exception vectors, ~9%.** Cover the OS's handlers so they stop being
   interpreted one instruction at a time -- the chassis already has the
   verification machinery that would let a low-RAM copy be recognised as a
   known DOL range.
3. **Rendering, ~15%.** It cannot be moved to another thread on this hardware,
   so it has to get cheaper. Not investigated further here.
4. **The interpreter's floating point.** FPRF elision (3.3% of the scene) and
   homing the FP register file, both priced above and neither built.

# The coverage a single rewritten instruction costs

Measured 2026-08-23 against Super Mario Strikers (G4QE01), which arrived with
the complaint "super slow on iPhone". It was: **9-14% speed** on an iPhone 15
Pro Max. It is now **~70%**, and none of that came from the interpreter.

## What was wrong

Two lines into any run of this title:

```
[staticrecomp] SMC: chunk [0x802456C0,0x802856C0) hash mismatch
```

A chunk is the unit the chassis verifies guest RAM against, and it was 65536
guest instructions -- 256 KB. One word inside it did not match the DOL the
module was built from:

```
80260560:  60000000  nop            <- in main.dol
80260560:  7c9043a6  mtspr SPRG0,r4 <- in RAM, once the game has booted
```

That is the SDK's `OSExceptionInit` patching its own handler template, which
every Nintendo title does. The chassis is right to stop trusting the chunk. But
"the chunk" was a quarter of a megabyte of the game's hottest code, and with no
fallback JIT -- which is every iOS build -- all of it went to Dolphin's plain
interpreter. `MODERNGEKKO_FALLBACK_TRACE` named the sites: 3.8 million
interpreted runs at 0x80257CC0, 1.9 million at 0x80248164, and so on down.

**A desktop run cannot see this.** The Mac links JitArm64 and compiles whatever
the module stops covering, so the same title reads 1.4x there and 0.17x with
`MODERNGEKKO_NO_FALLBACK_JIT=1`.

## The fix, and what it cost

`DOLVM_DEFAULT_CHUNK_INSTRUCTIONS` 65536 -> 4096, so the blast radius of a
patched word is 16 KB instead of 256 KB. Nothing else changed.

Throughput over a fixed gameplay window, no fallback JIT, M4 Pro:

| scene | before | after |
|---|---|---|
| **Mario Strikers, kickoff** | **0.169x** | **1.046x** |
| Disney skate, Olliewood | 1.499x | 1.489x |
| Melee, heavy | 1.207x | 1.185x |
| Star Fox Assault, heavy | 1.293x | 1.263x |

So it is 6.2x on the title that loses a region and **0.6-2.3%** on the titles
that do not. The cost is real and it is the gate: a call inside its own region
lowers to a jump, one that leaves lowers to a `CALL` whose gate check is a
dispatch the jump does not pay. Mario Party 4's *boot* exaggerates it to 6%;
its heavy scenes do not, and boot is not what anyone plays.

Of the eight titles on this machine, **only this one loses a region at all.**
The rest report `smc_failed=0`.

## Two things this made possible, and one it did not

The shutdown line now carries `smc_lost=<bytes>`, and the first failure of each
chunk prints how much guest code it took away and the range the guest last
invalidated -- which is the patched address, and was otherwise a day's work to
find.

The block histogram under `MODERNGEKKO_DOLVM_PROFILE` used to count only at
`CHARGE`, so every loop whose header charge the emitter folded into its own
back edge was invisible -- which is every hot loop worth finding. It counts at
`jmp.guard` now. That is how the next two leads were found, and both were dead:

**Idle-skipping the SDK's `PPCHalt`** (`sync; nop; li r3,0; nop; b`) is not
safe. The emitter would not mark it idle because its `nop` reads and writes r0,
which the rule reads as a carried value; making the rule see through the
identity, or lowering `ori rX,rX,0` to nothing, both get the loop recognised.
Either way the title then **freezes mid-match**: the guest starts reading
through null pointers within six guest seconds, and the emulator fast-forwards
the corpse at 14000% speed. It reproduces from a savestate in the simulator
with `DOLBUNDLER_NULL_VIDEO=1` in about a minute. And it buys nothing anyway --
1.0249x without against 0.9996x with, on a scene where the game is not idle.
The 2x it appeared to be worth was measured on a savestate whose game had
already died.

**Aligning region boundaries to function entries** -- so no straight-line run
or loop crosses one, leaving the gated calls as the only cost -- is worth
nothing measurable (MP4 3.8045x aligned against 3.8002x not) and it moves which
code lands in the un-covered hole. On this title that was enough to produce the
same freeze. Not kept.

## Where the remaining 30% is, on the phone

`DOLBUNDLER_NULL_VIDEO` / `DOLBUNDLER_NULL_AUDIO` against the same savestate,
iPhone 15 Pro Max, kickoff scene:

| arm | speed |
|---|---|
| as shipped | 62% |
| no audio | 68% |
| no rendering | 75% |

Rendering ~21%, audio ~10%. The interpreter is the rest, and the sampler says
it is at its floor for this title: `exact.float` ~12% of host time,
`fp.available` 5.9% (19% of all opcodes, and folding it away was measured at
exactly zero -- see above), the cycle charge ~11%, guest loads and stores ~12%.
This title calls or returns every seven guest cycles and its hottest guest
block is a squared-distance between two 3D points, run 37.9 million times in
six guest seconds. There is no single fix left here either.

---

# The native stand-ins: recognising the SDK inside every DOL (2026-08-23)

Every measurement above kept pointing at the interpreter and every
interpreter lever kept measuring zero, so this pass asked a different
question: *which guest code* is the time in? A new profiler column answers it
-- ops attributed per loop, because entry counts overweight short blocks by
orders of magnitude -- and on the heaviest scenes the answer was not the
games' own code. It was the Nintendo SDK, statically linked into every title:

- Melee's worst scene is its THP video decoder: **96% of every interpreted
  opcode** (the Y/U/V Huffman decoders, the IDCT frame driver, the AX reverb
  kernel).
- Star Fox Assault retires 32M `DCFlushRange` cache lines per 12e9 cycles;
  every SDK title leans on the same six cache-range loops.
- Disney skate stages ~20% of its ops through the SDK's paired-single matrix
  kernels.

A microbenchmark (dolvm_bench's `chain`/`par` kernels) sized the tax:
interpretation is **10-13x slower than native on branchy integer code** and
only ~1.1x on exact-float code, whose helpers dominate both arms. So the
lever is not making ops cheaper -- it is not executing them at all where the
code is provably known.

## The mechanism

`DOLVM_OP_HLE`: the recompiler proves a routine is the known SDK code by
comparing **every instruction word** against a stored pattern (a checksum
only narrows a search; a stand-in replaces the exact effect of exact words),
then plants the op at the routine's entry. Two kinds of stand-in:

- **Interpreter helpers** (the cache-range family, both SDK generations):
  hand-written, replacing a homes-flush-per-line loop with one bracket and
  the exact register/CR/charge effects, ending in the same `sc` fast-sync or
  resolved blr. All eight titles on this machine prove 5-6 sites each.
- **Native functions**: the pattern's words are run through DolRecomp's own
  C backend at *app build time* and the generated function ships in the
  interpreter library (src/vm/dolvm_hle_native.c, regenerated by
  tools/dolvm_hle_gen.c). Nothing is generated on a device. A cluster of
  routines lowers as one chunk so calls between them stay native; entries may
  sit mid-routine, so a return that lands on one rejoins native code.

Either kind declines -- `DOLVM_HLE=0`, an unmodeled mode, a helper not built
in -- and the interpreted body that still follows the op runs instead.

## What it is worth (M4 Pro, cpu-time, savestated heavy scenes, A/B same binary)

| scene | off | on | |
|---|---|---|---|
| Melee, THP movie (its worst scene) | 0.957x | **1.971x** | **+106%** |
| Star Fox Assault heavy | 0.691x | 0.706x | +2.2% |
| Disney skate, Olliewood | 1.030x | 1.042x | +1.2% |

(Absolute figures were taken under sustained thermal load; the A/B ratios are
the signal.) Disney's matrix kernels were also tried as natives and measured
**nothing** -- native code that spends itself in the exact-float helpers runs
at the interpreter's speed -- so they were removed, and the rule from the
fusion experiments generalises: **the win is wherever the tax was dispatch
and decode, and nowhere the work itself dominates.**

Correctness: a differential test runs the whole native cluster against its
interpreted body over 160 randomized states and RAM images; the FMV renders
frame-identical; a 100e9-cycle soak shows zero invalid accesses and clean
scene transitions. Charging agrees within the two backends' known difference
(merged superblocks pay a side-exit's skipped cost; the C backend charges the
path taken).

**Before an App Store submission**: the native patterns embed recompiled SDK
instruction sequences in the shipped binary -- unlike the interpreter, which
only ever runs code from the user's own disc. Ship or trim that list
deliberately.

**Adding a variant** (same SDK build linked at another address, or another
title's decoder): extract the words, add a DOLVM_HLE_NATIVE_PATTERN with its
entries to dolvm_hle_native_patterns.inc, rerun dolvm_hle_gen, commit both.
The differential test picks it up automatically via the pattern table.
