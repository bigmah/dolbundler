// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the differential test puts each opcode. One two-instruction function
// per opcode -- the opcode itself, then a blr -- spaced far enough apart that a
// branch out of one lands outside every region, which is what makes both
// backends hand it back to the caller identically.

#ifndef DOLRECOMP_TESTS_DOLVM_DIFF_LAYOUT_H
#define DOLRECOMP_TESTS_DOLVM_DIFF_LAYOUT_H

#include "../src/frontend/decoder.h"
#include "dolrecomp_opcode_table.h"

#define DOLVM_DIFF_BASE 0x80200000u
#define DOLVM_DIFF_STRIDE 0x40u
#define DOLVM_DIFF_RETURN 0x80400000u

// Straight-line opcodes are also chained into groups, so that superblock
// formation actually has something to merge and every address inside a merged
// block can be entered and compared.
#define DOLVM_DIFF_GROUP_BASE 0x80300000u
#define DOLVM_DIFF_GROUP_STRIDE 0x100u
#define DOLVM_DIFF_GROUP_SIZE 8u

static u32 dolvm_diff_address(int index) {
    return DOLVM_DIFF_BASE + (u32)index * DOLVM_DIFF_STRIDE;
}

static u32 dolvm_diff_group_address(u32 group) {
    return DOLVM_DIFF_GROUP_BASE + group * DOLVM_DIFF_GROUP_STRIDE;
}

// Opcodes where dolir_build_chunk and the C emitter already disagree, before
// anything reaches the bytecode. Each is a property of the shared IR and shows
// up in the LLVM backend identically, so excluding them keeps this test a check
// on the lowering rather than a restatement of a difference it did not
// introduce. The IR for every one of these was read to confirm the difference
// is upstream of the VM.
static bool dolvm_diff_ir_path_differs(PPCOpcode op) {
    switch (op) {
    // The C emitter hands these to the chassis interpreter; the IR calls
    // ppc_cache_control instead, so the two take different routes by design.
    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI:
        return true;
    // The C emitter inlines stwcx. and compares the reservation address
    // exactly; the IR calls ppc_stwcx_op, which also checks alignment and can
    // raise where the inline form cannot.
    case PPC_OP_STWCX:
        return true;
    // The C emitter sets or clears the named FPSCR bit and stops. The IR calls
    // ppc_mtfsb0_op / ppc_mtfsb1_op, which additionally recompute the
    // summary-exception bits.
    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
        return true;
    // The C emitter round-trips each half through the single-precision format,
    // as the paired registers hold singles. The IR negates or clears the sign
    // of the doubles directly, which differs for a double that is not already
    // an exact single.
    case PPC_OP_PS_NEG:
    case PPC_OP_PS_ABS:
    case PPC_OP_PS_NABS:
        return true;
    default:
        return false;
    }
}

#endif
