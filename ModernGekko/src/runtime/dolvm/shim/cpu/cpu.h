// SPDX-License-Identifier: GPL-3.0-or-later
//
// DolRecomp's interpreter half asks for "cpu/cpu.h". Inside the chassis that
// has to mean the chassis's CPUState, not the recompiler's own runtime copy:
// the struct the chassis hands to dispatch() is GXRuntime's, and the two differ
// past the last field any bytecode state slot names. This shim sits ahead of
// DolRecomp's own src/ on the include path so the interpreter compiles against
// the layout it will actually be run on.
#ifndef MODERNGEKKO_DOLVM_CPU_SHIM_H
#define MODERNGEKKO_DOLVM_CPU_SHIM_H
#include "core/cpu.h"

// GXRuntime spells the MEM2 window as a literal inside get_ram_ptr rather than
// naming it, so supply the name DolRecomp's interpreter expects. Same window,
// same folding of the uncached alias.
#ifndef WII_MEM2_BASE
#define WII_MEM2_BASE 0x90000000u
#endif
#ifndef WII_MEM2_UNCACHED
#define WII_MEM2_UNCACHED 0xD0000000u
#endif

#endif
