# Per-title tuning records

`recompios send` profiles every game for four minutes with a generic
button-masher before recompiling it for the phone. That is the floor every
title gets. This directory is the ceiling: what a developer did by hand for the
first title that ran well on an iPhone -- find the scene where it is actually
slow, profile *that*, try the recompiler's settings against it, read what the
chassis had to skip or lose -- made repeatable by `DolBundler/src/tunegame` and
written down here, per title, so every later send of that title on any Mac
starts from it.

```
DolBundler/tuning/
  README.md            this file
  SUMMARY.md           the library at a glance; rewritten by `tunegame all` / `status`
  <ID>/tuning.conf     what to build the phone module with, and what that measured
  <ID>/profile.profdata.xz   the LLVM PGO profile from the deep dive, xz-compressed
  <ID>/report.md       the findings: speed, health, where time goes, leads
  <ID>/play.txt        optional: a hand-written input route for a title the
                       button-masher cannot get into play
```

Everything here is small enough to commit for a hundred titles (a profile is a
few hundred kilobytes). The big things -- the bench scene savestate, the tuned
module, every log -- stay in the working store under
`~/Library/Application Support/DolBundler/tuning/<ID>/`. A finished dive trims
its own raw counters and instrumented modules; `all` also drops each title's
tuned module (170 MB) unless `--keep-modules`, and `tunegame clean` does the
same by hand. `ab --a tuned` rebuilds it from the store's profile in a couple
of minutes when it is needed again. What stays per title is the scene
savestate and three profiles, about 90 MB.

## What a record does

`recompios send` (and the iPhone button) reads `tuning.conf` for a title:

- `PROFILE` is expanded and handed to the phone recompile instead of a fresh
  four-minute collection. The send says `using the profile tuned on <date>`.
- `LLVM_CHUNK_INSTRUCTIONS` sets the function size the recompiler cuts the game
  into, for the phone build and for any profile collection alike. A profile is
  keyed per function and a function is a chunk, so the two have to agree.
- The record's identity is folded into the module's stamp, so a re-tuned title
  is rebuilt on its next send.

If the recompiler has changed since the dive and more than a tenth of the
game's chunks no longer match the profile, the send says so and falls back to
collecting its own. `tunegame status` shows a record as stale when the
recompiler commit it was made against is no longer the current one.

## Making one

```sh
DolBundler/src/tunegame run --disc-id G4QE01              # level 2, 45 min budget
DolBundler/src/tunegame run --disc-id G4QE01 --level 1    # about 20 minutes
DolBundler/src/tunegame all --hours 8                     # overnight, whole library
DolBundler/src/tunegame status
```

The title has to be in the library with its disc extracted (add it in the app,
or `tunegame add --iso game.iso`). Everything runs on this Mac, headless, with
the desktop runtime standing in for the phone (`MODERNGEKKO_NO_FALLBACK_JIT=1`,
speed limiter off, Null video). **The phone is never touched.**

Level 1: instrumented recompile; play from boot for five minutes with scripted
input, a savestate every 20 s and the speed every 2 s; rank the states by the
speed around them and re-bench the slowest three; the heaviest healthy one is
the bench scene; play from it for two more minutes instrumented; merge both
runs into one profile weighted toward the scene; recompile against it; run the
scene with the sampler on; write the record and the report. Every stage is
remembered in `results.json`, so an interrupted run resumes.

Level 2 adds variants, each a recompile and an interleaved A/B on the scene,
until the budget runs out: `generic` (the profile recompios would have
collected alone), `plain` (no profile), `chunk128` and `chunk32` (each with
its own instrumented build and runs, because a profile at one size matches
nothing at another). A variant that wins by 3% with arms that do not overlap
is adopted.

`all` takes the library untuned-first, then stale, then slowest; a title
already past `--target-speed` gets level 1 only. `--agent` runs
`claude -p /tune-games <ID>` after each title whose report needs attention.
Something like this in a terminal at night:

```sh
caffeinate -i DolBundler/src/tunegame all --hours 8 --agent
```

## Reading a report

The speed table is the headline: `tuned` against `generic` says what the deep
dive was worth over what `send` does alone; against `plain` what any profile
is worth. Speeds are x real time on the tuning Mac, phone-shaped; they compare
a title with itself across builds, not titles with each other, and not with the
phone -- the phone is roughly half this Mac per core, so a title under about
1.5x here will miss frame rate there.

**Every difference is by interleaved pairs, and a trailing `?` means the pairs
disagreed on the sign, which is no verdict at all.** The tuning Mac reads the
same module and savestate at two distinct levels -- about 2.8x and about 3.5x
on Mario Soccer -- flat within a run and stepping between runs, sometimes
inside one pair. Thread QoS was the first guess and measured nothing (see
`STATICRECOMP_CPU_QOS`); the cause is open. The record carries `SPEED_RANGE`
for the same reason: a single number from this machine is not a measurement.

`Health` is the counters the chassis prints at shutdown. Faults must be zero
or nothing else in the report is trustworthy. `fallback` and `hook_fb` are
guest instructions the module did not run, which on a phone means the
interpreter. `smc_lost` is guest code a self-modification took away from the
module.

`Where host time goes` is a 200 µs sampler on the CPU thread: `generated` is
the game's own recompiled code; `exact-fp/psq` the floating-point helpers the
generated code calls for the cases it does not inline; `chassis` dispatch and
gate checks; `video` the emulated GPU (with Null video, that is the FIFO and
vertex loading, not a renderer).

`Hottest guest functions` are the LLVM block counts from the merged profile,
named where Dolphin's signature database recognises the SDK routine. One
function far ahead of the rest is either a wait loop or a decoder, and the
disassembly at the end of the report is there to tell which.

`Leads` are heuristics over all of the above, phrased as things to look at.
They are leads, not diagnoses.

## Acting on a lead

Measure before and after, on the same scene, interleaved, back to back:

```sh
tunegame build --disc-id G4QE01 --out /tmp/g4qe01-try --pgo scene --env DOLRECOMP_NO_DATA_SECTIONS=1
tunegame ab --disc-id G4QE01 --a tuned --b /tmp/g4qe01-try/module-build/gG4QE01_recomp.dylib
```

`ab` prints both arms and says `OVERLAP` when the result is not one; `tuned`
as a module name means the store's tuned module. A runtime knob is tried the
same way with `--env` on `ab` (both arms get it) or on `bench`. A change to the recompiler itself is a new `build` and an `ab`;
rebuild `build-dolrecomp-llvm20` first (`ios/build-dolrecomp-llvm20.sh`).

When a change helps a title, put it where every title gets it: the recompiler
or the chassis, not the record. The record is for what the recompiler cannot
know on its own -- the profile, and settings that are a curve with a minimum
rather than a right answer. Then re-run `tunegame run --disc-id <ID> --fresh`
so the record and its numbers describe the new build.

## Dead ends, so nobody walks them twice

- **Idle-skipping the SDK's `PPCHalt`** (`sync; nop; li r3,0; nop; b`). Made
  Mario Strikers freeze mid-match and bought nothing (1.0249x without, 0.9996x
  with). The 2x it looked worth came from a savestate whose guest had already
  died.
- **A savestate whose guest has crashed reads as a huge speedup.** A crashed
  game sits in a wait the chassis skips. `tunegame` re-benches every candidate
  scene for faults and refuses one that reads many times faster than the run's
  median; do the same by hand.
- **Aligning chunk boundaries to function entries.** No measurable gain, and it
  moved which code landed in the SMC hole, which reproduced the same freeze.
- **Native stand-ins for floating-point kernels.** Interpretation was 10-13x
  native on branchy integer code and ~1.1x where the exact-float helpers
  dominated; paired-single matrix kernels as natives measured nothing. Only
  code whose cost is dispatch and decode -- codecs, bit readers, cache loops --
  pays.
- **The renderer and audio are outside everything measured here** (about 15%
  and 6% of a phone's frame on the first title). A title that is fast here and
  slow on the phone is one of those, not the recompiler.
- **Never compare two numbers taken more than a few minutes apart.** This
  machine drifts by up to a quarter under sustained load. `ab` interleaves;
  `bench` alone is a number, not a comparison.
- **Do not use the phone to tune.** Everything the phone would say about the
  CPU path, the Mac says with `MODERNGEKKO_NO_FALLBACK_JIT=1`; the phone is
  someone's phone, in their hand.
