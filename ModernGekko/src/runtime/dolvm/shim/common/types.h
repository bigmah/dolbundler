// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same story as cpu/cpu.h: DolRecomp and GXRuntime each ship a "u32 and
// friends" header under the same include guard, so whichever lands first wins
// and the other silently contributes nothing. GXRuntime's is the superset its
// own cpu.h needs, so point every DolRecomp include at that one.
#ifndef MODERNGEKKO_DOLVM_TYPES_SHIM_H
#define MODERNGEKKO_DOLVM_TYPES_SHIM_H
#include "core/types.h"
#endif
