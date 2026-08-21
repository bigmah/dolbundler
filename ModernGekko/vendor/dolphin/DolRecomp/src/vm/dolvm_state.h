// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where each guest state slot lives inside CPUState, and a fingerprint of that
// whole mapping.
//
// A .dvm bakes raw CPUState byte offsets into its state loads and stores, so a
// module is only safe to run against a CPUState laid out the way the emitter
// saw it. `sizeof(CPUState)` alone does not settle that: the recompiler's own
// runtime and the chassis agree on every field a slot names and still differ
// past the last of them, so a size check would reject a module that is in fact
// compatible -- and would also pass a module whose fields had merely been
// reordered. The fingerprint below covers exactly the mapping that matters.

#ifndef DOLRECOMP_VM_DOLVM_STATE_H
#define DOLRECOMP_VM_DOLVM_STATE_H

#include "common/types.h"
#include "cpu/cpu.h"
#include "ir/dolir.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline u32 dolvm_state_offset(u32 slot) {
    if (slot <= DOLIR_STATE_GPR31)
        return (u32)offsetof(CPUState, gpr) + 4u * (slot - DOLIR_STATE_GPR0);
    if (slot >= DOLIR_STATE_FPR0 && slot <= DOLIR_STATE_FPR31)
        return (u32)offsetof(CPUState, fpr) + 8u * (slot - DOLIR_STATE_FPR0);
    if (slot >= DOLIR_STATE_PS1_0 && slot <= DOLIR_STATE_PS1_31)
        return (u32)offsetof(CPUState, ps1) + 8u * (slot - DOLIR_STATE_PS1_0);
    if (slot >= DOLIR_STATE_SR0 && slot <= DOLIR_STATE_SR15)
        return (u32)offsetof(CPUState, sr) + 4u * (slot - DOLIR_STATE_SR0);
    if (slot >= DOLIR_STATE_GQR0 && slot <= DOLIR_STATE_GQR7)
        return (u32)offsetof(CPUState, gqr) + 4u * (slot - DOLIR_STATE_GQR0);
    switch (slot) {
    case DOLIR_STATE_PC: return (u32)offsetof(CPUState, pc);
    case DOLIR_STATE_LR: return (u32)offsetof(CPUState, lr);
    case DOLIR_STATE_CTR: return (u32)offsetof(CPUState, ctr);
    case DOLIR_STATE_CR: return (u32)offsetof(CPUState, cr);
    case DOLIR_STATE_XER: return (u32)offsetof(CPUState, xer);
    case DOLIR_STATE_FPSCR: return (u32)offsetof(CPUState, fpscr);
    case DOLIR_STATE_MSR: return (u32)offsetof(CPUState, msr);
    case DOLIR_STATE_SRR0: return (u32)offsetof(CPUState, srr0);
    case DOLIR_STATE_SRR1: return (u32)offsetof(CPUState, srr1);
    case DOLIR_STATE_DAR: return (u32)offsetof(CPUState, dar);
    case DOLIR_STATE_DSISR: return (u32)offsetof(CPUState, dsisr);
    case DOLIR_STATE_EAR: return (u32)offsetof(CPUState, ear);
    case DOLIR_STATE_HID2: return (u32)offsetof(CPUState, hid2);
    case DOLIR_STATE_TIMEBASE: return (u32)offsetof(CPUState, timebase);
    case DOLIR_STATE_EXCEPTION: return (u32)offsetof(CPUState, exception);
    case DOLIR_STATE_PROGRAM_EXCEPTION:
        return (u32)offsetof(CPUState, program_exception);
    case DOLIR_STATE_RESERVE_ADDR: return (u32)offsetof(CPUState, reserve_addr);
    case DOLIR_STATE_RESERVE_VALID: return (u32)offsetof(CPUState, reserve_valid);
    case DOLIR_STATE_DOWNCOUNT: return (u32)offsetof(CPUState, downcount);
    default: return 0;
    }
}

// FNV-1a 32 over every slot's offset, in slot order. Two runtimes that agree on
// this agree on everything a .dvm can reach by offset.
static inline u32 dolvm_state_layout_hash(void) {
    u32 hash = 2166136261u;
    for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
        u32 offset = dolvm_state_offset(slot);
        for (u32 byte = 0; byte < 4u; byte++) {
            hash ^= (offset >> (byte * 8u)) & 0xFFu;
            hash *= 16777619u;
        }
    }
    return hash;
}

#ifdef __cplusplus
}
#endif

#endif
