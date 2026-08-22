// SPDX-License-Identifier: GPL-3.0-or-later
//
// DolVM container: opcode metadata, loading and validation.
//
// Everything the interpreter assumes about a module is checked here, once, at
// load: header identity, section bounds, opcode legality, branch and entry
// targets, and every CPUState byte offset a state access bakes in. The hot loop
// then runs with no bounds checks of its own, which is only defensible because
// nothing reaches it that did not pass through this file.

#include "vm/dolvm.h"
#include "vm/dolvm_state.h"
#include "cpu/cpu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const u8 g_word_counts[DOLVM_OP_COUNT] = {
    [DOLVM_OP_FALLBACK] = 2,
    [DOLVM_OP_CALL] = 2,
    [DOLVM_OP_EXACT_FLOAT] = 2,
    [DOLVM_OP_EXACT_PAIRED] = 2,
    [DOLVM_OP_PSQ_LOAD] = 2,
    [DOLVM_OP_PSQ_STORE] = 2,
    [DOLVM_OP_PROGRAM_EXC] = 2,
    [DOLVM_OP_SPR_READ] = 2,
    [DOLVM_OP_SPR_WRITE] = 2,
    [DOLVM_OP_LSWX] = 2,
    [DOLVM_OP_CACHE_CONTROL] = 2,
    [DOLVM_OP_JMP_GUARD] = 2,
    [DOLVM_OP_CMP_STATE_I] = 2,
    [DOLVM_OP_CMP_STATE] = 2,
    [DOLVM_OP_LOAD_MEM_STATE] = 2,
    [DOLVM_OP_STORE_MEM_STATE] = 2,
    [DOLVM_OP_LOAD_MEM_TO_STATE] = 2,
    [DOLVM_OP_STORE_MEM_FROM_STATE] = 2,
    [DOLVM_OP_SET_CR_FIELDI] = 2,
    [DOLVM_OP_JMP_CHARGE] = 2,
    [DOLVM_OP_JMP_IF_CR_CHARGE] = 2,
    [DOLVM_OP_JMP_IF_CR_GUARD] = 2,
    [DOLVM_OP_CMP_JMP_IF_CR] = 2,
    [DOLVM_OP_CMP_JMP_IF_CR_CHARGE] = 2,
    [DOLVM_OP_CMP_JMP_IF_CR_GUARD] = 3,
};

u32 dolvm_op_words(u32 op) {
    if (op >= DOLVM_OP_COUNT)
        return 0;
    return g_word_counts[op] ? g_word_counts[op] : 1u;
}

static const char* const g_op_names[DOLVM_OP_COUNT] = {
    [DOLVM_OP_NOP] = "nop",
    [DOLVM_OP_MOV] = "mov",
    [DOLVM_OP_CONST32] = "const32",
    [DOLVM_OP_CONST64] = "const64",
    [DOLVM_OP_LOAD_STATE8] = "load.state8",
    [DOLVM_OP_LOAD_STATE32] = "load.state32",
    [DOLVM_OP_LOAD_STATE64] = "load.state64",
    [DOLVM_OP_LOAD_STATEF] = "load.statef",
    [DOLVM_OP_STORE_STATE8] = "store.state8",
    [DOLVM_OP_STORE_STATE32] = "store.state32",
    [DOLVM_OP_STORE_STATE64] = "store.state64",
    [DOLVM_OP_STORE_STATEF] = "store.statef",
    [DOLVM_OP_ADD32] = "add32",
    [DOLVM_OP_ADD32I] = "add32i",
    [DOLVM_OP_SUB32] = "sub32",
    [DOLVM_OP_MUL32] = "mul32",
    [DOLVM_OP_MUL32I] = "mul32i",
    [DOLVM_OP_UDIV32] = "udiv32",
    [DOLVM_OP_SDIV32] = "sdiv32",
    [DOLVM_OP_ADD64] = "add64",
    [DOLVM_OP_SUB64] = "sub64",
    [DOLVM_OP_MUL64] = "mul64",
    [DOLVM_OP_UDIV64] = "udiv64",
    [DOLVM_OP_SDIV64] = "sdiv64",
    [DOLVM_OP_AND] = "and",
    [DOLVM_OP_ANDI] = "andi",
    [DOLVM_OP_OR] = "or",
    [DOLVM_OP_ORI] = "ori",
    [DOLVM_OP_XOR] = "xor",
    [DOLVM_OP_XORI] = "xori",
    [DOLVM_OP_NOT] = "not",
    [DOLVM_OP_SHL32] = "shl32",
    [DOLVM_OP_SHL32I] = "shl32i",
    [DOLVM_OP_LSHR32] = "lshr32",
    [DOLVM_OP_LSHR32I] = "lshr32i",
    [DOLVM_OP_ASHR32] = "ashr32",
    [DOLVM_OP_ASHR32I] = "ashr32i",
    [DOLVM_OP_SHL64] = "shl64",
    [DOLVM_OP_SHL64I] = "shl64i",
    [DOLVM_OP_LSHR64] = "lshr64",
    [DOLVM_OP_LSHR64I] = "lshr64i",
    [DOLVM_OP_ASHR64] = "ashr64",
    [DOLVM_OP_ASHR64I] = "ashr64i",
    [DOLVM_OP_ROTL32] = "rotl32",
    [DOLVM_OP_ROTL32I] = "rotl32i",
    [DOLVM_OP_CLZ32] = "clz32",
    [DOLVM_OP_CLZ64] = "clz64",
    [DOLVM_OP_BSWAP16] = "bswap16",
    [DOLVM_OP_BSWAP32] = "bswap32",
    [DOLVM_OP_BSWAP64] = "bswap64",
    [DOLVM_OP_TRUNC] = "trunc",
    [DOLVM_OP_SEXT] = "sext",
    [DOLVM_OP_ICMP_EQ] = "icmp.eq",
    [DOLVM_OP_ICMP_EQI] = "icmp.eqi",
    [DOLVM_OP_ICMP_NE] = "icmp.ne",
    [DOLVM_OP_ICMP_NEI] = "icmp.nei",
    [DOLVM_OP_ICMP_ULT] = "icmp.ult",
    [DOLVM_OP_ICMP_ULTI] = "icmp.ulti",
    [DOLVM_OP_ICMP_ULE] = "icmp.ule",
    [DOLVM_OP_ICMP_ULEI] = "icmp.ulei",
    [DOLVM_OP_ICMP_SLT32] = "icmp.slt32",
    [DOLVM_OP_ICMP_SLT32I] = "icmp.slt32i",
    [DOLVM_OP_ICMP_SLE32] = "icmp.sle32",
    [DOLVM_OP_ICMP_SLE32I] = "icmp.sle32i",
    [DOLVM_OP_ICMP_SLT64] = "icmp.slt64",
    [DOLVM_OP_ICMP_SLE64] = "icmp.sle64",
    [DOLVM_OP_FCMP_OEQ] = "fcmp.oeq",
    [DOLVM_OP_FCMP_OLT] = "fcmp.olt",
    [DOLVM_OP_FCMP_OGE] = "fcmp.oge",
    [DOLVM_OP_SELECT] = "select",
    [DOLVM_OP_FADD] = "fadd",
    [DOLVM_OP_FSUB] = "fsub",
    [DOLVM_OP_FMUL] = "fmul",
    [DOLVM_OP_FDIV] = "fdiv",
    [DOLVM_OP_FNEG] = "fneg",
    [DOLVM_OP_FABS] = "fabs",
    [DOLVM_OP_FPTRUNC] = "fptrunc",
    [DOLVM_OP_FPEXT] = "fpext",
    [DOLVM_OP_LOAD8] = "load8",
    [DOLVM_OP_LOAD16] = "load16",
    [DOLVM_OP_LOAD32] = "load32",
    [DOLVM_OP_LOAD64] = "load64",
    [DOLVM_OP_LOAD8S] = "load8s",
    [DOLVM_OP_LOAD16S] = "load16s",
    [DOLVM_OP_LOAD32S] = "load32s",
    [DOLVM_OP_STORE8] = "store8",
    [DOLVM_OP_STORE16] = "store16",
    [DOLVM_OP_STORE32] = "store32",
    [DOLVM_OP_STORE64] = "store64",
    [DOLVM_OP_CHARGE] = "charge",
    [DOLVM_OP_PC_BASE] = "pc.base",
    [DOLVM_OP_LOOP_GUARD] = "loop.guard",
    [DOLVM_OP_JMP] = "jmp",
    [DOLVM_OP_JMP_IF] = "jmp.if",
    [DOLVM_OP_JMP_IFNOT] = "jmp.ifnot",
    [DOLVM_OP_EXIT] = "exit",
    [DOLVM_OP_EXIT_REG] = "exit.reg",
    [DOLVM_OP_INDIRECT] = "indirect",
    [DOLVM_OP_CALL] = "call",
    [DOLVM_OP_FALLBACK] = "fallback",
    [DOLVM_OP_SYSCALL] = "syscall",
    [DOLVM_OP_RFI] = "rfi",
    [DOLVM_OP_FP_AVAILABLE] = "fp.available",
    [DOLVM_OP_EXACT_FLOAT] = "exact.float",
    [DOLVM_OP_EXACT_PAIRED] = "exact.paired",
    [DOLVM_OP_PSQ_LOAD] = "psq.load",
    [DOLVM_OP_PSQ_STORE] = "psq.store",
    [DOLVM_OP_STWCX] = "stwcx",
    [DOLVM_OP_FPSCR_UPDATED] = "fpscr.updated",
    [DOLVM_OP_FPSCR_BIT] = "fpscr.bit",
    [DOLVM_OP_PROGRAM_EXC] = "program.exception",
    [DOLVM_OP_SPR_READ] = "spr.read",
    [DOLVM_OP_SPR_WRITE] = "spr.write",
    [DOLVM_OP_LSWX] = "lswx",
    [DOLVM_OP_DCBZ_L] = "dcbz.l",
    [DOLVM_OP_ECIWX] = "eciwx",
    [DOLVM_OP_ECOWX] = "ecowx",
    [DOLVM_OP_TLBIE] = "tlbie",
    [DOLVM_OP_CACHE_CONTROL] = "cache.control",
    [DOLVM_OP_FENCE] = "fence",
    [DOLVM_OP_SET_CR_FIELD] = "set.cr.field",
    [DOLVM_OP_JMP_GUARD] = "jmp.guard",
    [DOLVM_OP_JMP_IF_CR] = "jmp.if.cr",
    [DOLVM_OP_CMP_STATE_I] = "cmp.state.i",
    [DOLVM_OP_CMP_STATE] = "cmp.state",
    [DOLVM_OP_LOAD_MEM_STATE] = "load.mem.state",
    [DOLVM_OP_STORE_MEM_STATE] = "store.mem.state",
    [DOLVM_OP_LOAD_MEM_TO_STATE] = "load.mem.to.state",
    [DOLVM_OP_STORE_MEM_FROM_STATE] = "store.mem.from.state",
    [DOLVM_OP_SET_CR_FIELDI] = "set.cr.fieldi",
    [DOLVM_OP_JMP_CHARGE] = "jmp.charge",
    [DOLVM_OP_JMP_IF_CR_CHARGE] = "jmp.if.cr.charge",
    [DOLVM_OP_JMP_IF_CR_GUARD] = "jmp.if.cr.guard",
    [DOLVM_OP_SUPERVISOR] = "supervisor",
    [DOLVM_OP_CMP_JMP_IF_CR] = "cmp.jmp.if.cr",
    [DOLVM_OP_CMP_JMP_IF_CR_CHARGE] = "cmp.jmp.if.cr.charge",
    [DOLVM_OP_CMP_JMP_IF_CR_GUARD] = "cmp.jmp.if.cr.guard",
};

const char* dolvm_op_name(u32 op) {
    if (op >= DOLVM_OP_COUNT || !g_op_names[op])
        return "invalid";
    return g_op_names[op];
}

static bool fail(char* error, size_t size, const char* format, ...) {
    if (error && size) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
    }
    return false;
}

static bool state_access(u32 op) {
    switch (op) {
    case DOLVM_OP_LOAD_STATE8:
    case DOLVM_OP_LOAD_STATE32:
    case DOLVM_OP_LOAD_STATE64:
    case DOLVM_OP_LOAD_STATEF:
    case DOLVM_OP_STORE_STATE8:
    case DOLVM_OP_STORE_STATE32:
    case DOLVM_OP_STORE_STATE64:
    case DOLVM_OP_STORE_STATEF:
        return true;
    default:
        return false;
    }
}

static u32 state_access_width(u32 op) {
    switch (op) {
    case DOLVM_OP_LOAD_STATE8:
    case DOLVM_OP_STORE_STATE8:
        return 1u;
    case DOLVM_OP_LOAD_STATE32:
    case DOLVM_OP_STORE_STATE32:
        return 4u;
    default:
        return 8u;
    }
}

static bool branch_op(u32 op) {
    switch (op) {
    case DOLVM_OP_JMP:
    case DOLVM_OP_JMP_IF:
    case DOLVM_OP_JMP_IFNOT:
    case DOLVM_OP_JMP_GUARD:
    case DOLVM_OP_JMP_IF_CR:
    case DOLVM_OP_JMP_CHARGE:
    case DOLVM_OP_JMP_IF_CR_CHARGE:
    case DOLVM_OP_JMP_IF_CR_GUARD:
    case DOLVM_OP_CMP_JMP_IF_CR:
    case DOLVM_OP_CMP_JMP_IF_CR_CHARGE:
    case DOLVM_OP_CMP_JMP_IF_CR_GUARD:
    case DOLVM_OP_CALL:
        return true;
    default:
        return false;
    }
}

static bool verify_code(const DolVMModule* module, char* error, size_t size) {
    u32 index = 0;
    while (index < module->code_count) {
        const DolVMInst* inst = &module->code[index];
        u32 words = dolvm_op_words(inst->op);
        if (!words)
            return fail(error, size, "dolvm: bad opcode %u at %u", inst->op,
                        index);
        if (index + words > module->code_count)
            return fail(error, size, "dolvm: truncated %s at %u",
                        dolvm_op_name(inst->op), index);
        if (branch_op(inst->op) && inst->imm >= module->code_count)
            return fail(error, size, "dolvm: branch out of range at %u", index);
        if (inst->op == DOLVM_OP_CONST64 &&
            inst->imm >= module->constant_count)
            return fail(error, size, "dolvm: constant out of range at %u",
                        index);
        if (state_access(inst->op)) {
            u32 width = state_access_width(inst->op);
            if (inst->imm > sizeof(CPUState) - width)
                return fail(error, size,
                            "dolvm: state offset %u out of range at %u",
                            inst->imm, index);
        }
        if (inst->op == DOLVM_OP_INDIRECT && inst->imm != DOLVM_NO_ENTRY &&
            inst->imm >= module->region_count)
            return fail(error, size, "dolvm: indirect names region %u of %u",
                        inst->imm, module->region_count);
        if (inst->op == DOLVM_OP_CALL &&
            ((u32)inst->a | (u32)inst->b << 8) >= module->region_count)
            return fail(error, size, "dolvm: call names region %u of %u",
                        (u32)inst->a | (u32)inst->b << 8, module->region_count);
        if ((inst->op == DOLVM_OP_JMP_IF_CR ||
             inst->op == DOLVM_OP_JMP_IF_CR_CHARGE ||
             inst->op == DOLVM_OP_JMP_IF_CR_GUARD) && inst->a >= 32u)
            return fail(error, size, "dolvm: cr bit %u at %u", inst->a, index);
        // The fused compares carry their CPUState offsets in the payload word
        // rather than in imm, so the generic state-offset check above misses
        // them. They read four bytes from each.
        if (inst->op == DOLVM_OP_LOAD_MEM_TO_STATE ||
            inst->op == DOLVM_OP_STORE_MEM_FROM_STATE) {
            u64 payload;
            memcpy(&payload, &module->code[index + 1u], sizeof(payload));
            if ((u32)payload > sizeof(CPUState) - 4u)
                return fail(error, size,
                            "dolvm: fused memory slot out of range at %u", index);
        }
        if (inst->op == DOLVM_OP_LOAD_MEM_STATE ||
            inst->op == DOLVM_OP_STORE_MEM_STATE) {
            u64 payload;
            memcpy(&payload, &module->code[index + 1u], sizeof(payload));
            if ((u32)payload > sizeof(CPUState) - 4u ||
                (u32)(payload >> 32) > sizeof(CPUState) - 4u)
                return fail(error, size,
                            "dolvm: fused memory slot out of range at %u", index);
            bool load = inst->op == DOLVM_OP_LOAD_MEM_STATE;
            bool known = load ? (inst->b == DOLVM_OP_LOAD8 ||
                                 inst->b == DOLVM_OP_LOAD16 ||
                                 inst->b == DOLVM_OP_LOAD32 ||
                                 inst->b == DOLVM_OP_LOAD8S ||
                                 inst->b == DOLVM_OP_LOAD16S)
                              : (inst->b == DOLVM_OP_STORE8 ||
                                 inst->b == DOLVM_OP_STORE16 ||
                                 inst->b == DOLVM_OP_STORE32);
            if (!known)
                return fail(error, size,
                            "dolvm: fused memory op stands in for %u at %u",
                            inst->b, index);
        }
        if (inst->op == DOLVM_OP_CMP_STATE_I ||
            inst->op == DOLVM_OP_CMP_STATE) {
            u64 payload;
            memcpy(&payload, &module->code[index + 1u], sizeof(payload));
            u32 left = (u32)(payload >> 32);
            if (left > sizeof(CPUState) - 4u)
                return fail(error, size,
                            "dolvm: compare state offset %u out of range at %u",
                            left, index);
            if (inst->op == DOLVM_OP_CMP_STATE) {
                u32 right = (u32)payload;
                if (right > sizeof(CPUState) - 4u)
                    return fail(error, size,
                                "dolvm: compare state offset %u out of range at %u",
                                right, index);
            }
        }
        index += words;
    }
    return true;
}

static bool verify_regions(const DolVMModule* module, char* error, size_t size) {
    // CALL carries its target's region in two bytes.
    if (module->region_count > 0xFFFFu)
        return fail(error, size, "dolvm: %u regions, at most 65535",
                    module->region_count);
    u32 previous_end = 0;
    for (u32 i = 0; i < module->region_count; i++) {
        const DolVMRegion* region = &module->regions[i];
        if (region->guest_start >= region->guest_end ||
            (region->guest_start & 3u) || (region->guest_end & 3u))
            return fail(error, size, "dolvm: bad region %u", i);
        if (i && region->guest_start < previous_end)
            return fail(error, size, "dolvm: regions overlap at %u", i);
        previous_end = region->guest_end;
        u32 count = (region->guest_end - region->guest_start) / 4u;
        if (region->map_index > module->map_count ||
            count > module->map_count - region->map_index)
            return fail(error, size, "dolvm: region %u entry map overflows", i);
        for (u32 n = 0; n < count; n++) {
            u32 entry = module->map[region->map_index + n].entry;
            if (entry == DOLVM_NO_ENTRY)
                continue;
            if ((entry & DOLVM_ENTRY_OFFSET_MASK) >= module->code_count)
                return fail(error, size,
                            "dolvm: entry for 0x%08X out of range",
                            region->guest_start + n * 4u);
        }
    }
    return true;
}

// The chassis takes these as sorted, non-overlapping, end-exclusive ranges and
// does not re-check them, so this is where that has to be true.
static bool verify_smc_ranges(const DolVMModule* module, char* error,
                              size_t size) {
    u32 previous_end = 0;
    for (u32 i = 0; i < module->smc_count; i++) {
        const DolVMRange* range = &module->smc_ranges[i];
        if (range->start >= range->end || (range->start & 3u) ||
            (range->end & 3u))
            return fail(error, size, "dolvm: bad smc range %u", i);
        if (i && range->start < previous_end)
            return fail(error, size, "dolvm: smc ranges overlap at %u", i);
        previous_end = range->end;
    }
    return true;
}

// The O(1) region table. Every dispatch and every module-wide indirect branch
// starts with "which region is this address in", and a binary search over a
// few dozen regions is a handful of data-dependent branches each time; a
// bucket table answers it with one load and a compare or two. Running out of
// memory here is not an error -- the search stays available.
static void build_lookup(DolVMModule* module) {
    module->lookup = NULL;
    if (!module->region_count)
        return;
    u32 base = module->regions[0].guest_start & ~((1u << DOLVM_LOOKUP_SHIFT) - 1u);
    u32 end = module->regions[module->region_count - 1u].guest_end;
    u64 span = (u64)end - base;
    u64 buckets = (span + (1u << DOLVM_LOOKUP_SHIFT) - 1u) >> DOLVM_LOOKUP_SHIFT;
    if (buckets > DOLVM_LOOKUP_MAX_BUCKETS || span > 0xFFFFFFFFull)
        return;
    u32* lookup = (u32*)malloc((size_t)buckets * sizeof(u32));
    if (!lookup)
        return;
    u32 region = 0;
    for (u32 b = 0; b < (u32)buckets; b++) {
        u32 bucket_start = base + (b << DOLVM_LOOKUP_SHIFT);
        // Regions are sorted, so the first one ending past this bucket's start
        // is at or after the one found for the previous bucket.
        while (region < module->region_count &&
               module->regions[region].guest_end <= bucket_start)
            region++;
        lookup[b] = region;
    }
    module->lookup = lookup;
    module->lookup_base = base;
    module->lookup_span = (u32)span;
}

bool dolvm_module_open(DolVMModule* module, const void* image, size_t size,
                       char* error, size_t error_size) {
    memset(module, 0, sizeof(*module));
    if (!image || size < sizeof(DolVMHeader))
        return fail(error, error_size, "dolvm: image is too small");

    const DolVMHeader* header = (const DolVMHeader*)image;
    if (memcmp(header->magic, DOLVM_MAGIC, DOLVM_MAGIC_SIZE) != 0)
        return fail(error, error_size, "dolvm: not a DolVM module");
    if (header->version != DOLVM_VERSION)
        return fail(error, error_size, "dolvm: module version %u, expected %u",
                    header->version, DOLVM_VERSION);
    if (header->abi_version != DOLVM_ABI_VERSION)
        return fail(error, error_size, "dolvm: module ABI %u, expected %u",
                    header->abi_version, DOLVM_ABI_VERSION);
    // Not a size check: the recompiler's own CPU runtime and a chassis agree on
    // every field a state slot names and still differ past the last of them.
    // What has to match is the slot-to-offset mapping itself.
    if (header->state_layout_hash != dolvm_state_layout_hash())
        return fail(error, error_size,
                    "dolvm: CPUState layout %08X here, %08X in the module",
                    dolvm_state_layout_hash(), header->state_layout_hash);

    if (!memchr(header->game_id, '\0', sizeof(header->game_id)))
        return fail(error, error_size, "dolvm: game id is not terminated");

    // Sections are laid out in this order, each already 8-byte aligned by the
    // writer because every element type is a multiple of 8 or 4 bytes.
    size_t offset = sizeof(DolVMHeader);
    size_t code_bytes = (size_t)header->code_count * sizeof(DolVMInst);
    size_t constant_bytes = (size_t)header->constant_count * sizeof(u64);
    size_t region_bytes = (size_t)header->region_count * sizeof(DolVMRegion);
    size_t map_bytes = (size_t)header->map_count * sizeof(DolVMEntryPoint);
    size_t hash_bytes = (size_t)header->region_count * sizeof(u64);
    size_t smc_bytes = (size_t)header->smc_count * sizeof(DolVMRange);
    if (code_bytes / sizeof(DolVMInst) != header->code_count ||
        size - offset < code_bytes)
        return fail(error, error_size, "dolvm: code section overflows image");
    module->code = (const DolVMInst*)((const u8*)image + offset);
    offset += code_bytes;
    if (size - offset < constant_bytes)
        return fail(error, error_size, "dolvm: constants overflow image");
    module->constants = (const u64*)((const u8*)image + offset);
    offset += constant_bytes;
    if (size - offset < region_bytes)
        return fail(error, error_size, "dolvm: regions overflow image");
    module->regions = (const DolVMRegion*)((const u8*)image + offset);
    offset += region_bytes;
    if (size - offset < map_bytes)
        return fail(error, error_size, "dolvm: entry map overflows image");
    module->map = (const DolVMEntryPoint*)((const u8*)image + offset);
    offset += map_bytes;
    if (size - offset < hash_bytes)
        return fail(error, error_size, "dolvm: region hashes overflow image");
    module->region_hashes = (const u64*)((const u8*)image + offset);
    offset += hash_bytes;
    if (size - offset < smc_bytes)
        return fail(error, error_size, "dolvm: smc ranges overflow image");
    module->smc_ranges = (const DolVMRange*)((const u8*)image + offset);

    module->header = header;
    module->code_count = header->code_count;
    module->constant_count = header->constant_count;
    module->region_count = header->region_count;
    module->map_count = header->map_count;
    module->smc_count = header->smc_count;
    module->flags = header->flags;
    if (!verify_code(module, error, error_size) ||
        !verify_regions(module, error, error_size) ||
        !verify_smc_ranges(module, error, error_size)) {
        memset(module, 0, sizeof(*module));
        return false;
    }
    build_lookup(module);
    return true;
}

bool dolvm_module_load_file(DolVMModule* module, const char* path, char* error,
                            size_t error_size) {
    memset(module, 0, sizeof(*module));
    FILE* file = fopen(path, "rb");
    if (!file)
        return fail(error, error_size, "dolvm: cannot open %s", path);
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return fail(error, error_size, "dolvm: cannot size %s", path);
    }
    long length = ftell(file);
    if (length <= 0) {
        fclose(file);
        return fail(error, error_size, "dolvm: %s is empty", path);
    }
    rewind(file);
    // The interpreter reads instructions as 8-byte structs straight out of this
    // buffer, so it has to be at least that aligned; malloc guarantees it.
    void* image = malloc((size_t)length);
    if (!image) {
        fclose(file);
        return fail(error, error_size, "dolvm: out of memory reading %s", path);
    }
    size_t read = fread(image, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(image);
        return fail(error, error_size, "dolvm: short read on %s", path);
    }
    if (!dolvm_module_open(module, image, (size_t)length, error, error_size)) {
        free(image);
        return false;
    }
    module->image = image;
    module->image_size = (size_t)length;
    return true;
}

void dolvm_module_close(DolVMModule* module) {
    if (!module)
        return;
    free(module->lookup);
    free(module->image);
    memset(module, 0, sizeof(*module));
}

void dolvm_module_set_gate(DolVMModule* module, const DolVMGate* gate) {
    if (module)
        module->gate = gate;
}

const DolVMRegion* dolvm_module_region(const DolVMModule* module, u32 address) {
    return dolvm_module_region_inline(module, address);
}

const DolVMEntryPoint* dolvm_module_entry(const DolVMModule* module,
                                          u32 address) {
    return dolvm_module_entry_inline(module, address);
}
