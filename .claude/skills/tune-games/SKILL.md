---
name: tune-games
description: Read a title's tuning report from tunegame (DolBundler/tuning/<ID>/report.md), chase its leads with measured A/Bs on the Mac, and commit what helps. Use after `tunegame run`/`all`, when asked to look at a game's report, or as the nightly agent pass (`claude -p "/tune-games <ID>"`).
---

# Tune a game from its report

You are working the leads in a per-title tuning report that `DolBundler/src/tunegame`
wrote. Read `DolBundler/tuning/README.md` first; it explains every section of a
report and lists the dead ends already walked. Then read
`DolBundler/tuning/$ARGUMENTS/report.md` (if no ID was given, read
`DolBundler/tuning/SUMMARY.md` and pick the slowest title marked
`needs-attention`).

## Rules that are not optional

- **Never touch the iPhone.** No `devicectl`, no install, no launch. Every
  measurement is on this Mac through `tunegame bench` / `tunegame ab`, which
  run the desktop runtime phone-shaped (no fallback JIT, limiter off).
- **Nothing counts until `tunegame ab` says the arms are apart.** A number from
  one run, or two runs minutes apart, is not a result. Three interleaved pairs
  is the minimum; `OVERLAP` means "no".
- **A crashed guest reads as a speedup.** If a bench reports faults, or a speed
  many times the report's, stop and re-pick the scene (`tunegame run --disc-id
  <ID> --fresh`) rather than believe it.
- **Kill nothing that is not yours.** If another `moderngekko-run` is alive,
  `tunegame` refuses to bench; find out whose it is before anything else.
- **Fixes go where every title gets them** -- the recompiler
  (`ModernGekko/vendor/dolphin/DolRecomp`) or the chassis
  (`ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp`) -- not
  into the record. The record is for the profile and for settings that are a
  curve with a minimum. After a recompiler change: `ios/build-dolrecomp-llvm20.sh`,
  then `tunegame build`, then `tunegame ab`.
- Do not re-walk the dead ends in the README. If a lead points at one, say so
  in your notes and move to the next.

## The loop

1. Read the report's `Leads`, `Where host time goes`, `Hottest guest functions`
   and the disassembly. Decide which lead is worth the first hour; write one
   sentence saying why.
2. Form a hypothesis with a measurable prediction ("skipping this poll site
   should take the scene from 1.09x to at least 1.2x").
3. Build the variant: a recompiler flag through `tunegame build --env`, a
   runtime knob through `tunegame ab --env`, or a code change followed by
   `ios/build-dolrecomp-llvm20.sh` (recompiler) or `ninja -C ModernGekko/build
   moderngekko-run` (chassis/runtime) and `tunegame build`.
4. `tunegame ab --disc-id <ID> --a tuned --b <variant dylib>` (`tuned` is the store's
   tuned module, rebuilt from its profile if it was cleaned away).
5. If it helps and the arms are apart, keep it: make the code change proper,
   with a comment saying what was measured, run the relevant tests
   (`ctest --test-dir ModernGekko/build -R staticrecomp` at least), re-run
   `tunegame run --disc-id <ID> --fresh` so the record's numbers describe the
   new build, and commit the code and the record together with the numbers in
   the message. If it does not help, write what was tried and what it read
   into `DolBundler/tuning/<ID>/notes.md` so it is not tried again.
6. Stop when the budget is spent or the leads are exhausted. End with a short
   summary: what moved, by how much, what did not, what the next person should
   try first.

## Things worth knowing about this project

- Speeds are x real time on this Mac, phone-shaped. The phone is about half
  this Mac per core; under ~1.5x here a title misses frame rate there.
- The chassis already skips polls it can prove (the `poll_reads` counter):
  sixteen consecutive same-value reads from the same guest instruction, of a
  read it services -- a hardware register. A wait on plain RAM (the SDK
  scheduler's `lwz r0,-18584(r13); cmplwi; beq` on the run-queue bits, which
  is 64% of Mario Soccer's host time) is done inline by generated code and the
  chassis never sees it; that one needs idle recognition in the recompiler's
  loop guard, the shape Dolphin's `IsBusyWaitLoop` recognises. The report's
  lead says which kind it found.
- `fallback > 0` with no fallback JIT means the phone would interpret those
  instructions one at a time. The run log names the ranges.
- One rewritten guest instruction takes a whole chunk from the module
  (`smc_lost`). At 64-instruction chunks that is 256 bytes; more than that means
  something rewrites code past the SDK's one-word patch.
- The `exact-fp/psq` helpers are the slow path of floating point the generated
  code did not inline; a title heavy there is hitting FPSCR/denormal/quantise
  cases, and the `by symbol` list in the store's `diagnose.log` says which.
- Memory notes for this project live outside the repo and have more history on
  every one of these; the README's dead-ends list is the distilled version.
