// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lowering from optimized DolIR to a DolVM bytecode module.

#ifndef DOLRECOMP_BACKEND_VM_DOLVM_EMIT_H
#define DOLRECOMP_BACKEND_VM_DOLVM_EMIT_H

#include "backend/vm/dolvm_opt.h"
#include "ir/dolir.h"
#include "vm/dolvm.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Lower intra-module linked branches and tail calls to CALL rather than
    // EXIT, naming the target's region so a gated interpreter can follow them
    // in place. Without a gate a CALL leaves exactly as the EXIT would have,
    // so this is safe to leave on; the pipeline does.
    bool direct_calls;
    // Hold the guest's general purpose registers, LR and CTR in the VM's own
    // register file for the length of a dispatch rather than reading and
    // writing CPUState per access. On by default; the switch exists so the
    // same title can be lowered both ways and timed.
    bool home_state;
    u32 entry_point;
    // Six-character disc ID, so a chassis can refuse a module built for another
    // game. Empty is allowed; a chassis that checks will then reject it.
    const char* game_id;
    // Self-modifying-code candidate sites, end-exclusive, sorted. Passed
    // through to the container for the chassis to enforce.
    const DolVMRange* smc_ranges;
    u32 smc_count;
    // FNV-1a 64 of the original guest text over [start, end), used by the
    // chassis to notice that RAM no longer holds the code this was built from.
    // Regions are hashed one by one because the emitter, not the caller, is the
    // only thing that knows where the region boundaries ended up.
    bool (*hash_guest_range)(void* user, u32 start, u32 end, u64* out);
    void* hash_user;
} DolVMEmitOptions;

// Optimizes `module` in place, then lowers it. On success `*image`/`*size`
// hold a complete .dvm container the caller frees.
bool dolvm_build_module(DolIRModule* module, const DolVMEmitOptions* options,
                        void** image, size_t* size, DolVMOptStats* stats,
                        FILE* diagnostics);

bool dolvm_write_module(const void* image, size_t size, const char* path,
                        FILE* diagnostics);

#ifdef __cplusplus
}
#endif

#endif
