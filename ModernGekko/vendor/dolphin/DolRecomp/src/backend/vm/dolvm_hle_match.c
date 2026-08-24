// SPDX-License-Identifier: GPL-3.0-or-later

#include "backend/vm/dolvm_hle_match.h"

#include "vm/dolvm.h"

#include <stdlib.h>

// One routine the interpreter can stand in for. The pattern is every
// instruction word of the routine as the SDK ships it; nothing is masked,
// because the helper reproduces the exact effect of these exact words --
// register writebacks, the condition field, the ending -- and a variant build
// that moved one register would silently get the wrong effect from a looser
// match. The cost of being strict is only that unrecognised variants stay
// interpreted; the coverage cost of that strictness across the titles to hand
// was zero once both SDK generations' prologues were in the table.

// cmplwi r4,0; blelr; clrlwi r5,r3,27; add r4,r4,r5; addi r4,r4,31;
// srwi r4,r4,5; mtctr r4
#define DOLVM_HLE_PROLOGUE_NEW                                                \
    0x28040000u, 0x4C810020u, 0x546506FEu, 0x7C842A14u, 0x3884001Fu,          \
        0x5484D97Eu, 0x7C8903A6u
// cmplwi r4,0; blelr; clrlwi. r5,r3,27; beq +8; addi r4,r4,32;
// addi r4,r4,31; srwi r4,r4,5; mtctr r4
#define DOLVM_HLE_PROLOGUE_OLD                                                \
    0x28040000u, 0x4C810020u, 0x546506FFu, 0x41820008u, 0x38840020u,          \
        0x3884001Fu, 0x5484D97Eu, 0x7C8903A6u
// <cache op> 0,r3; addi r3,r3,32; bdnz -8
#define DOLVM_HLE_LOOP(op) (op), 0x38630020u, 0x4200FFF8u
#define DOLVM_HLE_DCBF 0x7C0018ACu
#define DOLVM_HLE_DCBI 0x7C001BACu
#define DOLVM_HLE_DCBST 0x7C00186Cu
#define DOLVM_HLE_ICBI 0x7C001FACu
#define DOLVM_HLE_TAIL_SC 0x44000002u, 0x4E800020u
#define DOLVM_HLE_TAIL_BLR 0x4E800020u
#define DOLVM_HLE_TAIL_SYNC 0x7C0004ACu, 0x4C00012Cu, 0x4E800020u

typedef struct {
    u8 id;
    u16 word_count;
    // Nonzero pins the routine to its link address: the native stand-ins bake
    // it into their exception paths, so the same bytes linked elsewhere must
    // stay interpreted until that variant is generated too.
    u32 base;
    const u32* words;
} DolVMHlePattern;

// The big routines the C backend compiles at build time.
#define DOLVM_HLE_NATIVE_PATTERN(id, name, base, count, ...)                  \
    static const u32 k_native_words_##id[] = {__VA_ARGS__};
#define DOLVM_HLE_NATIVE_ENTRY(id, offset)
#include "vm/dolvm_hle_native_patterns.inc"
#undef DOLVM_HLE_NATIVE_PATTERN
#undef DOLVM_HLE_NATIVE_ENTRY

// Where the outside world enters a native pattern: one site is planted per
// entry, and the generated function's switch takes it from there.
typedef struct {
    u8 id;
    u32 offset;
} DolVMHleNativeEntryPoint;

static const DolVMHleNativeEntryPoint k_native_entries[] = {
#define DOLVM_HLE_NATIVE_PATTERN(id, name, base, count, ...)
#define DOLVM_HLE_NATIVE_ENTRY(id, offset) {id, offset},
#include "vm/dolvm_hle_native_patterns.inc"
#undef DOLVM_HLE_NATIVE_PATTERN
#undef DOLVM_HLE_NATIVE_ENTRY
};

#define DOLVM_HLE_NATIVE_ENTRIES                                              \
    (sizeof(k_native_entries) / sizeof(k_native_entries[0]))

#define DOLVM_HLE_PATTERN(name, ...)                                          \
    static const u32 name[] = {__VA_ARGS__};

DOLVM_HLE_PATTERN(k_dc_flush, DOLVM_HLE_PROLOGUE_NEW,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBF), DOLVM_HLE_TAIL_SC)
DOLVM_HLE_PATTERN(k_dc_invalidate, DOLVM_HLE_PROLOGUE_NEW,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBI), DOLVM_HLE_TAIL_BLR)
DOLVM_HLE_PATTERN(k_dc_store, DOLVM_HLE_PROLOGUE_NEW,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBST), DOLVM_HLE_TAIL_SC)
DOLVM_HLE_PATTERN(k_dc_flush_ns, DOLVM_HLE_PROLOGUE_NEW,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBF), DOLVM_HLE_TAIL_BLR)
DOLVM_HLE_PATTERN(k_dc_store_ns, DOLVM_HLE_PROLOGUE_NEW,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBST), DOLVM_HLE_TAIL_BLR)
DOLVM_HLE_PATTERN(k_ic_invalidate, DOLVM_HLE_PROLOGUE_NEW,
                  DOLVM_HLE_LOOP(DOLVM_HLE_ICBI), DOLVM_HLE_TAIL_SYNC)
DOLVM_HLE_PATTERN(k_dc_flush_old, DOLVM_HLE_PROLOGUE_OLD,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBF), DOLVM_HLE_TAIL_SC)
DOLVM_HLE_PATTERN(k_dc_invalidate_old, DOLVM_HLE_PROLOGUE_OLD,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBI), DOLVM_HLE_TAIL_BLR)
DOLVM_HLE_PATTERN(k_dc_store_old, DOLVM_HLE_PROLOGUE_OLD,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBST), DOLVM_HLE_TAIL_SC)
DOLVM_HLE_PATTERN(k_dc_flush_ns_old, DOLVM_HLE_PROLOGUE_OLD,
                  DOLVM_HLE_LOOP(DOLVM_HLE_DCBF), DOLVM_HLE_TAIL_BLR)
DOLVM_HLE_PATTERN(k_ic_invalidate_old, DOLVM_HLE_PROLOGUE_OLD,
                  DOLVM_HLE_LOOP(DOLVM_HLE_ICBI), DOLVM_HLE_TAIL_SYNC)

#define DOLVM_HLE_ENTRY(id, name)                                             \
    {id, (u16)(sizeof(name) / sizeof(name[0])), 0, name}

static const DolVMHlePattern k_patterns[] = {
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_FLUSH_RANGE, k_dc_flush),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_INVALIDATE_RANGE, k_dc_invalidate),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_STORE_RANGE, k_dc_store),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_FLUSH_RANGE_NO_SYNC, k_dc_flush_ns),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_STORE_RANGE_NO_SYNC, k_dc_store_ns),
    DOLVM_HLE_ENTRY(DOLVM_HLE_IC_INVALIDATE_RANGE, k_ic_invalidate),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_FLUSH_RANGE_OLD, k_dc_flush_old),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_INVALIDATE_RANGE_OLD, k_dc_invalidate_old),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_STORE_RANGE_OLD, k_dc_store_old),
    DOLVM_HLE_ENTRY(DOLVM_HLE_DC_FLUSH_RANGE_NO_SYNC_OLD, k_dc_flush_ns_old),
    DOLVM_HLE_ENTRY(DOLVM_HLE_IC_INVALIDATE_RANGE_OLD, k_ic_invalidate_old),
#define DOLVM_HLE_NATIVE_PATTERN(id, name, base, count, ...)                  \
    {id, count, base, k_native_words_##id},
#define DOLVM_HLE_NATIVE_ENTRY(id, offset)
#include "vm/dolvm_hle_native_patterns.inc"
#undef DOLVM_HLE_NATIVE_PATTERN
#undef DOLVM_HLE_NATIVE_ENTRY
};

#define DOLVM_HLE_PATTERNS (sizeof(k_patterns) / sizeof(k_patterns[0]))

const char* dolvm_hle_name(u8 id) {
    switch (id) {
    case DOLVM_HLE_DC_FLUSH_RANGE: return "DCFlushRange";
    case DOLVM_HLE_DC_INVALIDATE_RANGE: return "DCInvalidateRange";
    case DOLVM_HLE_DC_STORE_RANGE: return "DCStoreRange";
    case DOLVM_HLE_DC_FLUSH_RANGE_NO_SYNC: return "DCFlushRangeNoSync";
    case DOLVM_HLE_DC_STORE_RANGE_NO_SYNC: return "DCStoreRangeNoSync";
    case DOLVM_HLE_IC_INVALIDATE_RANGE: return "ICInvalidateRange";
    case DOLVM_HLE_DC_FLUSH_RANGE_OLD: return "DCFlushRange(old)";
    case DOLVM_HLE_DC_INVALIDATE_RANGE_OLD: return "DCInvalidateRange(old)";
    case DOLVM_HLE_DC_STORE_RANGE_OLD: return "DCStoreRange(old)";
    case DOLVM_HLE_DC_FLUSH_RANGE_NO_SYNC_OLD: return "DCFlushRangeNoSync(old)";
    case DOLVM_HLE_IC_INVALIDATE_RANGE_OLD: return "ICInvalidateRange(old)";
#define DOLVM_HLE_NATIVE_PATTERN(pattern_id, pattern_name, base, count, ...)  \
    case pattern_id: return pattern_name;
#define DOLVM_HLE_NATIVE_ENTRY(id, offset)
#include "vm/dolvm_hle_native_patterns.inc"
#undef DOLVM_HLE_NATIVE_PATTERN
#undef DOLVM_HLE_NATIVE_ENTRY
    default: return "?";
    }
}

const u32* dolvm_hle_native_words(u8 id, u32* count, u32* base) {
#define DOLVM_HLE_NATIVE_PATTERN(pattern_id, pattern_name, pattern_base,      \
                                 pattern_count, ...)                          \
    if (id == pattern_id) {                                                   \
        *count = pattern_count;                                               \
        *base = pattern_base;                                                 \
        return k_native_words_##pattern_id;                                   \
    }
#define DOLVM_HLE_NATIVE_ENTRY(id, offset)
#include "vm/dolvm_hle_native_patterns.inc"
#undef DOLVM_HLE_NATIVE_PATTERN
#undef DOLVM_HLE_NATIVE_ENTRY
    (void)id;
    *count = 0;
    *base = 0;
    return NULL;
}

static int compare_sites(const void* left, const void* right) {
    u32 a = ((const DolVMHleSite*)left)->pc;
    u32 b = ((const DolVMHleSite*)right)->pc;
    return a < b ? -1 : (a > b ? 1 : 0);
}

u32 dolvm_hle_match_sections(const LoadedCodeSection* sections,
                             u32 section_count, DolVMHleSite** out) {
    *out = NULL;
    u32 count = 0;
    u32 capacity = 0;
    DolVMHleSite* sites = NULL;
    for (u32 s = 0; s < section_count; s++) {
        const LoadedCodeSection* section = &sections[s];
        if (!section->data || section->size < 4u)
            continue;
        u32 words = section->size / 4u;
        for (u32 i = 0; i < words; i++) {
            u32 first = read_be32(section->data + (size_t)i * 4u);
            for (u32 p = 0; p < DOLVM_HLE_PATTERNS; p++) {
                const DolVMHlePattern* pattern = &k_patterns[p];
                if (first != pattern->words[0])
                    continue;
                if (i + pattern->word_count > words)
                    continue;
                if (pattern->base &&
                    pattern->base != section->address + i * 4u)
                    continue;
                u32 w = 1;
                while (w < pattern->word_count &&
                       read_be32(section->data + (size_t)(i + w) * 4u) ==
                           pattern->words[w])
                    w++;
                if (w != pattern->word_count)
                    continue;
                // A pinned pattern may span several routines; every entry
                // the outside world calls gets its own site. Position
                // independent patterns enter only at their first word.
                u32 entry_first = 0;
                u32 entry_limit = 1;
                if (pattern->base) {
                    entry_first = 0;
                    entry_limit = 0;
                    for (u32 e = 0; e < DOLVM_HLE_NATIVE_ENTRIES; e++)
                        if (k_native_entries[e].id == pattern->id)
                            entry_limit++;
                }
                if (!pattern->base) {
                    if (count == capacity) {
                        capacity = capacity ? capacity * 2u : 8u;
                        DolVMHleSite* grown = (DolVMHleSite*)realloc(
                            sites, (size_t)capacity * sizeof(*sites));
                        if (!grown) {
                            free(sites);
                            return 0;
                        }
                        sites = grown;
                    }
                    sites[count].pc = section->address + i * 4u;
                    sites[count].id = pattern->id;
                    count++;
                } else {
                    for (u32 e = 0; e < DOLVM_HLE_NATIVE_ENTRIES; e++) {
                        if (k_native_entries[e].id != pattern->id)
                            continue;
                        if (count == capacity) {
                            capacity = capacity ? capacity * 2u : 8u;
                            DolVMHleSite* grown = (DolVMHleSite*)realloc(
                                sites, (size_t)capacity * sizeof(*sites));
                            if (!grown) {
                                free(sites);
                                return 0;
                            }
                            sites = grown;
                        }
                        sites[count].pc = section->address + i * 4u +
                                          k_native_entries[e].offset;
                        sites[count].id = pattern->id;
                        count++;
                    }
                }
                (void)entry_first;
                (void)entry_limit;
                break;
            }
        }
    }
    if (count)
        qsort(sites, count, sizeof(*sites), compare_sites);
    *out = sites;
    return count;
}
