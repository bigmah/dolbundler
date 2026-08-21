// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_VM_DOLVM_INTERP_H
#define DOLRECOMP_VM_DOLVM_INTERP_H

#include "cpu/cpu.h"
#include "vm/dolvm.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Execute the bytecode covering `address`. Mirrors StaticRecompModuleDesc's
// dispatch contract: returns 1 when the address was covered and executed, with
// ctx->pc holding the next guest pc, and 0 when the module does not cover it.
int dolvm_dispatch(const DolVMModule* module, CPUState* ctx, u32 address);

#ifdef DOLVM_PROFILE
// Executed opcode histogram, most frequent first. Only present in a build
// configured with DOLVM_PROFILE.
void dolvm_profile_report(FILE* out);
#endif

#ifdef __cplusplus
}
#endif

#endif
