// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DolIR optimizer the bytecode backend runs before lowering.
//
// A native backend can hand raw DolIR to LLVM and let it clean up. The VM has
// no such partner: every DolIR instruction that survives becomes a dispatch,
// and a dispatch is the interpreter's unit of cost. So the difference between a
// usable interpreter and an unusable one is made here, not in the interpreter.
//
// What the IR looks like coming in matters. `dolir_build_chunk` emits one basic
// block per *guest instruction* and re-reads architectural state from scratch in
// each one, because that is the shape a native backend wants -- every guest
// address is trivially a branch target and the host register allocator sorts out
// the redundancy. Left alone it means `addi r3,r3,1; cmpwi r3,10` reloads r3
// twice, spends two block dispatches, materializes two pcs and charges downcount
// twice, for two guest instructions.
//
// Five passes close that gap:
//
//   1. Superblock formation merges a fallthrough chain into one block, so
//      per-block costs are paid once for a whole straight-line run.
//   2. State forwarding turns a read of a slot this block already read or wrote
//      into a reference to the value that is already in a register.
//   3. Constant folding and algebraic simplification collapse the address
//      arithmetic, mask chains and comparison scaffolding the builder emits
//      unconditionally.
//   4. Local value numbering removes what the first three duplicated.
//   5. Dead code elimination drops everything left unused.
//
// Mid-block entry is what makes 2-4 delicate. The chassis can dispatch to any
// guest address in the module -- an exception resumes at the faulting
// instruction, and that instruction is usually in the middle of a superblock --
// so entering at an address must not observe a value that was computed by code
// the entry skipped. The optimizer enforces the rule that makes this safe: a
// value may cross a guest-instruction boundary only if it can be recreated from
// scratch there, meaning it is a constant or it is the current contents of a
// guest state slot. Every such crossing is recorded as a `DolVMRecipe`, and the
// emitter replays the recipes as an entry stub for that address. Values that
// cannot be recreated are dropped at the boundary rather than forwarded.

#ifndef DOLRECOMP_BACKEND_VM_DOLVM_OPT_H
#define DOLRECOMP_BACKEND_VM_DOLVM_OPT_H

#include "ir/dolir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOLVM_RECIPE_NONE = 0,
    DOLVM_RECIPE_CONSTANT,   // re-materialize an immediate
    DOLVM_RECIPE_STATE,      // re-read a CPUState slot that still holds it
} DolVMRecipeKind;

typedef struct {
    DolIRValue value;
    u8 kind;
    u8 slot;
    u8 type;
    u64 immediate;
} DolVMRecipe;

// One per guest instruction in the function, in address order.
typedef struct {
    u32 guest_address;
    u32 block;            // index into function->blocks after merging
    u32 instruction;      // first instruction at or after this address
    u32 recipe_start;
    u32 recipe_count;
    u8 return_target;     // reachable by blr/bctr, so a legal indirect target
} DolVMEntry;

typedef struct {
    DolVMEntry* entries;
    u32 entry_count;
    DolVMRecipe* recipes;
    u32 recipe_count;
    u32 recipe_capacity;
} DolVMLayout;

typedef struct {
    u32 instructions_before;
    u32 instructions_after;
    u32 blocks_before;
    u32 blocks_after;
    u32 state_reads_forwarded;
    u32 state_writes_removed;
    u32 constants_folded;
    u32 values_numbered;
    u32 dead_removed;
    u32 cr_fields_fused;
    u32 boundary_recipes;
} DolVMOptStats;

void dolvm_layout_free(DolVMLayout* layout);

// Optimize one function in place and describe where each guest address landed.
bool dolvm_optimize_function(DolIRFunction* function, DolVMLayout* layout,
                             DolVMOptStats* stats);

void dolvm_stats_add(DolVMOptStats* total, const DolVMOptStats* delta);
void dolvm_stats_report(const DolVMOptStats* stats, const char* label,
                        FILE* out);

#ifdef __cplusplus
}
#endif

#endif
