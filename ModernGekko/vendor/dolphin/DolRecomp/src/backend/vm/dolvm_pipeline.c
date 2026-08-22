// SPDX-License-Identifier: GPL-3.0-or-later
//
// The --backend vm arm of the pipeline: decode, build DolIR, optimize, lower,
// and write one .dvm container for the whole title.
//
// There is no chunking pressure here of the kind the C backend has. Chunks
// exist so a C compiler is not handed a million-line function; the bytecode
// emitter has no such limit, and larger regions are strictly better for the
// interpreter, because an indirect branch that lands inside the region it
// started in is resolved without a chassis round trip. The remaining reason to
// chunk at all is optimizer working set, so regions are large but bounded.

#include "backend/vm/dolvm_emit.h"
#include "backend/vm/dolvm_pipeline.h"
#include "analysis/embedded_data.h"
#include "analysis/smc.h"
#include "ir/dolir_builder.h"

#include <stdlib.h>
#include <string.h>

#define DOLVM_DEFAULT_CHUNK_INSTRUCTIONS 65536u

static u32 chunk_instructions(void) {
    const char* configured = getenv("DOLRECOMP_VM_CHUNK");
    if (!configured || !configured[0])
        return DOLVM_DEFAULT_CHUNK_INSTRUCTIONS;
    char* end = NULL;
    unsigned long value = strtoul(configured, &end, 0);
    if (end == configured || *end || value < 16u || value > 4u * 1024u * 1024u) {
        fprintf(stderr,
                "warning: DOLRECOMP_VM_CHUNK must be 16..4194304; using %u\n",
                DOLVM_DEFAULT_CHUNK_INSTRUCTIONS);
        return DOLVM_DEFAULT_CHUNK_INSTRUCTIONS;
    }
    return (u32)value;
}

// On unless asked otherwise. A module lowered this way carries CALL where it
// would otherwise carry EXIT; whether the interpreter follows one is decided at
// run time by the chassis's gate, and a chassis without one gets the EXIT
// behaviour back. So the switch costs nothing to leave on, and `0` exists to
// lower a title the old way for comparison.
static bool direct_calls_enabled(void) {
    const char* configured = getenv("DOLRECOMP_VM_DIRECT_CALLS");
    return !configured || configured[0] != '0';
}

// On unless asked otherwise. The switch is here so the same title can be
// lowered both ways and the difference measured, which is the only way to keep
// a claim about what homing is worth honest.
static bool home_state_enabled(void) {
    const char* configured = getenv("DOLRECOMP_VM_HOME_STATE");
    return !configured || configured[0] != '0';
}

static bool make_module_path(const char* output_path, char* out, size_t size) {
    size_t length = strlen(output_path);
    size_t stem = length;
    for (size_t i = length; i-- > 0;) {
        if (output_path[i] == '/' || output_path[i] == '\\')
            break;
        if (output_path[i] == '.') {
            stem = i;
            break;
        }
    }
    if (stem + 5u >= size)
        return false;
    memcpy(out, output_path, stem);
    memcpy(out + stem, ".dvm", 5u);
    return true;
}

// The chassis verifies a region's guest RAM against this before it trusts the
// bytecode for that region, so it has to be the hash of the same bytes the
// decoder saw -- the patched DOL, not the pristine one.
typedef struct {
    const LoadedCodeSection* sections;
    u32 section_count;
} GuestText;

static bool hash_guest_range(void* user, u32 start, u32 end, u64* out) {
    const GuestText* text = (const GuestText*)user;
    for (u32 i = 0; i < text->section_count; i++) {
        const LoadedCodeSection* section = &text->sections[i];
        if (!section->data || !section->size)
            continue;
        u64 section_end = (u64)section->address + section->size;
        if (start < section->address || (u64)end > section_end)
            continue;
        const u8* bytes = section->data + (start - section->address);
        u64 hash = 0xcbf29ce484222325ull;
        for (u32 offset = 0; offset < end - start; offset++) {
            hash ^= bytes[offset];
            hash *= 0x100000001b3ull;
        }
        *out = hash;
        return true;
    }
    return false;
}

static int compare_ranges(const void* left, const void* right) {
    u32 a = ((const DolVMRange*)left)->start;
    u32 b = ((const DolVMRange*)right)->start;
    return a < b ? -1 : (a > b ? 1 : 0);
}

// smc_note only coalesces with the range it appended last, so what comes out is
// in note order, not address order, and adjacent ranges can still be split. The
// container promises sorted and non-overlapping.
static DolVMRange* normalize_smc_ranges(const SMCAnalysis* smc, u32* count_out) {
    *count_out = 0;
    if (!smc->range_count)
        return NULL;
    DolVMRange* ranges =
        (DolVMRange*)malloc((size_t)smc->range_count * sizeof(*ranges));
    if (!ranges)
        return NULL;
    for (u32 i = 0; i < smc->range_count; i++) {
        ranges[i].start = smc->ranges[i].start;
        // SMCRange::end names the last instruction; the ABI wants one past it.
        ranges[i].end = smc->ranges[i].end + 4u;
    }
    qsort(ranges, smc->range_count, sizeof(*ranges), compare_ranges);
    u32 count = 1;
    for (u32 i = 1; i < smc->range_count; i++) {
        if (ranges[i].start <= ranges[count - 1u].end) {
            if (ranges[i].end > ranges[count - 1u].end)
                ranges[count - 1u].end = ranges[i].end;
            continue;
        }
        ranges[count++] = ranges[i];
    }
    *count_out = count;
    return ranges;
}

int emit_code_sections_vm(const LoadedCodeSection* sections, u32 section_count,
                          const char* output_path, u32 entry_point,
                          const char* game_id) {
    char module_path[1200];
    if (!make_module_path(output_path, module_path, sizeof(module_path))) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    const u32 limit = chunk_instructions();
    SMCAnalysis smc;
    memset(&smc, 0, sizeof(smc));
    DolIRModule ir;
    dolir_module_init(&ir);

    for (u32 s = 0; s < section_count; s++) {
        const LoadedCodeSection* section = &sections[s];
        if (!section->data || !section->size)
            continue;
        u32 total = section->size / 4u;
        PPCInst* instructions =
            (PPCInst*)malloc((size_t)total * sizeof(*instructions));
        if (!instructions) {
            fprintf(stderr, "error: out of memory decoding %s[%u]\n",
                    section->label, section->index);
            dolir_module_free(&ir);
            return 0;
        }
        u32 embedded = 0;
        u32 unknown = 0;
        for (u32 i = 0; i < total; i++) {
            u32 raw = read_be32(section->data + i * 4u);
            instructions[i] = ppc_decode(raw, section->address + i * 4u);
            if (instructions[i].op == PPC_OP_UNKNOWN &&
                embedded_data_word(section->embedded_data_mode, raw))
                instructions[i].embedded_data = true;
            embedded += instructions[i].embedded_data;
            unknown += instructions[i].op == PPC_OP_UNKNOWN &&
                       !instructions[i].embedded_data;
        }
        printf("decoding %s[%u]: %u instructions at 0x%08X\n", section->label,
               section->index, total, section->address);
        printf("  %u known, %u embedded data, %u unknown\n",
               total - embedded - unknown, embedded, unknown);

        if (section->embedded_data_mode == EMBEDDED_DATA_DOL) {
            analyze_smc_section(sections, section_count, instructions, total,
                                &smc);
            if (smc.allocation_failed) {
                fprintf(stderr, "error: out of memory analyzing SMC sites\n");
                free(instructions);
                smc_analysis_free(&smc);
                dolir_module_free(&ir);
                return 0;
            }
        }

        for (u32 start = 0; start < total; start += limit) {
            u32 count = total - start;
            if (count > limit)
                count = limit;
            if (!dolir_build_chunk(&ir, instructions + start, count,
                                   section->address + start * 4u)) {
                fprintf(stderr, "error: cannot build IR at 0x%08X\n",
                        section->address + start * 4u);
                free(instructions);
                smc_analysis_free(&smc);
                dolir_module_free(&ir);
                return 0;
            }
        }
        free(instructions);
    }

    if (!ir.function_count) {
        fprintf(stderr, "error: no executable sections to lower\n");
        smc_analysis_free(&smc);
        dolir_module_free(&ir);
        return 0;
    }
    if (!dolir_verify(&ir, stderr)) {
        smc_analysis_free(&smc);
        dolir_module_free(&ir);
        return 0;
    }

    u32 smc_count = 0;
    DolVMRange* smc_ranges = normalize_smc_ranges(&smc, &smc_count);
    if (smc.range_count && !smc_ranges) {
        fprintf(stderr, "error: out of memory normalizing SMC sites\n");
        smc_analysis_free(&smc);
        dolir_module_free(&ir);
        return 0;
    }
    smc_analysis_free(&smc);

    GuestText text;
    text.sections = sections;
    text.section_count = section_count;

    DolVMEmitOptions options;
    memset(&options, 0, sizeof(options));
    options.direct_calls = direct_calls_enabled();
    options.home_state = home_state_enabled();
    options.entry_point = entry_point;
    options.game_id = game_id;
    options.smc_ranges = smc_ranges;
    options.smc_count = smc_count;
    options.hash_guest_range = hash_guest_range;
    options.hash_user = &text;

    DolVMOptStats stats;
    memset(&stats, 0, sizeof(stats));
    void* image = NULL;
    size_t size = 0;
    bool ok = dolvm_build_module(&ir, &options, &image, &size, &stats, stderr);
    dolir_module_free(&ir);
    free(smc_ranges);
    if (!ok) {
        fprintf(stderr, "error: bytecode lowering failed\n");
        return 0;
    }

    dolvm_stats_report(&stats, "dolvm", stdout);
    printf("dolvm: %zu bytes of module%s\n", size,
           options.direct_calls ? ", intra-module calls resolved inline" : "");
    if (smc_count)
        printf("dolvm: %u self-modifying-code candidate ranges\n", smc_count);
    ok = dolvm_write_module(image, size, module_path, stderr);
    free(image);
    if (!ok)
        return 0;
    printf("wrote %s\n", module_path);
    return 1;
}
