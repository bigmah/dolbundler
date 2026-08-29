// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CPUState and dispatch-gate layout the current emission is targeting.
//
// Generated objects carry byte offsets, not field references, so the offsets
// baked into them have to be the *target's* -- and wasm32 is a 32-bit target
// whose pointer-bearing tail does not line up with the host's. Everything here
// forwards to dolnative_target_layout(); it exists so the emitters can say
// which layout they mean without each of them plumbing a triple around.
//
// Set once per emission from the module's triple. Read before it is set, it
// gives the host layout -- right for every target that shares the host's
// pointer size, and wrong in a loud way for one that does not, because the
// static asserts in native_state_layout.h then refuse the module.

#ifndef DOLRECOMP_LLVM_TARGET_LAYOUT_H
#define DOLRECOMP_LLVM_TARGET_LAYOUT_H

#define DOLNATIVE_WITH_DOLIR 1
#include "core/native_state_layout.h"

#include <cstddef>

namespace dolllvm {

void setTargetLayout(unsigned pointer_size);
const DolNativeTargetLayout &targetLayout();

// The DolIR state slots, for the target rather than the host. Only DOWNCOUNT
// lives past the pointers; every other slot is in the prefix, which lays out
// the same everywhere, so those forward to dolnative_state_offset().
size_t targetStateOffset(DolIRStateSlot slot);

}  // namespace dolllvm

#endif
