// SPDX-License-Identifier: GPL-3.0-or-later
//
// Proving an SDK routine is present. Every GameCube title statically links the
// same SDK, so its hottest leaves are the same instruction words in every DOL;
// a routine is proved by comparing every word of a candidate against the
// stored pattern, not by a checksum, because a helper stands in for the exact
// architectural effect of the exact sequence and nothing looser is safe to
// replace. A title whose SDK build differs by one word simply keeps
// interpreting that routine.

#ifndef DOLRECOMP_BACKEND_VM_DOLVM_HLE_MATCH_H
#define DOLRECOMP_BACKEND_VM_DOLVM_HLE_MATCH_H

#include "analysis/code_section.h"
#include "backend/vm/dolvm_emit.h"
#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Scan every section for known routines. Returns the sites sorted by pc in
// *out (caller frees), and the count. A failed allocation returns 0 sites,
// which only means nothing gets accelerated.
u32 dolvm_hle_match_sections(const LoadedCodeSection* sections,
                             u32 section_count, DolVMHleSite** out);

// The name a site's id stands for, for build-time reporting.
const char* dolvm_hle_name(u8 id);

#ifdef __cplusplus
}
#endif

#endif
