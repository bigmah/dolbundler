# Mario Party 7: Snow Ride CPU performance

Measured on 2026-09-08 (local time). These are Mac CPU measurements with the
iPhone's LLVM target CPU, 64-instruction chunks, all 129 RELs, and fallback JIT
disabled. They do not establish iPhone frame rate or thermal behavior.

## Active race comparison

| Run pair | Before | After | Change |
|---|---:|---:|---:|
| 1 | 1.070x | 1.130x | +5.6% |
| 2 | 1.040x | 1.165x | +12.0% |
| 3 | 1.025x | 1.140x | +11.2% |

Median paired improvement: **11.2%**, with non-overlapping arms. Order was
ABBAAB, with no compiler or other game running during measurement. Each run
loaded `/tmp/snowride-perf/nav-states/race-early.sav`, a new checkpoint about
14 seconds into the race, and measured eight wall seconds using all four
two-second speed samples. Metal screenshots confirmed the capture was between
12 and 15 seconds on the game clock; both arms stay well before the finish.
Timed measurements used Null video and
no audio. All runs exited normally with zero guest faults, zero hook fallback,
and zero lost or failed code chunks. Each had 58,373 fallback instructions
during startup and zero fallback in subsequent measurement intervals.

Both builds used the existing phone PGO profile and the same compiler options.
The changed idle-loop control flow invalidated 42 of 54,936 object profiles;
those chunks were compiled without their stale function records. The other
54,894 objects were reused unchanged for the after build.

The old 0.68x -> 2.05x comparison used the C backend and discarded the first
12 seconds as warm-up. That save is about 31 seconds into a race won at 33.31
seconds. An initial four-second comparison in this investigation also included
the finish animation and showed +18.6%; it was replaced by the earlier
checkpoint above. Neither finish-scene measurement describes sustained racing.

## Changes

- Keep ordinary DOL address translation and the common single-section REL
  translation inline. The fast dispatch check now uses them directly instead
  of entering the general REL lookup path on every DOL dispatch.
- Match each analyzed idle loop's conditional branch with its load address.
  DolIR has one block per guest instruction; looking for a self-loop on the
  load's block never matched the SDK's load/compare/branch loop. The compiler
  now yields on the proven waiting edge, including across chunk boundaries,
  and executes the non-waiting edge normally.
- Include the exact idle edge in affected object cache/resume keys. Update
  this title's tuning identity so the next phone send rebuilds its module;
  `recompios` also checks the compiler's incremental build before using it.

## Validation

The execution regression failed before the idle fix: it consumed its execution
budget instead of yielding after three guest instructions. Tests now cover a
single pass, wake-up, inverted conditions, a split across chunks, and an
unrelated branch to the same head. Pipeline tests switch idle detection off
and back on to check object-cache and resume invalidation on macOS and iOS.

The recompiler's 22 tests and the runtime's 43 tests pass. The local netplay
test needs loopback socket access. The runtime ABI test's stale version-4
assertion was updated to the version 6 introduced by the earlier REL commit.
The changed runtime also builds for iPhoneOS as an unsigned `core` target.

## Rejected experiment

On the older checkpoint near the finish, chaining ordinary chunk fallthroughs through the existing native gate, with
constant-stack tail calls, reduced performance: 1.135/1.085/1.140x before
against 1.025/0.990/0.990x in three interleaved pairs. All DOL code and all
298 Snow Ride REL chunks were rebuilt for this experiment; unused REL objects
were retained from the baseline. This experiment was removed.

Working logs and benchmark modules are under `/tmp/snowride-perf/` on the
measurement machine. No game bytes, savestates, or generated game code are
included in this record.
