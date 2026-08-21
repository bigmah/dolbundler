# DolVM

A DolVM module is a recompiled game that is data rather than code.

The C and LLVM backends both end at host machine code, and the host jumps into
it. Some hosts are not allowed to do that: an App Store binary may not generate
executable pages or load code it did not ship with, which rules out both
backends on iOS no matter how the recompilation is packaged. DolVM takes the
same DolIR those backends consume and lowers it to a register machine's
instruction stream instead. The recompiler still runs ahead of time and still
does the analysis; the app just interprets the result.

```
main.dol ──▶ decoder ──▶ DolIR ──┬──▶ C backend    ──▶ .c  ──▶ host compiler ──▶ machine code
                                 ├──▶ LLVM backend ──▶ .o                    ──▶ machine code
                                 └──▶ VM backend   ──▶ .dvm ──▶ interpreter  ──▶ no code at all
```

## Layout

| Path | Role |
|---|---|
| `src/vm/dolvm.h` | the instruction set and the container format |
| `src/vm/dolvm_state.h` | where each guest state slot lives, and its fingerprint |
| `src/vm/dolvm_module.c` | loading, and every check the interpreter then skips |
| `src/vm/dolvm_interp.c` | the interpreter |
| `src/backend/vm/dolvm_opt.c` | the DolIR passes that make interpretation affordable |
| `src/backend/vm/dolvm_emit.c` | DolIR to bytecode, including register allocation |
| `src/backend/vm/dolvm_pipeline.c` | the `--backend vm` arm of the pipeline |

`src/vm/` depends on `cpu/` and nothing else. That is the half that ships inside
an app; the rest is build-time only.

## Running one

A `.dvm` carries everything a chassis needs to accept it as a recompiled module
and to police it the way it polices a native one: the disc ID, the guest ranges
it covers, the self-modifying-code candidate sites, and the FNV-1a 64 hash of
each region's original text so guest RAM can be checked against the code the
module was built from. ModernGekko opens one through
`src/runtime/dolvm/` and hands the chassis an ordinary module descriptor whose
`dispatch` runs the interpreter; nothing downstream can tell the difference.

The interpreter is compiled against the chassis's own `CPUState`, not the
recompiler's. The two agree on every field a bytecode state access can name and
differ past the last of them, so `sizeof(CPUState)` is the wrong thing to check.
`dolvm_state_layout_hash()` covers exactly the slot-to-offset mapping a module
bakes in, and the loader compares that instead.

## Homed guest state

The guest's registers live in the VM's. All 32 general purpose registers, plus
LR, CTR and XER, have a fixed *home* at the top of the register file
(`regs[220..254]`, laid out in `dolvm.h`); the interpreter fills them from
`CPUState` when a dispatch begins and writes them back when it ends, and a
module says it was lowered that way with `DOLVM_FLAG_HOMED_STATE`.

The point is not that a register file is faster than a struct field -- both are
memory. It is that the *opcodes* disappear. A guest register access stops being
a dispatched load or store and becomes an operand of the instruction that wanted
it, and a write to a guest register stops being a store and becomes the
destination the producing instruction computes into. On the SpongeBob movie that
was 37% of every op the interpreter executed; measured end to end it is worth
7-13% depending on the title.

Everything else architectural -- the floating point registers, CR, MSR, the
segment and quantization registers -- stays in `CPUState` and is read and
written per access, because either the traffic does not justify a home or the
fused opcodes read it out of `CPUState` themselves.

Three rules keep it correct, and each was caught by a test rather than by
reasoning:

- **A helper that reaches into `CPUState` for something a home is holding needs
  it flushed first and refilled after.** Which helpers those are is decided one
  by one (`DOLVM_HOMES_OUT`/`IN`) rather than by bracketing every call:
  `fp.available` runs tens of millions of times a second and touches nothing
  homed, while `mfspr` reads LR and CTR by number. The guest memory slow path
  counts too, because the chassis services it and may do anything with the state
  while it does.
- **A state write may only be folded onto the instruction that computed the
  value if that instruction cannot fail.** The IR puts the write after the raise
  check on purpose, so a faulted guest instruction leaves the register alone;
  `eciwx` into a disabled EAR is the case `test_dolvm_diff` catches. A guest load
  is the exception, and the important one -- its handler writes the destination
  only when the access succeeded.
- **Everything leaves through one flush.** The dispatch loop has three dozen
  exits and they all `goto leave`. It is a label rather than a wrapper function
  because a wrapper is a second stack frame per dispatch, and that alone cost
  more than homing gained on Melee.

Narrowing the home set to the registers the EABI makes hot -- r0-r7 and
r24-r31, three quarters of all register traffic on every title measured --
halves the fill and the flush and is 4-10% *slower* on all three. The copy is a
vector run over contiguous memory with nothing depending on it, so it costs far
less than its instruction count suggests, while every slot left out goes back to
a load and a store per access in exactly the loops that mattered.

## What the optimizer is for

`dolir_build_chunk` emits one basic block per *guest instruction* and re-reads
architectural state from scratch in each one. That is the right shape for a
native backend, which gets a host register allocator to clean up after it. It is
the wrong shape for an interpreter, where every surviving DolIR instruction is a
dispatch. Five passes close the gap:

1. **Condition-register fusion.** Every compare and record-form instruction ends
   with a CR field update that the builder writes out longhand -- 29 DolIR
   instructions. One opcode replaces all of it.
2. **Superblock formation.** A fallthrough chain becomes one block, so the cycle
   charge, the pc materialization and the block dispatch are paid once per
   straight-line run instead of once per guest instruction.
3. **State forwarding.** A read of a slot the block already read or wrote becomes
   a reference to the value already in a register.
4. **Folding, simplification and local value numbering**, over the address
   arithmetic, mask chains and comparison scaffolding the builder emits
   unconditionally.
5. **Dead code elimination.**

Lowering then folds constants into immediate operand forms, folds address adds
into load and store displacements, resolves every access to a homed slot to its
register, and recycles the rest at last use.

Superblock formation is bounded by how many values a block defines -- two for a
vector, one for anything else, capped at the block-local part of the register
file. Those registers have no spill area, so a merge that outran them would have
nowhere to go; the bound is roughly ten times what a merged block actually
reaches, so it only clips the pathological runs.

## Fused forms

Dispatch is what an interpreter spends its time on, so the ISA has opcodes that
stand in for whole sequences the builder writes out longhand. Each does the same
work as the sequence it replaces and costs one dispatch instead of several:

| Opcode | Replaces |
|---|---|
| `jmp.if.cr` | the CR read, the bit mask, the compare against zero and the branch that `bc` becomes |
| `cmp.state.i`, `cmp.state`, `set.cr.fieldi` | a compare that reads its operands out of `CPUState` or carries the comparand inline, rather than through registers |
| `load.mem.state`, `store.mem.state`, `load.mem.to.state`, `store.mem.from.state` | a guest load or store whose address base, destination or source is a state slot -- `lwz rD,off(rA)` end to end |
| `jmp.guard` | the back edge's budget check and the jump it always preceded |
| `jmp.charge`, `jmp.if.cr.charge` | a branch that also pays the cycle charge its target block opens with, and lands past it |
| `jmp.if.cr.guard` | a loop's entire back edge -- the test, the budget guard and the charge -- which a loop pays on every iteration |
| `cmp.jmp.if.cr`, `.charge`, `.guard` | a compare against a constant *and* the branch that reads the field it just wrote |
| `supervisor` | the MSR read, mask, compare and raise every privileged instruction opens with |

A fused branch is also allowed to name its target block directly instead of
jumping over the other edge's code, which takes the unconditional jump off the
taken path.

Two of these are worth singling out. `cmpwi rX,n; bne` is the shape of every
counted loop and every search loop there is, and as two opcodes the branch had
to read the condition register back out of `CPUState` one instruction after the
compare stored it -- a store-to-load round trip on the loop's carried
dependency. And every privileged instruction opens with a check that the guest
is in supervisor mode, which the builder writes out longhand; a game's operating
system runs `mfmsr`/`mtmsr` around every interrupt mask it takes, so those four
opcodes came to a tenth of everything the interpreter executed.

What pays and what does not is worth stating, because it is not what counting
opcodes suggests, and the rule has held through every change since. **Removing a
dispatch is worth little; removing the work under it is worth a lot.** Folding a
guest load's state write into the load removed a tenth of all dispatches and
changed nothing measurable, because the store still happened and a dispatch that
follows its predecessor in the instruction stream is predicted and nearly free.
Folding a loop's back edge removed fewer dispatches and was worth 5.5%, because
the one it removed was a taken branch. Homing the guest registers removed the
same state opcodes as the first of those *and* the loads and stores under them,
and was worth 7-13%. As a rule of thumb on these three titles, cutting the
executed opcode count by 4% buys about 1% of wall clock unless memory traffic or
a dependency chain goes with it.

Measured as guest cycles retired per second of wall clock against the same title
lowered without them, the set is worth about 29% on Mario Party 4.

On Melee's `main.dol`: 6.65M DolIR instructions become 3.69M, 970,568 blocks
become 233,582, and the module holds 2.40M bytecode instructions -- 2.5 per
guest instruction, against 4.2 before guest state was homed. Lowering the whole
title takes about six seconds.

## Mid-block entry

The chassis can dispatch to any guest address in the module. An exception
resumes at the faulting instruction, and that instruction is usually in the
middle of a superblock. So entering at an address must not observe a value
computed by code the entry skipped.

The rule that makes this safe: a value may cross a guest-instruction boundary
only if it can be recreated from scratch there -- it is a constant, or it is the
current contents of a guest state slot. Every crossing is recorded as a recipe,
and the emitter replays the recipes as an entry stub. Values that cannot be
recreated are dropped at the boundary rather than forwarded.

Two corollaries are easy to get wrong, and both were caught by the differential
test rather than by reasoning:

- **One slot per value.** A value can sit in two slots at once -- `mfspr r10,lr`
  leaves it in LR and in GPR10 -- and forwarding will then justify one later use
  by "LR holds it" and another by "GPR10 holds it". Entering below the code that
  made them equal, the two slots hold unrelated values and no single reload
  satisfies both uses. So only the most recent association is kept.
- **A store of what the slot already holds is only dead within one guest
  instruction.** Across a boundary that store is the thing making the two equal,
  and an entry below the code that set them up would skip it.

`test_dolvm.c` enters a merged block at every address in it and checks each tail
against a hand model of the same program; `test_dolvm_diff.c` does the same
against the C backend, over 25 merged blocks built from the decoder's whole
opcode table. `test_dolvm_diff --no-home-state` runs the corpus again with the
guest registers left in `CPUState`, because every slot that is not homed still
uses that path and a change that breaks only one of the two is the easy mistake
to make.

## Using it

```sh
dolrecomp --gamecube --backend vm main.dol out.c   # writes out.dvm
dolvm_dis out.dvm --code                           # read the bytecode back
dolvm_bench                                        # interpreter vs generated code
```

| Variable | Effect |
|---|---|
| `DOLRECOMP_VM_CHUNK` | guest instructions per region (default 65536); see Speed |
| `DOLRECOMP_VM_DIRECT_CALLS` | `1` resolves intra-module calls inside the interpreter |
| `DOLRECOMP_VM_HOME_STATE` | `0` leaves the guest registers in `CPUState`, for comparison |

Direct calls are off by default for the same reason the C backend's are: they
bypass whatever the chassis checks on dispatch, including host-call interception
and self-modifying-code retirement.

## Speed

`dolvm_bench` runs identical guest programs through the C backend's generated
code and through the interpreter -- the native arm is emitted by the C backend
at build time, so both arms are provably the same instructions. On an Apple
silicon Mac the interpreter comes to 1.48x the wall time of generated code,
down from 1.80x before guest state was homed.

What that comes to on real games is a different number, because a game is not a
kernel: it branches constantly and its working set is the whole title. The
figure to use is throughput -- cpu seconds to retire a fixed number of guest
cycles, which `MODERNGEKKO_DOLVM_BENCH` reports and which a busy machine barely
moves. Over each title's first six billion guest cycles, against the Gekko's
486 MHz:

| Title | before | now |
|---|---|---|
| Mario Party 4 | 1.39x | **1.69x** |
| The SpongeBob SquarePants Movie | 1.02x | **1.25x** |
| Super Smash Bros Melee | 0.84x | **0.92x** |

Measure that way and nothing else. A run of a fixed number of *seconds* measures
whatever scene the game happened to reach, and a faster build reaches a
different one, which reads as several percent in either direction.

**Regenerate the PGO profile after every change to the interpreter, before
believing any number.** A profile stale by one new opcode is worth about -10%,
which is larger than most of the wins being measured; twice during this work it
turned a real gain into an apparent regression. The recipe is in ModernGekko's
`CMakeLists.txt`.

Melee is the one with room left, and the reason is not its opcode mix. It ends
a dispatch every 26 guest cycles, against 90 for Mario Party 4, so it pays the
fixed cost of a dispatch -- the home fill and flush, the prologue, the entry
lookup -- three times as often over the same work. Nine tenths of those
dispatches are an indirect branch whose target is in another region, which the
interpreter will not resolve in place: a region is the unit the chassis verifies
against guest RAM and the unit it drops to the fallback interpreter when a mod
patches inside it, so jumping between them behind its back would skip both.

`DOLRECOMP_VM_CHUNK` decides how large a region is, and it is the largest lever
left. Raising it from the default 65536 guest instructions is worth, per title:

| Title | 65536 (default) | 262144 | 1048576 |
|---|---|---|---|
| Mario Party 4 | 1.69x | 1.71x | 1.73x |
| The SpongeBob Movie | 1.25x | 1.35x | 1.36x |
| Melee | 0.92x | 0.97x | **1.03x** |

It is not free. A larger region is a larger blast radius for a mod: one host
call anywhere inside one drops the whole thing to the fallback interpreter, and
at 1048576 that is the entire title. Recompilation also loses its parallelism --
Melee goes from seconds to about a minute. The default stays where it is; the
knob is there for a build that ships without mod support.

## Correctness

`test_dolvm_diff` runs every opcode the decoder knows through both backends on
identical randomized state and compares the whole architectural state plus a
128 KiB memory window, twice over: once per opcode, and again with the
straight-line opcodes chained into merged blocks entered at every address inside
them. The C backend is the shipping one, so it defines correct.

One thing the differential test cannot cover is the privilege trap: the C
backend does not emit the supervisor check the IR builder puts in front of every
privileged instruction, so the two arms are entitled to disagree about it. The
bytecode backend folds that check into a single opcode, and `test_dolvm.c`
checks it against a hand model from user mode instead.

Twenty-seven opcodes are excluded, each for a reason recorded in
`tests/dolvm_diff_layout.h`: seventeen the IR builder does not lower at all
(they become fallbacks, which the test's stub interpreter cannot match against
the C backend's inline code), and ten where `dolir_build_chunk` and the C
emitter already disagree upstream of any backend -- the LLVM backend inherits
those identically.

One thing neither test reaches, and which a real game did: a constant that
reaches a later guest instruction through a state slot is not a constant there.

```
    li    r0, 20
    divwu r3, r3, r0     <- folds the divide-by-zero guard away
```

Forwarding is right to say the divide's second operand is the value `li` wrote.
It is not right to keep calling that value 20 past the end of the `li`, because
the chassis may enter at the `divwu` with `r0` holding something else, and the
guard has to still be there when it does. So a value crossing a guest
instruction boundary in a slot stops being a known constant at the boundary: it
becomes whatever the slot holds, which is what the entry recipe reloads.
