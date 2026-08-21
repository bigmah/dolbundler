// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dump a .dvm module: header, regions, and the bytecode stream with the guest
// address each entry point lands on. Exists so a wrong answer from the
// interpreter can be read back against the bytecode that produced it.

#include "vm/dolvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A real title has millions of instructions and nearly as many entry points, so
// the entry annotations are indexed by the offset they land on rather than
// rescanned per instruction. Chained buckets: head[offset] starts a list linked
// through next[].
typedef struct {
    u32* head;
    u32* next;
} EntryIndex;

static bool entry_index_build(EntryIndex* index, const DolVMModule* module) {
    index->head = (u32*)malloc((size_t)module->code_count * sizeof(u32));
    index->next = (u32*)malloc((size_t)module->map_count * sizeof(u32));
    if (!index->head || !index->next) {
        free(index->head);
        free(index->next);
        return false;
    }
    for (u32 i = 0; i < module->code_count; i++)
        index->head[i] = DOLVM_NO_ENTRY;
    for (u32 i = module->map_count; i-- > 0;) {
        u32 entry = module->map[i].entry;
        if (entry == DOLVM_NO_ENTRY)
            continue;
        u32 offset = entry & DOLVM_ENTRY_OFFSET_MASK;
        if (offset >= module->code_count)
            continue;
        index->next[i] = index->head[offset];
        index->head[offset] = i;
    }
    return true;
}

static void entry_index_free(EntryIndex* index) {
    free(index->head);
    free(index->next);
}

// Which guest address does entry slot `i` describe?
static u32 entry_address(const DolVMModule* module, u32 slot) {
    for (u32 r = 0; r < module->region_count; r++) {
        const DolVMRegion* region = &module->regions[r];
        u32 count = (region->guest_end - region->guest_start) / 4u;
        if (slot >= region->map_index && slot < region->map_index + count)
            return region->guest_start + (slot - region->map_index) * 4u;
    }
    return 0;
}

static void print_entries(const DolVMModule* module, const EntryIndex* index,
                          u32 offset) {
    for (u32 slot = index->head[offset]; slot != DOLVM_NO_ENTRY;
         slot = index->next[slot]) {
        printf("  ; entry 0x%08X%s\n", entry_address(module, slot),
               (module->map[slot].entry & DOLVM_ENTRY_RETURN_TARGET)
                   ? " (return target)"
                   : "");
    }
}

static u64 payload_of(const DolVMInst* inst) {
    u64 value;
    memcpy(&value, inst, sizeof(value));
    return value;
}

static void usage(void) {
    fprintf(stderr,
            "usage: dolvm_dis <module.dvm> [--code] [--at <guest-address>]\n"
            "                 [--count <instructions>]\n"
            "\n"
            "  --code           disassemble the whole stream\n"
            "  --at 0x80005940  start at the bytecode for a guest address\n"
            "  --count 200      stop after this many instructions\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    bool with_code = false;
    bool have_address = false;
    u32 address = 0;
    u32 limit = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--code")) {
            with_code = true;
        } else if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            address = (u32)strtoul(argv[++i], NULL, 0);
            have_address = true;
        } else if (!strcmp(argv[i], "--count") && i + 1 < argc) {
            limit = (u32)strtoul(argv[++i], NULL, 0);
        } else {
            usage();
            return 2;
        }
    }

    DolVMModule module;
    char error[256] = "";
    if (!dolvm_module_load_file(&module, argv[1], error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    printf("dolvm module %s\n", argv[1]);
    printf("  version %u, abi %u, CPUState %u bytes\n", module.header->version,
           module.header->abi_version, module.header->cpu_state_size);
    printf("  entry point 0x%08X, flags 0x%X%s\n", module.header->entry_point,
           module.flags,
           (module.flags & DOLVM_FLAG_DIRECT_CALLS) ? " (direct calls)" : "");
    printf("  game id %s, state layout %08X, %u smc ranges\n",
           module.header->game_id[0] ? module.header->game_id : "(none)",
           module.header->state_layout_hash, module.smc_count);
    printf("  %u instructions, %u constants, %u regions, %u entry points\n",
           module.code_count, module.constant_count, module.region_count,
           module.map_count);
    printf("  %zu bytes on disk, %.2f bytes per guest instruction\n",
           module.image_size,
           module.map_count ? (double)module.image_size / (double)module.map_count
                            : 0.0);

    u32 covered = 0;
    u32 return_targets = 0;
    for (u32 i = 0; i < module.map_count; i++) {
        covered += module.map[i].entry != DOLVM_NO_ENTRY;
        return_targets += module.map[i].entry != DOLVM_NO_ENTRY &&
                          (module.map[i].entry & DOLVM_ENTRY_RETURN_TARGET) != 0;
    }
    printf("  %u addresses covered, %u of them indirect-branch targets\n",
           covered, return_targets);
    for (u32 r = 0; r < module.region_count; r++) {
        printf("  region %u: 0x%08X..0x%08X\n", r, module.regions[r].guest_start,
               module.regions[r].guest_end);
    }

    if (!with_code && !have_address) {
        dolvm_module_close(&module);
        return 0;
    }

    u32 start = 0;
    if (have_address) {
        const DolVMEntryPoint* entry = dolvm_module_entry(&module, address);
        if (!entry) {
            fprintf(stderr, "dolvm_dis: 0x%08X is not covered by this module\n",
                    address);
            dolvm_module_close(&module);
            return 1;
        }
        start = entry->entry & DOLVM_ENTRY_OFFSET_MASK;
        if (!limit)
            limit = 200u;
        printf("\n; 0x%08X enters at %u, pc base 0x%08X\n", address, start,
               entry->pc_base);
    }

    EntryIndex index;
    if (!entry_index_build(&index, &module)) {
        fprintf(stderr, "dolvm_dis: out of memory indexing entry points\n");
        dolvm_module_close(&module);
        return 1;
    }

    u32 shown = 0;
    for (u32 i = start; i < module.code_count;) {
        if (limit && shown == limit) {
            printf("... %u more instructions (raise --count to see them)\n",
                   module.code_count - i);
            break;
        }
        const DolVMInst* inst = &module.code[i];
        u32 words = dolvm_op_words(inst->op);
        print_entries(&module, &index, i);
        printf("%8u  %-18s a=%-3u b=%-3u c=%-3u imm=0x%08X", i,
               dolvm_op_name(inst->op), inst->a, inst->b, inst->c, inst->imm);
        if (words == 2)
            printf("  payload=0x%016llX",
                   (unsigned long long)payload_of(inst + 1));
        printf("\n");
        i += words;
        shown++;
    }

    entry_index_free(&index);
    dolvm_module_close(&module);
    return 0;
}
