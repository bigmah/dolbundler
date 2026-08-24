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
    u8 word_count;
    const u32* words;
} DolVMHlePattern;

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
    {id, (u8)(sizeof(name) / sizeof(name[0])), name}

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
    default: return "?";
    }
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
                u32 w = 1;
                while (w < pattern->word_count &&
                       read_be32(section->data + (size_t)(i + w) * 4u) ==
                           pattern->words[w])
                    w++;
                if (w != pattern->word_count)
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
                sites[count].pc = section->address + i * 4u;
                sites[count].id = pattern->id;
                count++;
                break;
            }
        }
    }
    if (count)
        qsort(sites, count, sizeof(*sites), compare_sites);
    *out = sites;
    return count;
}
