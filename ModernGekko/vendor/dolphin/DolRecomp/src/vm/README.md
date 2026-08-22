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

### The dispatch gate

A chassis checks things on every dispatch -- that the region still matches
guest RAM, that no mod has hooked the address, that its timing slice has
something left -- and an interpreter that followed a call or a return by
itself would skip all of them. So by itself it follows none: a `bl` leaves
exactly as an `EXIT` would, and a `blr` is resolved only inside its own region
and only onto an address a linked branch could return to, which is what the
native backends route locally.

A chassis that wants more installs a gate (`DolVMGate`, via
`dolvm_module_set_gate`): one byte per region saying whether it would itself
dispatch there right now, a pointer to its live slice counter, and a word it
raises exceptions in. With that the interpreter follows calls, tail calls and
indirect branches into any open region, landing on any entry -- the entry
stubs exist so the chassis can resume anywhere, which makes every one of them
as good a landing site as a dispatch -- and keeps going until the slice is
spent, a closed region is reached, or an exception is waiting. On Melee that
is one dispatch per timing slice instead of one per thirty guest instructions.

Two things about the gate are easy to get wrong and were, both found by the
game wandering down a different path rather than by a test:

- **The budget is read live at every guard, not snapshotted.** Dolphin's
  CoreTiming shortens the slice from inside the hooks the interpreter calls --
  an MMIO write that schedules an event due before the slice was going to
  end -- and an interpreter that ran to the old end delivered every such event
  late. The chassis also flushes the charge accumulated so far at the top of
  every hook, so an event is scheduled against the exact guest moment and not
  the start of the dispatch.
- **Exceptions are checked at resolved edges.** An interrupt the guest raised
  with a store is delivered by the time the function that raised it returns,
  without the interpreter having to poll a word on every back edge.

ModernGekko's chassis publishes its gate through
`StaticRecompModuleSource::publish_gate`; see `GXRuntime/include/core/
dispatch_gate.h` for the shape and `StaticRecompCore::RefreshChunkOpen` for
what closes a region.

### Idle loops

`lwz rX,d(rY); cmp[l]wi rX,n; bc` back to its own head is a guest waiting for
an interrupt handler or the other processor to change one word of memory, and
nothing the loop does can end the wait. Dolphin's JITs skip it -- they charge
the rest of the slice and let the next event run -- and so does this: the
emitter marks the back edge of a block with exactly that shape (one load, no
stores, nothing written but the loaded register and CR), and the interpreter,
when the edge is taken, charges what is left of the budget and leaves at the
loop head. Counted delay loops and loops with a store are left to run, since
a skipped iteration of those would be a visible one.

It matters more than it sounds: `GXDrawDone`'s wait for the GPU is this loop,
and before it was skipped it was 40% of Melee's guest cycles and half of Mario
Party 4's.

### Waits the loop shape cannot see

That test wants one load, no stores and no call, and plenty of waits have all
three. Disney's Extreme Skate Adventure spends 45% of every frame here:

```
loop:   bl    GXGetGPStatus      ; lhz from 0xCC000000, unpacks five bits
        lbz   r0, overhi
        cmpwi r0, 0
        beq   loop
```

`overhi` is the command processor's FIFO-overflow bit and only a FIFO *write*
can set it, so nothing the loop does can end the wait -- but the loop calls a
function, and that function stores. Run faithfully it was 57% of every opcode
the interpreter executed and a quarter of its wall clock, most of the second
part because the `lhz` leaves RAM and the chassis services it.

So the interpreter recognises the wait at the poll instead of at the loop. It
counts consecutive reads the chassis had to service that come back with the
same value, from the same guest instruction, at the same address; past
`DOLVM_POLL_SPIN` of them the next back edge charges the slice and leaves,
exactly as a recognised idle loop does. Nothing a loop can do while making
progress produces that run: a write the chassis services resets it, and a
`poll_fresh` flag stops a run left behind by a loop that has already exited
from firing on the next loop's first back edge. A wait that has once been
proved is remembered by (pc, address, value), so the threshold is paid once and
not once per timing slice -- worth another 11% on top, because the loop is
re-entered six hundred thousand times over a run.

The value is part of that key on purpose: the read that finally *answers* the
wait does not match the site that recorded the waiting.

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
| `rotl32i.and`, `shl32i.and`, `lshr32i.and`, `ashr32i.and` | a shift or rotate by a constant and the mask that follows it -- `rlwinm`, which is how PowerPC extracts every bitfield there is |

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
opcodes came to a tenth of everything the interpreter executed. And `rlwinm` is
one guest instruction that the builder writes as two, with the second reading
back through the register file what the first just wrote: on Disney skate every
single `rotl32i` executed was followed by the `andi` that consumed it, together
9% of all opcodes.

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
| `DOLRECOMP_VM_DIRECT_CALLS` | `0` lowers intra-module calls to `EXIT` instead of `CALL`, for comparison |
| `DOLRECOMP_VM_HOME_STATE` | `0` leaves the guest registers in `CPUState`, for comparison |

Calls are lowered to `CALL` by default. Whether one is followed is decided at
run time by the chassis's gate (above), and a chassis that installs none gets
the `EXIT` behaviour back, so the module is the same either way and only the
interpreter's permission differs.

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
486 MHz, on an M4 Pro:

| Title | homed registers | + gate, idle loops, build flags | now |
|---|---|---|---|
| Mario Party 4 | 1.69x | 2.94x | **4.06x** |
| The SpongeBob SquarePants Movie | 1.25x | 1.81x | **11.89x** |
| Super Smash Bros Melee | 0.92x | 1.39x | **1.62x** |
| Disney's Extreme Skate Adventure | -- | 1.46x | 1.39x |
| Luigi's Mansion | -- | -- | **6.05x** |
| Star Fox Assault | -- | -- | **34.4x** |

In savestated **heavy scenes**, which is the number that matters: Disney skate
1.023x -> **2.693x**, Melee 0.974x -> **1.341x**, Star Fox Assault 0.451x ->
**1.203x**.

Those are each title's first six billion cycles, which for most of them is
boot -- and the boot column is misleading enough to be worth a warning. Star
Fox Assault's 34x is not a fast game; it is a game that spends its first
seventy guest seconds waiting on the disc, which the interpreter now skips. In
its heavy scene it runs at 1.09x. It
moves by a factor of six on SpongeBob, whose boot is almost entirely hardware
waiting, and by nothing at all on Disney skate, whose wait loop does not run
until the game is playing. Measured instead over a fixed *gameplay* window --
a savestate 82 billion cycles into a run -- Disney skate goes from **1.055x to
2.88x**. Bench the scene you care about.

Where the second column came from, in the order it was found: the dispatch gate
(+5%, +21%, +9%); one indirect branch per handler, which LLVM's tail merger
had folded into a single `br` for the whole loop until `-mllvm
-tail-dup-pred-size` was raised (+3%, +7%, +5%); inlining the MEM1 fast path of
guest loads and stores (+2%, +6%, +4%); bracketing `psq_l`/`psq_st` with the
homes flush only when the access can leave RAM (+2%, 0%, +5%); LTO over the
interpreter and the cpu helpers (Melee +3%); and idle loop skipping (+55%,
+13%, +17%). Folding the landing block's cycle charge into the resolved edge was
tried and *lost* 3%: a dispatch that lands on a `charge` gives the next
indirect branch its own history, and that is worth more than the dispatch.

Measure that way and nothing else. A run of a fixed number of *seconds* measures
whatever scene the game happened to reach, and a faster build reaches a
different one, which reads as several percent in either direction. The bench
counts guest cycles off the timebase rather than off `downcount`, because the
chassis flushes the latter from inside hooks.

Two more rules, each learned the hard way:

**Regenerate the PGO profile after every change to the interpreter, before
believing any number.** A profile whose function hash no longer matches is
dropped silently (`-Wno-profile-instr-out-of-date` sees to that), which costs
6-10% -- larger than most of the wins being measured. The recipe is in
ModernGekko's `CMakeLists.txt`.

**Check the executed op count, not just the speed.** With
`MODERNGEKKO_DOLVM_PROFILE=ON` the interpreter is deterministic to the
instruction from run to run (Melee: 7.94 billion ops over its first six billion
cycles). A change that moves timing granularity shifts that by a fraction of a
percent; a run that executes a wildly different number of ops, or a different
opcode mix, has sent the game down a different path and is a bug wearing a
speedup's clothes -- the live-budget trap above first showed up as a 30% "gain".

What is left is the interpreter proper: about five host cycles per bytecode op
on an M4 Pro, most of it the indirect branch that every op ends in and the
store-to-load round trips through the register file. `DOLRECOMP_VM_CHUNK` no
longer matters much, since the gate resolves across regions; the default stays
at 65536, which keeps the blast radius of a mod's host call to one region.

## Correctness

`test_dolvm` exercises the gate directly: calls followed into open regions and
refused into closed ones, indirect branches landing on addresses that are not
returns, an exception the chassis is holding turning the next resolved edge
into a leave, and a spent budget doing the same.

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
