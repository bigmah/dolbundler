// SPDX-License-Identifier: GPL-3.0-or-later
//
// See dolvm_opt.h for what these passes are for and why mid-block entry
// constrains them.

#include "backend/vm/dolvm_opt.h"
#include "vm/dolvm.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

static u64 type_mask(DolIRType type) {
    switch (type) {
    case DOLIR_TYPE_I1: return 1ull;
    case DOLIR_TYPE_I8: return 0xFFull;
    case DOLIR_TYPE_I16: return 0xFFFFull;
    case DOLIR_TYPE_I32: return 0xFFFFFFFFull;
    default: return 0xFFFFFFFFFFFFFFFFull;
    }
}

static bool integer_type(DolIRType type) {
    return type >= DOLIR_TYPE_I1 && type <= DOLIR_TYPE_I64;
}

static bool pure_op(DolIROp op) {
    switch (op) {
    case DOLIR_OP_PHI:
    case DOLIR_OP_STATE_READ:
    case DOLIR_OP_STATE_WRITE:
    case DOLIR_OP_GUEST_LOAD:
    case DOLIR_OP_GUEST_STORE:
    case DOLIR_OP_HELPER_CALL:
        return false;
    default:
        return true;
    }
}

// An instruction that can reach code outside this block -- a helper, or the
// slow path of a guest access -- may change any state slot and may not return
// at all. Everything cached about guest state stops being true across one.
static bool cr_field_helper(const DolIRInstruction* inst) {
    return inst->op == DOLIR_OP_HELPER_CALL &&
           inst->aux == DOLIR_HELPER_CR_FIELD;
}

static bool state_barrier(const DolIRInstruction* inst) {
    if (cr_field_helper(inst))
        return false;
    return inst->op == DOLIR_OP_HELPER_CALL ||
           inst->op == DOLIR_OP_GUEST_LOAD || inst->op == DOLIR_OP_GUEST_STORE;
}

// ---------------------------------------------------------------------------
// Per-function working state
// ---------------------------------------------------------------------------

typedef struct {
    DolIRValue key_operands[4];
    u32 op;
    u32 type;
    u32 aux;
    u64 immediate;
    DolIRValue result;
    bool constant;
    bool used;
} CSEEntry;

typedef struct {
    DolIRFunction* function;
    DolVMLayout* layout;
    DolVMOptStats* stats;

    // Per-value tables, valid only for the value range owned by the block
    // currently being processed.
    DolIRValue* alias;
    u8* is_constant;
    u64* constant_value;
    u64* known_mask;
    // The one state slot a value may be recovered from at a boundary, and
    // whether it has one. See hold_in_slot.
    u8* value_slot;
    u8* value_has_slot;
    u32* def_index;
    u32* last_use;

    CSEEntry* table;
    u32 table_mask;

    DolIRValue slot_value[DOLIR_STATE_COUNT];
    u32 slot_write_index[DOLIR_STATE_COUNT];
    u32 slot_write_pc[DOLIR_STATE_COUNT];
    u32 slot_value_pc[DOLIR_STATE_COUNT];
    u8 slot_read_since[DOLIR_STATE_COUNT];

    u8* dead;
    u32 dead_capacity;

    DolIRValue* live_constants;
    u32 live_constant_count;
    u32 live_constant_capacity;
} Optimizer;

static void reset_value_range(Optimizer* opt, u32 first, u32 last) {
    if (last < first)
        return;
    u32 count = last - first + 1u;
    memset(opt->alias + first, 0, count * sizeof(*opt->alias));
    memset(opt->is_constant + first, 0, count * sizeof(*opt->is_constant));
    memset(opt->constant_value + first, 0, count * sizeof(*opt->constant_value));
    memset(opt->known_mask + first, 0, count * sizeof(*opt->known_mask));
    memset(opt->value_slot + first, 0, count * sizeof(*opt->value_slot));
    memset(opt->value_has_slot + first, 0, count * sizeof(*opt->value_has_slot));
    memset(opt->def_index + first, 0xFF, count * sizeof(*opt->def_index));
    memset(opt->last_use + first, 0, count * sizeof(*opt->last_use));
}

// Record that `slot` currently holds `value`, and that it is the only slot the
// value may be recovered from.
//
// A value can genuinely sit in two slots at once -- `mfspr r10,lr` leaves it in
// both LR and GPR10 -- and forwarding will then justify one later use by "LR
// holds it" and another by "GPR10 holds it". Both justifications are true along
// the block, and neither survives entering the block below the code that made
// them equal: the two slots hold unrelated values then, and no single reload
// satisfies both uses. Keeping one association means every forwarded use of the
// value rests on the same reload, which the entry stub can actually perform.
static void hold_in_slot(Optimizer* opt, u32 slot, DolIRValue value,
                         u32 guest_pc) {
    DolIRValue previous = opt->slot_value[slot];
    if (previous && opt->value_slot[previous] == (u8)slot)
        opt->value_has_slot[previous] = 0;
    if (value && opt->value_has_slot[value] &&
        opt->value_slot[value] != (u8)slot)
        opt->slot_value[opt->value_slot[value]] = DOLIR_NO_VALUE;
    opt->slot_value[slot] = value;
    opt->slot_value_pc[slot] = guest_pc;
    if (value) {
        opt->value_slot[value] = (u8)slot;
        opt->value_has_slot[value] = 1;
    }
}

static DolIRValue resolve(Optimizer* opt, DolIRValue value) {
    while (value && opt->alias[value])
        value = opt->alias[value];
    return value;
}

static bool constant_of(Optimizer* opt, DolIRValue value, u64* out) {
    value = resolve(opt, value);
    if (!value || !opt->is_constant[value])
        return false;
    *out = opt->constant_value[value];
    return true;
}

// ---------------------------------------------------------------------------
// Pass 0: condition-register field fusion
// ---------------------------------------------------------------------------

// Every compare and every record-form instruction in the guest ends with a
// condition-register field update, and the builder writes it out longhand:
// three comparisons, three selects, a read of XER for the summary-overflow
// bit, and a read-modify-write of CR. Twenty-nine DolIR instructions, which a
// native backend folds back down to a handful of machine instructions and an
// interpreter would otherwise pay for one dispatch at a time.
//
// The shape is fixed, because one function emits all of it, so this matches the
// literal sequence on freshly built IR -- before anything has been folded,
// renumbered or merged -- and replaces the whole run with a single helper. It is
// a peephole in the strict sense: an exact match or nothing happens, so a change
// to the builder costs this optimization and cannot cost correctness.

typedef struct {
    u32 field;
    bool is_signed;
    DolIRValue lhs;
    DolIRValue rhs;
} CRFieldMatch;

static bool is_constant(const DolIRInstruction* inst, u64 value) {
    return inst->op == DOLIR_OP_CONSTANT && inst->type == DOLIR_TYPE_I32 &&
           inst->immediate == value;
}

static bool is_select(const DolIRInstruction* inst, DolIRValue condition,
                      DolIRValue set, DolIRValue clear) {
    return inst->op == DOLIR_OP_SELECT && inst->type == DOLIR_TYPE_I32 &&
           inst->operand_count == 3 && inst->operands[0] == condition &&
           inst->operands[1] == set && inst->operands[2] == clear;
}

static bool is_binary(const DolIRInstruction* inst, DolIROp op, DolIRType type,
                      DolIRValue left, DolIRValue right) {
    return inst->op == op && inst->type == type && inst->operand_count == 2 &&
           inst->operands[0] == left && inst->operands[1] == right;
}

#define DOLVM_CR_FIELD_LENGTH 29u

static bool match_cr_field(const DolIRBlock* block, u32 at, CRFieldMatch* out) {
    if (at + DOLVM_CR_FIELD_LENGTH > block->instruction_count)
        return false;
    const DolIRInstruction* v = block->instructions + at;

    DolIROp compare = v[0].op;
    if (compare != DOLIR_OP_ICMP_SLT && compare != DOLIR_OP_ICMP_ULT)
        return false;
    if (v[0].type != DOLIR_TYPE_I1 || v[0].operand_count != 2)
        return false;
    DolIRValue lhs = v[0].operands[0];
    DolIRValue rhs = v[0].operands[1];
    if (!is_binary(&v[1], compare, DOLIR_TYPE_I1, rhs, lhs) ||
        !is_binary(&v[2], DOLIR_OP_ICMP_EQ, DOLIR_TYPE_I1, lhs, rhs))
        return false;
    if (!is_constant(&v[3], 8) || !is_constant(&v[4], 0) ||
        !is_select(&v[5], v[0].result, v[3].result, v[4].result))
        return false;
    if (!is_constant(&v[6], 4) || !is_constant(&v[7], 0) ||
        !is_select(&v[8], v[1].result, v[6].result, v[7].result))
        return false;
    if (!is_binary(&v[9], DOLIR_OP_OR, DOLIR_TYPE_I32, v[5].result, v[8].result))
        return false;
    if (!is_constant(&v[10], 2) || !is_constant(&v[11], 0) ||
        !is_select(&v[12], v[2].result, v[10].result, v[11].result))
        return false;
    if (!is_binary(&v[13], DOLIR_OP_OR, DOLIR_TYPE_I32, v[9].result,
                   v[12].result))
        return false;
    if (v[14].op != DOLIR_OP_STATE_READ || v[14].aux != DOLIR_STATE_XER)
        return false;
    if (!is_constant(&v[15], 31) ||
        !is_binary(&v[16], DOLIR_OP_LSHR, DOLIR_TYPE_I32, v[14].result,
                   v[15].result))
        return false;
    if (!is_constant(&v[17], 1) ||
        !is_binary(&v[18], DOLIR_OP_AND, DOLIR_TYPE_I32, v[16].result,
                   v[17].result))
        return false;
    if (!is_binary(&v[19], DOLIR_OP_OR, DOLIR_TYPE_I32, v[13].result,
                   v[18].result))
        return false;
    if (v[20].op != DOLIR_OP_CONSTANT || v[20].type != DOLIR_TYPE_I32)
        return false;
    u32 shift = (u32)v[20].immediate;
    if (shift > 28u || (shift & 3u))
        return false;
    u32 mask = 0xFu << shift;
    if (!is_binary(&v[21], DOLIR_OP_SHL, DOLIR_TYPE_I32, v[19].result,
                   v[20].result))
        return false;
    if (v[22].op != DOLIR_OP_STATE_READ || v[22].aux != DOLIR_STATE_CR)
        return false;
    if (!is_constant(&v[23], ~mask) ||
        !is_binary(&v[24], DOLIR_OP_AND, DOLIR_TYPE_I32, v[22].result,
                   v[23].result))
        return false;
    if (!is_constant(&v[25], mask) ||
        !is_binary(&v[26], DOLIR_OP_AND, DOLIR_TYPE_I32, v[21].result,
                   v[25].result))
        return false;
    if (!is_binary(&v[27], DOLIR_OP_OR, DOLIR_TYPE_I32, v[24].result,
                   v[26].result))
        return false;
    if (v[28].op != DOLIR_OP_STATE_WRITE || v[28].aux != DOLIR_STATE_CR ||
        v[28].operand_count != 1 || v[28].operands[0] != v[27].result)
        return false;

    // Nothing the run computed may be read after it; the run is the only
    // consumer of its own temporaries when the builder emitted it.
    for (u32 i = at + DOLVM_CR_FIELD_LENGTH; i < block->instruction_count; i++) {
        const DolIRInstruction* later = &block->instructions[i];
        for (u32 o = 0; o < later->operand_count; o++) {
            DolIRValue operand = later->operands[o];
            for (u32 n = 0; n < DOLVM_CR_FIELD_LENGTH; n++)
                if (v[n].result && v[n].result == operand)
                    return false;
        }
    }
    if (block->terminator.condition || block->terminator.target_value) {
        for (u32 n = 0; n < DOLVM_CR_FIELD_LENGTH; n++) {
            if (!v[n].result)
                continue;
            if (v[n].result == block->terminator.condition ||
                v[n].result == block->terminator.target_value)
                return false;
        }
    }

    out->field = 7u - shift / 4u;
    out->is_signed = compare == DOLIR_OP_ICMP_SLT;
    out->lhs = lhs;
    out->rhs = rhs;
    return true;
}

// Two instructions read a width from the *type their operand was written at*,
// and the passes below are about to make that unrecoverable: eliding a truncate
// leaves a value with a wider declared type than the instruction reading it
// meant. A signed comparison and a sign extension both give different answers
// at different widths, so each records the width it was built with, in an aux
// field neither of them otherwise uses, while the original types still stand.
static void stamp_operand_widths(DolIRFunction* function) {
    for (u32 b = 0; b < function->block_count; b++) {
        DolIRBlock* block = &function->blocks[b];
        for (u32 i = 0; i < block->instruction_count; i++) {
            DolIRInstruction* inst = &block->instructions[i];
            if (inst->op != DOLIR_OP_ICMP_SLT && inst->op != DOLIR_OP_ICMP_SLE &&
                inst->op != DOLIR_OP_SEXT)
                continue;
            if (!inst->operand_count || !inst->operands[0])
                continue;
            inst->aux = (u32)function->value_types[inst->operands[0]];
        }
    }
}

static void fuse_cr_fields(DolIRFunction* function, DolVMOptStats* stats) {
    for (u32 b = 0; b < function->block_count; b++) {
        DolIRBlock* block = &function->blocks[b];
        u32 written = 0;
        for (u32 i = 0; i < block->instruction_count;) {
            CRFieldMatch match;
            if (!match_cr_field(block, i, &match)) {
                if (written != i)
                    block->instructions[written] = block->instructions[i];
                written++;
                i++;
                continue;
            }
            DolIRInstruction* fused = &block->instructions[written++];
            u32 guest_pc = block->instructions[i].guest_pc;
            memset(fused, 0, sizeof(*fused));
            fused->op = DOLIR_OP_HELPER_CALL;
            fused->type = DOLIR_TYPE_VOID;
            fused->aux = DOLIR_HELPER_CR_FIELD;
            fused->operands[0] = match.lhs;
            fused->operands[1] = match.rhs;
            fused->operand_count = 2;
            fused->immediate = match.field | (match.is_signed ? 0x100u : 0u);
            fused->guest_pc = guest_pc;
            fused->effects = DOLIR_EFFECT_READ_STATE | DOLIR_EFFECT_WRITE_STATE;
            stats->cr_fields_fused++;
            i += DOLVM_CR_FIELD_LENGTH;
        }
        block->instruction_count = written;
    }
}

// ---------------------------------------------------------------------------
// Pass 1: superblock formation
// ---------------------------------------------------------------------------

// How many VM registers a block could need at once: one per value it defines,
// two for a vector, since every value is live from its definition and the later
// passes only ever remove definitions. Bounding a superblock by this is what
// makes the emitter's allocator unable to fail -- registers are block-local and
// the file has no spill area, so an over-large merge would have nowhere to go.
// The bound is roughly ten times what a merged block actually reaches, so it
// costs nothing on ordinary code and only clips the pathological runs.
//
// The homed guest state sits above this, and is not allocatable, so the budget
// is what the block-local pool holds rather than the whole file.
#define DOLVM_MERGE_VALUE_BUDGET DOLVM_LOCAL_REGISTERS

static u32 block_value_pressure(const DolIRBlock* block) {
    u32 pressure = 0;
    for (u32 i = 0; i < block->instruction_count; i++) {
        const DolIRInstruction* inst = &block->instructions[i];
        if (!inst->result)
            continue;
        pressure += (inst->type == DOLIR_TYPE_V2F32 ||
                     inst->type == DOLIR_TYPE_V2F64)
                        ? 2u
                        : 1u;
    }
    return pressure;
}

// Merge a chain of unconditional fallthroughs into one block. A block joins its
// predecessor only when that predecessor is its *only* control-flow source, so
// what comes out is still a basic block in the ordinary sense; the chassis may
// enter it at an interior address, which is exactly what the C backend allows
// for its own blocks and what the entry map below is for.
static bool merge_blocks(Optimizer* opt) {
    DolIRFunction* function = opt->function;
    u32 count = function->block_count;
    if (!count)
        return true;

    u32* predecessors = (u32*)calloc(count, sizeof(*predecessors));
    u32* head = (u32*)malloc((size_t)count * sizeof(*head));
    u32* remap = (u32*)malloc((size_t)count * sizeof(*remap));
    DolIRBlock* blocks = (DolIRBlock*)calloc(count, sizeof(*blocks));
    if (!predecessors || !head || !remap || !blocks) {
        free(predecessors);
        free(head);
        free(remap);
        free(blocks);
        return false;
    }

    for (u32 b = 0; b < count; b++) {
        const DolIRTerminator* term = &function->blocks[b].terminator;
        u32 edges = term->kind == DOLIR_TERM_COND_BRANCH ? 2u
                    : term->kind == DOLIR_TERM_BRANCH    ? 1u
                    : term->kind == DOLIR_TERM_INDIRECT  ? 2u
                                                         : 0u;
        for (u32 e = 0; e < edges; e++) {
            u32 target = term->targets[e];
            if (target != DOLIR_NO_BLOCK && target < count)
                predecessors[target]++;
        }
        // A fallback resumes at the following instruction without naming it as
        // an edge, so the block after one still has a predecessor.
        if (term->kind == DOLIR_TERM_FALLBACK && b + 1u < count)
            predecessors[b + 1u]++;
    }

    u32* pressure = (u32*)malloc((size_t)count * sizeof(*pressure));
    if (!pressure) {
        free(predecessors);
        free(head);
        free(remap);
        free(blocks);
        return false;
    }
    for (u32 b = 0; b < count; b++) {
        head[b] = b;
        pressure[b] = block_value_pressure(&function->blocks[b]);
    }
    for (u32 b = 0; b + 1u < count; b++) {
        const DolIRTerminator* term = &function->blocks[b].terminator;
        if (term->kind != DOLIR_TERM_BRANCH || term->linked ||
            term->targets[0] != b + 1u || predecessors[b + 1u] != 1u)
            continue;
        u32 chain = head[b];
        u32 joined = pressure[chain] + pressure[b + 1u];
        if (joined > DOLVM_MERGE_VALUE_BUDGET || joined < pressure[chain])
            continue;
        pressure[chain] = joined;
        head[b + 1u] = chain;
    }
    free(pressure);

    u32 written = 0;
    for (u32 b = 0; b < count; b++) {
        if (head[b] == b)
            remap[b] = written++;
        else
            remap[b] = remap[head[b]];
    }

    for (u32 b = 0; b < count; b++) {
        DolIRBlock* target = &blocks[remap[b]];
        const DolIRBlock* source = &function->blocks[b];
        target->instruction_capacity += source->instruction_count;
        target->cycle_cost += source->cycle_cost;
        // The chain runs in index order, so the last member wins both the
        // terminator and the guest end of the merged block.
        target->terminator = source->terminator;
        if (head[b] == b)
            target->guest_address = source->guest_address;
    }
    for (u32 n = 0; n < written; n++) {
        u32 capacity = blocks[n].instruction_capacity;
        if (!capacity)
            continue;
        blocks[n].instructions =
            (DolIRInstruction*)malloc((size_t)capacity * sizeof(DolIRInstruction));
        if (!blocks[n].instructions) {
            for (u32 m = 0; m < n; m++)
                free(blocks[m].instructions);
            free(predecessors);
            free(head);
            free(remap);
            free(blocks);
            return false;
        }
    }
    for (u32 b = 0; b < count; b++) {
        DolIRBlock* target = &blocks[remap[b]];
        DolIRBlock* source = &function->blocks[b];
        memcpy(target->instructions + target->instruction_count,
               source->instructions,
               (size_t)source->instruction_count * sizeof(DolIRInstruction));
        target->instruction_count += source->instruction_count;
        free(source->instructions);
        source->instructions = NULL;
    }

    for (u32 b = 0; b < written; b++) {
        DolIRTerminator* term = &blocks[b].terminator;
        for (u32 e = 0; e < 2; e++) {
            if (term->targets[e] != DOLIR_NO_BLOCK && term->targets[e] < count)
                term->targets[e] = remap[term->targets[e]];
        }
    }

    free(function->blocks);
    function->blocks = blocks;
    function->block_count = written;
    function->block_capacity = count;
    free(predecessors);
    free(head);
    free(remap);
    return true;
}

// ---------------------------------------------------------------------------
// Pass 2-4: local optimization
// ---------------------------------------------------------------------------

static void cse_reset(Optimizer* opt, bool constants_too) {
    for (u32 i = 0; i <= opt->table_mask; i++) {
        if (!opt->table[i].used)
            continue;
        if (constants_too || !opt->table[i].constant)
            opt->table[i].used = false;
    }
}

static u32 cse_hash(const DolIRInstruction* inst, const DolIRValue* operands) {
    u64 hash = 1469598103934665603ull;
    hash = (hash ^ inst->op) * 1099511628211ull;
    hash = (hash ^ inst->type) * 1099511628211ull;
    hash = (hash ^ inst->aux) * 1099511628211ull;
    hash = (hash ^ inst->immediate) * 1099511628211ull;
    for (u32 i = 0; i < inst->operand_count; i++)
        hash = (hash ^ operands[i]) * 1099511628211ull;
    return (u32)(hash ^ (hash >> 32));
}

static DolIRValue cse_lookup_or_insert(Optimizer* opt,
                                       const DolIRInstruction* inst,
                                       const DolIRValue* operands,
                                       DolIRValue result) {
    u32 index = cse_hash(inst, operands) & opt->table_mask;
    for (u32 probe = 0; probe <= opt->table_mask; probe++) {
        CSEEntry* entry = &opt->table[index];
        if (!entry->used) {
            entry->used = true;
            entry->constant = inst->op == DOLIR_OP_CONSTANT;
            entry->op = inst->op;
            entry->type = inst->type;
            entry->aux = inst->aux;
            entry->immediate = inst->immediate;
            memset(entry->key_operands, 0, sizeof(entry->key_operands));
            for (u32 i = 0; i < inst->operand_count; i++)
                entry->key_operands[i] = operands[i];
            entry->result = result;
            return DOLIR_NO_VALUE;
        }
        if (entry->op == (u32)inst->op && entry->type == (u32)inst->type &&
            entry->aux == inst->aux && entry->immediate == inst->immediate) {
            bool same = true;
            for (u32 i = 0; i < 4; i++) {
                DolIRValue want = i < inst->operand_count ? operands[i] : 0u;
                same = same && entry->key_operands[i] == want;
            }
            if (same)
                return entry->result;
        }
        index = (index + 1u) & opt->table_mask;
    }
    return DOLIR_NO_VALUE;
}

static u64 fold_binary(DolIROp op, DolIRType type, u64 a, u64 b, bool* folded) {
    u64 mask = type_mask(type);
    u32 bits = type == DOLIR_TYPE_I64 ? 64u : type == DOLIR_TYPE_I32 ? 32u
               : type == DOLIR_TYPE_I16                              ? 16u
               : type == DOLIR_TYPE_I8                               ? 8u
                                                                     : 1u;
    *folded = true;
    switch (op) {
    case DOLIR_OP_ADD: return (a + b) & mask;
    case DOLIR_OP_SUB: return (a - b) & mask;
    case DOLIR_OP_MUL: return (a * b) & mask;
    case DOLIR_OP_AND: return (a & b) & mask;
    case DOLIR_OP_OR: return (a | b) & mask;
    case DOLIR_OP_XOR: return (a ^ b) & mask;
    case DOLIR_OP_UDIV: return b ? (a / b) & mask : 0u;
    case DOLIR_OP_SDIV: {
        if (!b) return 0u;
        s64 sa = (s64)((a ^ (1ull << (bits - 1u))) - (1ull << (bits - 1u)));
        s64 sb = (s64)((b ^ (1ull << (bits - 1u))) - (1ull << (bits - 1u)));
        if (sb == -1 && sa == -(s64)(1ull << (bits - 1u)))
            return 0u;
        return (u64)(sa / sb) & mask;
    }
    case DOLIR_OP_SHL: return (a << (b & (bits - 1u))) & mask;
    case DOLIR_OP_LSHR: return (a & mask) >> (b & (bits - 1u));
    case DOLIR_OP_ASHR: {
        u64 sign = 1ull << (bits - 1u);
        s64 value = (s64)(((a & mask) ^ sign) - sign);
        return (u64)(value >> (b & (bits - 1u))) & mask;
    }
    case DOLIR_OP_ROTL: {
        u32 shift = (u32)b & (bits - 1u);
        u64 value = a & mask;
        return shift ? ((value << shift) | (value >> (bits - shift))) & mask
                     : value;
    }
    case DOLIR_OP_ICMP_EQ: return (a & mask) == (b & mask);
    case DOLIR_OP_ICMP_NE: return (a & mask) != (b & mask);
    case DOLIR_OP_ICMP_ULT: return (a & mask) < (b & mask);
    case DOLIR_OP_ICMP_ULE: return (a & mask) <= (b & mask);
    default:
        *folded = false;
        return 0u;
    }
}

// The signed comparisons need the *operand* width, which is not the i1 result
// type, so they are folded apart from the table above.
static bool fold_signed_compare(DolIROp op, DolIRType operand_type, u64 a,
                                u64 b, u64* out) {
    if (op != DOLIR_OP_ICMP_SLT && op != DOLIR_OP_ICMP_SLE)
        return false;
    u32 bits = operand_type == DOLIR_TYPE_I64 ? 64u : 32u;
    u64 sign = 1ull << (bits - 1u);
    u64 mask = bits == 64u ? ~0ull : 0xFFFFFFFFull;
    s64 sa = (s64)(((a & mask) ^ sign) - sign);
    s64 sb = (s64)(((b & mask) ^ sign) - sign);
    *out = op == DOLIR_OP_ICMP_SLT ? (u64)(sa < sb) : (u64)(sa <= sb);
    return true;
}

static u64 mask_of(Optimizer* opt, DolIRValue value, DolIRType type) {
    value = resolve(opt, value);
    if (!value)
        return type_mask(type);
    u64 mask = opt->known_mask[value];
    return mask ? mask : type_mask(type);
}

static void set_constant(Optimizer* opt, DolIRValue result, DolIRType type,
                         u64 value) {
    opt->is_constant[result] = 1;
    opt->constant_value[result] = value & type_mask(type);
    opt->known_mask[result] = value & type_mask(type);
}

// Return a value this instruction is simply equal to, or DOLIR_NO_VALUE.
static DolIRValue simplify(Optimizer* opt, const DolIRInstruction* inst,
                           const DolIRValue* ops) {
    u64 a = 0, b = 0;
    bool a_const = inst->operand_count > 0 && constant_of(opt, ops[0], &a);
    bool b_const = inst->operand_count > 1 && constant_of(opt, ops[1], &b);
    u64 mask = type_mask(inst->type);

    switch (inst->op) {
    case DOLIR_OP_ADD:
    case DOLIR_OP_SUB:
    case DOLIR_OP_OR:
    case DOLIR_OP_XOR:
        if (b_const && (b & mask) == 0)
            return ops[0];
        if (inst->op == DOLIR_OP_ADD && a_const && (a & mask) == 0)
            return ops[1];
        if (inst->op == DOLIR_OP_OR && a_const && (a & mask) == 0)
            return ops[1];
        if (inst->op == DOLIR_OP_XOR && a_const && (a & mask) == 0)
            return ops[1];
        if (inst->op == DOLIR_OP_OR && ops[0] == ops[1])
            return ops[0];
        return DOLIR_NO_VALUE;
    case DOLIR_OP_MUL:
        if (b_const && (b & mask) == 1u)
            return ops[0];
        if (a_const && (a & mask) == 1u)
            return ops[1];
        return DOLIR_NO_VALUE;
    case DOLIR_OP_AND:
        if (ops[0] == ops[1])
            return ops[0];
        // A mask that cannot clear any bit the value might have is not a mask.
        if (b_const && (mask_of(opt, ops[0], inst->type) & ~b & mask) == 0)
            return ops[0];
        if (a_const && (mask_of(opt, ops[1], inst->type) & ~a & mask) == 0)
            return ops[1];
        return DOLIR_NO_VALUE;
    case DOLIR_OP_SHL:
    case DOLIR_OP_LSHR:
    case DOLIR_OP_ASHR:
    case DOLIR_OP_ROTL:
        if (b_const && (b & (inst->type == DOLIR_TYPE_I64 ? 63u : 31u)) == 0)
            return ops[0];
        return DOLIR_NO_VALUE;
    case DOLIR_OP_ZEXT:
        // Values are held zero-extended already, so widening is a rename.
        return ops[0];
    case DOLIR_OP_BITCAST:
        // A VM register is untyped storage; a bitcast never moves a bit.
        return ops[0];
    case DOLIR_OP_TRUNC:
        if ((mask_of(opt, ops[0], DOLIR_TYPE_I64) & ~mask) == 0)
            return ops[0];
        return DOLIR_NO_VALUE;
    case DOLIR_OP_SELECT:
        if (ops[1] == ops[2])
            return ops[1];
        if (a_const)
            return (a & 1u) ? ops[1] : ops[2];
        return DOLIR_NO_VALUE;
    default:
        return DOLIR_NO_VALUE;
    }
}

static u64 propagate_mask(Optimizer* opt, const DolIRInstruction* inst,
                          const DolIRValue* ops) {
    u64 result = type_mask(inst->type);
    u64 a = inst->operand_count > 0 ? mask_of(opt, ops[0], inst->type) : result;
    u64 b = inst->operand_count > 1 ? mask_of(opt, ops[1], inst->type) : result;
    u64 constant = 0;
    switch (inst->op) {
    case DOLIR_OP_ADD: {
        // A sum cannot exceed the widest operand rounded up to all-ones,
        // doubled. That one extra bit is what makes a carry out of a 32-bit
        // add provably one bit wide, which in turn makes the truncate that
        // extracts it -- emitted by every addc, adde and addic in the guest --
        // disappear.
        u64 filled = a | b;
        filled |= filled >> 1;
        filled |= filled >> 2;
        filled |= filled >> 4;
        filled |= filled >> 8;
        filled |= filled >> 16;
        filled |= filled >> 32;
        return ((filled << 1) | filled | 1ull) & result;
    }
    case DOLIR_OP_AND:
        return a & b;
    case DOLIR_OP_OR:
    case DOLIR_OP_XOR:
        return (a | b) & result;
    case DOLIR_OP_SELECT:
        return (mask_of(opt, ops[1], inst->type) |
                mask_of(opt, ops[2], inst->type)) & result;
    case DOLIR_OP_SHL:
        if (constant_of(opt, ops[1], &constant))
            return (a << (constant & 63u)) & result;
        return result;
    case DOLIR_OP_LSHR:
        if (constant_of(opt, ops[1], &constant))
            return (a & result) >> (constant & 63u);
        return result;
    case DOLIR_OP_TRUNC:
        return mask_of(opt, ops[0], DOLIR_TYPE_I64) & result;
    case DOLIR_OP_ZEXT:
        return mask_of(opt, ops[0], DOLIR_TYPE_I64) & result;
    case DOLIR_OP_CLZ:
        return inst->type == DOLIR_TYPE_I64 ? 0x7Full : 0x3Full;
    case DOLIR_OP_ICMP_EQ:
    case DOLIR_OP_ICMP_NE:
    case DOLIR_OP_ICMP_ULT:
    case DOLIR_OP_ICMP_ULE:
    case DOLIR_OP_ICMP_SLT:
    case DOLIR_OP_ICMP_SLE:
    case DOLIR_OP_FCMP_OEQ:
    case DOLIR_OP_FCMP_OLT:
    case DOLIR_OP_FCMP_OGE:
        return 1ull;
    case DOLIR_OP_GUEST_LOAD: {
        u32 width = inst->aux & 0xFFu;
        bool sign = (inst->aux & 0x100u) != 0;
        if (sign || width >= 8u)
            return result;
        return ((1ull << (width * 8u)) - 1u) & result;
    }
    default:
        return result;
    }
}

static DolVMEntry* record_boundary(Optimizer* opt, u32 block, u32 instruction,
                                   u32 guest_address) {
    DolVMLayout* layout = opt->layout;
    if (guest_address < opt->function->guest_start ||
        guest_address >= opt->function->guest_end)
        return NULL;
    u32 index = (guest_address - opt->function->guest_start) / 4u;
    if (index >= layout->entry_count)
        return NULL;
    DolVMEntry* entry = &layout->entries[index];
    entry->guest_address = guest_address;
    entry->block = block;
    entry->instruction = instruction;
    entry->recipe_start = layout->recipe_count;
    entry->recipe_count = 0;
    return entry;
}

static bool layout_push_recipe(DolVMLayout* layout, DolIRValue value, u8 kind,
                               u8 slot, u8 type, u64 immediate) {
    if (layout->recipe_count == layout->recipe_capacity) {
        u32 capacity = layout->recipe_capacity ? layout->recipe_capacity * 2u : 64u;
        DolVMRecipe* grown = (DolVMRecipe*)realloc(
            layout->recipes, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return false;
        layout->recipes = grown;
        layout->recipe_capacity = capacity;
    }
    DolVMRecipe* recipe = &layout->recipes[layout->recipe_count++];
    recipe->value = value;
    recipe->kind = kind;
    recipe->slot = slot;
    recipe->type = type;
    recipe->immediate = immediate;
    return true;
}

// A guest-instruction boundary. Everything the block still knows is either
// recreatable at this address or has to be forgotten; see the header.
static void cross_boundary(Optimizer* opt) {
    cse_reset(opt, false);
    for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
        opt->slot_write_index[slot] = DOLIR_NO_BLOCK;
        opt->slot_read_since[slot] = 0;
        // What a value *is*, past this point, is "whatever that slot holds" --
        // and an entry here reloads it from the slot, which may hold something
        // else entirely. So it stops being a known constant, or code after the
        // boundary would be specialised on a number that only holds when the
        // block was entered above the instruction that put it there.
        //
        //     li   r0, 20        <- value V is the constant 20
        //     divwu r3, r3, r0   <- folds the divide-by-zero guard away
        //
        // Enter at the divide with r0 = 0 and the guard has to still be there.
        DolIRValue held = opt->slot_value[slot];
        if (held && opt->is_constant[held]) {
            opt->is_constant[held] = 0;
            opt->constant_value[held] = 0;
            opt->known_mask[held] =
                type_mask(opt->function->value_types[held]);
        }
    }
}

static bool optimize_block(Optimizer* opt, u32 block_index) {
    DolIRFunction* function = opt->function;
    DolIRBlock* block = &function->blocks[block_index];
    u32 count = block->instruction_count;

    // Values are handed out in program order, so this block owns a contiguous
    // slice of the value space and only that slice needs clearing.
    DolIRValue first = function->value_count;
    DolIRValue last = 0;
    for (u32 i = 0; i < count; i++) {
        DolIRValue result = block->instructions[i].result;
        if (!result)
            continue;
        if (result < first)
            first = result;
        if (result > last)
            last = result;
    }
    if (last)
        reset_value_range(opt, first, last);
    cse_reset(opt, true);
    memset(opt->slot_value, 0, sizeof(opt->slot_value));
    memset(opt->slot_value_pc, 0, sizeof(opt->slot_value_pc));
    memset(opt->slot_read_since, 0, sizeof(opt->slot_read_since));
    for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++)
        opt->slot_write_index[slot] = DOLIR_NO_BLOCK;

    if (count > opt->dead_capacity) {
        u8* grown = (u8*)realloc(opt->dead, count);
        if (!grown)
            return false;
        opt->dead = grown;
        opt->dead_capacity = count;
    }
    memset(opt->dead, 0, count);

    u32 current_pc = 0xFFFFFFFFu;
    for (u32 i = 0; i < count; i++) {
        DolIRInstruction* inst = &block->instructions[i];
        if (inst->guest_pc != current_pc) {
            if (current_pc != 0xFFFFFFFFu)
                cross_boundary(opt);
            current_pc = inst->guest_pc;
        }

        DolIRValue ops[4] = {0, 0, 0, 0};
        for (u32 n = 0; n < inst->operand_count; n++) {
            ops[n] = resolve(opt, inst->operands[n]);
            inst->operands[n] = ops[n];
        }

        if (inst->op == DOLIR_OP_STATE_READ) {
            DolIRValue cached = opt->slot_value[inst->aux];
            if (cached) {
                opt->alias[inst->result] = cached;
                opt->slot_read_since[inst->aux] = 1;
                opt->dead[i] = 1;
                opt->stats->state_reads_forwarded++;
                continue;
            }
            hold_in_slot(opt, inst->aux, inst->result, inst->guest_pc);
            opt->known_mask[inst->result] = type_mask(inst->type);
            opt->def_index[inst->result] = i;
            continue;
        }

        if (inst->op == DOLIR_OP_STATE_WRITE) {
            DolIRValue value = ops[0];
            // A store of what the slot already holds is only dead if the block
            // established that within this same guest instruction. Across a
            // boundary the store is the thing that makes the two equal, and an
            // entry below the code that set them up would skip it.
            if (opt->slot_value[inst->aux] == value && value &&
                opt->slot_value_pc[inst->aux] == inst->guest_pc) {
                opt->dead[i] = 1;
                opt->stats->state_writes_removed++;
                continue;
            }
            u32 previous = opt->slot_write_index[inst->aux];
            // Only a store this same guest instruction already overwrote is
            // dead. Across a boundary the slot is a recipe source, so a store
            // that looks redundant may be the only thing making a forwarded
            // value recoverable at the addresses in between.
            if (previous != DOLIR_NO_BLOCK &&
                opt->slot_write_pc[inst->aux] == inst->guest_pc &&
                !opt->slot_read_since[inst->aux]) {
                opt->dead[previous] = 1;
                opt->stats->state_writes_removed++;
            }
            hold_in_slot(opt, inst->aux, value, inst->guest_pc);
            opt->slot_write_index[inst->aux] = i;
            opt->slot_write_pc[inst->aux] = inst->guest_pc;
            opt->slot_read_since[inst->aux] = 0;
            continue;
        }

        if (cr_field_helper(inst)) {
            // Reads XER and CR, writes CR. Saying so keeps forwarding alive for
            // every other slot across what is now one instruction.
            hold_in_slot(opt, DOLIR_STATE_CR, DOLIR_NO_VALUE, inst->guest_pc);
            opt->slot_write_index[DOLIR_STATE_CR] = DOLIR_NO_BLOCK;
            opt->slot_read_since[DOLIR_STATE_CR] = 1;
            opt->slot_read_since[DOLIR_STATE_XER] = 1;
            continue;
        }

        if (state_barrier(inst)) {
            for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
                DolIRValue held = opt->slot_value[slot];
                if (held)
                    opt->value_has_slot[held] = 0;
            }
            memset(opt->slot_value, 0, sizeof(opt->slot_value));
            memset(opt->slot_read_since, 0, sizeof(opt->slot_read_since));
            for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++)
                opt->slot_write_index[slot] = DOLIR_NO_BLOCK;
            if (inst->result) {
                opt->known_mask[inst->result] = propagate_mask(opt, inst, ops);
                opt->def_index[inst->result] = i;
            }
            continue;
        }

        if (!pure_op(inst->op) || !inst->result) {
            if (inst->result)
                opt->def_index[inst->result] = i;
            continue;
        }

        if (inst->op == DOLIR_OP_CONSTANT) {
            DolIRValue existing = cse_lookup_or_insert(opt, inst, ops,
                                                       inst->result);
            if (existing) {
                opt->alias[inst->result] = existing;
                opt->dead[i] = 1;
                opt->stats->values_numbered++;
                continue;
            }
            if (integer_type(inst->type))
                set_constant(opt, inst->result, inst->type, inst->immediate);
            else
                opt->known_mask[inst->result] = type_mask(inst->type);
            opt->def_index[inst->result] = i;
            continue;
        }

        // Constant folding.
        if (integer_type(inst->type) && inst->operand_count == 2) {
            u64 a = 0, b = 0;
            if (constant_of(opt, ops[0], &a) && constant_of(opt, ops[1], &b)) {
                bool folded = false;
                // stamp_operand_widths recorded the width the builder wrote,
                // which resolve() above may have widened.
                DolIRType operand_type = (DolIRType)inst->aux;
                u64 value = 0;
                if (fold_signed_compare(inst->op, operand_type, a, b, &value))
                    folded = true;
                else
                    value = fold_binary(inst->op, inst->type, a, b, &folded);
                if (folded) {
                    inst->op = DOLIR_OP_CONSTANT;
                    inst->operand_count = 0;
                    inst->immediate = value;
                    inst->effects = DOLIR_EFFECT_NONE;
                    memset(inst->operands, 0, sizeof(inst->operands));
                    set_constant(opt, inst->result, inst->type, value);
                    opt->def_index[inst->result] = i;
                    opt->stats->constants_folded++;
                    DolIRValue existing =
                        cse_lookup_or_insert(opt, inst, ops, inst->result);
                    if (existing) {
                        opt->alias[inst->result] = existing;
                        opt->dead[i] = 1;
                    }
                    continue;
                }
            }
        }
        if (integer_type(inst->type) && inst->operand_count == 1) {
            u64 a = 0;
            if (constant_of(opt, ops[0], &a)) {
                bool folded = true;
                u64 mask = type_mask(inst->type);
                u64 value = 0;
                DolIRType source = inst->op == DOLIR_OP_SEXT
                                       ? (DolIRType)inst->aux
                                       : function->value_types[ops[0]];
                switch (inst->op) {
                case DOLIR_OP_NOT: value = (~a) & mask; break;
                case DOLIR_OP_TRUNC: value = a & mask; break;
                case DOLIR_OP_ZEXT: value = a & mask; break;
                case DOLIR_OP_BITCAST: value = a & mask; break;
                case DOLIR_OP_SEXT: {
                    u64 sign = (type_mask(source) >> 1) + 1u;
                    value = (((a & type_mask(source)) ^ sign) - sign) & mask;
                    break;
                }
                case DOLIR_OP_CLZ: {
                    u32 bits = inst->type == DOLIR_TYPE_I64 ? 64u : 32u;
                    u64 v = a & mask;
                    value = bits;
                    for (u32 bit = 0; bit < bits; bit++) {
                        if (v & (1ull << (bits - 1u - bit))) {
                            value = bit;
                            break;
                        }
                    }
                    break;
                }
                case DOLIR_OP_BSWAP:
                    if (inst->type == DOLIR_TYPE_I16)
                        value = bswap16((u16)a);
                    else if (inst->type == DOLIR_TYPE_I32)
                        value = bswap32((u32)a);
                    else
                        value = ((u64)bswap32((u32)a) << 32) |
                                bswap32((u32)(a >> 32));
                    break;
                default: folded = false; break;
                }
                if (folded) {
                    inst->op = DOLIR_OP_CONSTANT;
                    inst->operand_count = 0;
                    inst->immediate = value;
                    inst->effects = DOLIR_EFFECT_NONE;
                    memset(inst->operands, 0, sizeof(inst->operands));
                    set_constant(opt, inst->result, inst->type, value);
                    opt->def_index[inst->result] = i;
                    opt->stats->constants_folded++;
                    DolIRValue existing =
                        cse_lookup_or_insert(opt, inst, ops, inst->result);
                    if (existing) {
                        opt->alias[inst->result] = existing;
                        opt->dead[i] = 1;
                    }
                    continue;
                }
            }
        }

        DolIRValue equivalent = simplify(opt, inst, ops);
        if (equivalent) {
            opt->alias[inst->result] = equivalent;
            opt->dead[i] = 1;
            opt->stats->constants_folded++;
            continue;
        }

        DolIRValue existing = cse_lookup_or_insert(opt, inst, ops, inst->result);
        if (existing) {
            opt->alias[inst->result] = existing;
            opt->dead[i] = 1;
            opt->stats->values_numbered++;
            continue;
        }
        opt->known_mask[inst->result] = propagate_mask(opt, inst, ops);
        opt->def_index[inst->result] = i;
    }

    DolIRTerminator* term = &block->terminator;
    if (term->condition)
        term->condition = resolve(opt, term->condition);
    if (term->target_value)
        term->target_value = resolve(opt, term->target_value);

    // Dead code elimination. The terminator's operands are seeded first so the
    // single backward walk sees every use.
    memset(opt->last_use + first, 0,
           last >= first ? (last - first + 1u) * sizeof(*opt->last_use) : 0u);
    if (term->condition)
        opt->last_use[term->condition] = count + 1u;
    if (term->target_value)
        opt->last_use[term->target_value] = count + 1u;
    for (u32 n = count; n-- > 0;) {
        DolIRInstruction* inst = &block->instructions[n];
        if (opt->dead[n])
            continue;
        if (inst->result && pure_op(inst->op) &&
            opt->last_use[inst->result] == 0) {
            opt->dead[n] = 1;
            opt->stats->dead_removed++;
            continue;
        }
        for (u32 o = 0; o < inst->operand_count; o++)
            if (inst->operands[o])
                opt->last_use[inst->operands[o]] = n + 1u;
    }

    u32 written = 0;
    for (u32 n = 0; n < count; n++) {
        if (opt->dead[n])
            continue;
        if (written != n)
            block->instructions[written] = block->instructions[n];
        written++;
    }
    block->instruction_count = written;
    return true;
}

// ---------------------------------------------------------------------------
// Entry table and recipes
// ---------------------------------------------------------------------------

// hold_in_slot's rule, against the local map build_entries walks.
static void replay_slot(Optimizer* opt, DolIRValue* slot_value, u32 slot,
                        DolIRValue value) {
    DolIRValue previous = slot_value[slot];
    if (previous && opt->value_slot[previous] == (u8)slot)
        opt->value_has_slot[previous] = 0;
    if (value && opt->value_has_slot[value] &&
        opt->value_slot[value] != (u8)slot)
        slot_value[opt->value_slot[value]] = DOLIR_NO_VALUE;
    slot_value[slot] = value;
    if (value) {
        opt->value_slot[value] = (u8)slot;
        opt->value_has_slot[value] = 1;
    }
}

static bool track_live_constant(Optimizer* opt, DolIRValue value) {
    if (opt->live_constant_count == opt->live_constant_capacity) {
        u32 capacity = opt->live_constant_capacity ? opt->live_constant_capacity * 2u : 64u;
        DolIRValue* grown = (DolIRValue*)realloc(
            opt->live_constants, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return false;
        opt->live_constants = grown;
        opt->live_constant_capacity = capacity;
    }
    opt->live_constants[opt->live_constant_count++] = value;
    return true;
}

// Replay the surviving block, recording for every guest address it covers the
// offset to start at and the recipes needed to recreate the values that cross
// into that address. Walking by address rather than by instruction matters: a
// guest instruction whose IR was folded away entirely still has to be an entry
// point, and it lands on whatever comes next.
static bool build_entries(Optimizer* opt, u32 block_index) {
    DolIRFunction* function = opt->function;
    DolIRBlock* block = &function->blocks[block_index];
    DolVMLayout* layout = opt->layout;
    u32 count = block->instruction_count;

    // Instruction indices moved when the block was compacted, so definitions
    // and last uses are recomputed against what actually survived.
    for (u32 i = 0; i < count; i++) {
        DolIRValue result = block->instructions[i].result;
        if (result) {
            opt->def_index[result] = i;
            opt->last_use[result] = 0;
            opt->value_has_slot[result] = 0;
        }
    }
    if (block->terminator.condition)
        opt->last_use[block->terminator.condition] = count + 1u;
    if (block->terminator.target_value)
        opt->last_use[block->terminator.target_value] = count + 1u;
    for (u32 i = count; i-- > 0;) {
        const DolIRInstruction* inst = &block->instructions[i];
        for (u32 o = 0; o < inst->operand_count; o++) {
            DolIRValue operand = inst->operands[o];
            if (operand && opt->last_use[operand] < i + 1u)
                opt->last_use[operand] = i + 1u;
        }
    }

    DolIRValue slot_value[DOLIR_STATE_COUNT];
    memset(slot_value, 0, sizeof(slot_value));
    opt->live_constant_count = 0;

    u32 first_address = block->guest_address;
    u32 last_address = block->terminator.guest_pc;
    if (last_address < first_address)
        last_address = first_address;

    u32 i = 0;
    for (u32 address = first_address; address <= last_address; address += 4u) {
        while (i < count && block->instructions[i].guest_pc < address) {
            const DolIRInstruction* inst = &block->instructions[i];
            // Mirrors optimize_block exactly; a divergence here would propose a
            // recipe the block never relied on, or omit one it did.
            if (inst->op == DOLIR_OP_STATE_READ)
                replay_slot(opt, slot_value, inst->aux, inst->result);
            else if (inst->op == DOLIR_OP_STATE_WRITE)
                replay_slot(opt, slot_value, inst->aux, inst->operands[0]);
            else if (cr_field_helper(inst))
                replay_slot(opt, slot_value, DOLIR_STATE_CR, DOLIR_NO_VALUE);
            else if (state_barrier(inst)) {
                for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++)
                    if (slot_value[slot])
                        opt->value_has_slot[slot_value[slot]] = 0;
                memset(slot_value, 0, sizeof(slot_value));
            } else if (inst->op == DOLIR_OP_CONSTANT && inst->result &&
                       !track_live_constant(opt, inst->result))
                return false;
            i++;
        }

        DolVMEntry* entry = record_boundary(opt, block_index, i, address);
        if (!entry)
            continue;
        for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
            DolIRValue value = slot_value[slot];
            if (!value || opt->def_index[value] >= i || opt->last_use[value] <= i)
                continue;
            if (!layout_push_recipe(layout, value, DOLVM_RECIPE_STATE, (u8)slot,
                                    (u8)function->value_types[value], 0))
                return false;
            entry->recipe_count++;
            opt->stats->boundary_recipes++;
        }
        for (u32 n = 0; n < opt->live_constant_count;) {
            DolIRValue value = opt->live_constants[n];
            if (opt->last_use[value] <= i) {
                opt->live_constants[n] =
                    opt->live_constants[--opt->live_constant_count];
                continue;
            }
            if (opt->def_index[value] < i) {
                if (!layout_push_recipe(
                        layout, value, DOLVM_RECIPE_CONSTANT, 0,
                        (u8)function->value_types[value],
                        block->instructions[opt->def_index[value]].immediate))
                    return false;
                entry->recipe_count++;
                opt->stats->boundary_recipes++;
            }
            n++;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void dolvm_layout_free(DolVMLayout* layout) {
    if (!layout)
        return;
    free(layout->entries);
    free(layout->recipes);
    memset(layout, 0, sizeof(*layout));
}

static void mark_return_targets(DolIRFunction* function, DolVMLayout* layout) {
    for (u32 b = 0; b < function->block_count; b++) {
        const DolIRTerminator* term = &function->blocks[b].terminator;
        if (!term->linked)
            continue;
        u32 continuation = term->guest_pc + 4u;
        if (continuation < function->guest_start ||
            continuation >= function->guest_end)
            continue;
        u32 index = (continuation - function->guest_start) / 4u;
        if (index < layout->entry_count)
            layout->entries[index].return_target = 1;
    }
}

bool dolvm_optimize_function(DolIRFunction* function, DolVMLayout* layout,
                             DolVMOptStats* stats) {
    DolVMOptStats local = {0};
    if (!stats)
        stats = &local;

    memset(layout, 0, sizeof(*layout));
    u32 addresses = (function->guest_end - function->guest_start) / 4u;
    layout->entries = (DolVMEntry*)calloc(addresses, sizeof(*layout->entries));
    if (!layout->entries)
        return false;
    layout->entry_count = addresses;
    for (u32 i = 0; i < addresses; i++) {
        layout->entries[i].guest_address = function->guest_start + i * 4u;
        layout->entries[i].block = DOLIR_NO_BLOCK;
    }

    for (u32 b = 0; b < function->block_count; b++)
        stats->instructions_before += function->blocks[b].instruction_count;
    stats->blocks_before += function->block_count;

    Optimizer opt;
    memset(&opt, 0, sizeof(opt));
    opt.function = function;
    opt.layout = layout;
    opt.stats = stats;

    u32 values = function->value_count;
    u32 table_size = 64u;
    u32 largest = 0;
    for (u32 b = 0; b < function->block_count; b++)
        if (function->blocks[b].instruction_count > largest)
            largest = function->blocks[b].instruction_count;

    opt.alias = (DolIRValue*)calloc(values, sizeof(*opt.alias));
    opt.is_constant = (u8*)calloc(values, sizeof(*opt.is_constant));
    opt.constant_value = (u64*)calloc(values, sizeof(*opt.constant_value));
    opt.known_mask = (u64*)calloc(values, sizeof(*opt.known_mask));
    opt.value_slot = (u8*)calloc(values, sizeof(*opt.value_slot));
    opt.value_has_slot = (u8*)calloc(values, sizeof(*opt.value_has_slot));
    opt.def_index = (u32*)calloc(values, sizeof(*opt.def_index));
    opt.last_use = (u32*)calloc(values, sizeof(*opt.last_use));
    stamp_operand_widths(function);
    fuse_cr_fields(function, stats);
    if (!merge_blocks(&opt)) {
        goto fail;
    }
    for (u32 b = 0; b < function->block_count; b++)
        if (function->blocks[b].instruction_count > largest)
            largest = function->blocks[b].instruction_count;
    while (table_size < largest * 2u)
        table_size *= 2u;
    opt.table = (CSEEntry*)calloc(table_size, sizeof(*opt.table));
    opt.table_mask = table_size - 1u;
    if (!opt.alias || !opt.is_constant || !opt.constant_value ||
        !opt.known_mask || !opt.value_slot || !opt.value_has_slot ||
        !opt.def_index || !opt.last_use || !opt.table)
        goto fail;

    for (u32 b = 0; b < function->block_count; b++) {
        if (!optimize_block(&opt, b) || !build_entries(&opt, b))
            goto fail;
    }
    mark_return_targets(function, layout);

    for (u32 b = 0; b < function->block_count; b++)
        stats->instructions_after += function->blocks[b].instruction_count;
    stats->blocks_after += function->block_count;

    free(opt.alias);
    free(opt.is_constant);
    free(opt.constant_value);
    free(opt.known_mask);
    free(opt.value_slot);
    free(opt.value_has_slot);
    free(opt.def_index);
    free(opt.last_use);
    free(opt.table);
    free(opt.dead);
    free(opt.live_constants);
    return true;

fail:
    free(opt.alias);
    free(opt.is_constant);
    free(opt.constant_value);
    free(opt.known_mask);
    free(opt.value_slot);
    free(opt.value_has_slot);
    free(opt.def_index);
    free(opt.last_use);
    free(opt.table);
    free(opt.dead);
    free(opt.live_constants);
    dolvm_layout_free(layout);
    return false;
}

void dolvm_stats_add(DolVMOptStats* total, const DolVMOptStats* delta) {
    total->instructions_before += delta->instructions_before;
    total->instructions_after += delta->instructions_after;
    total->blocks_before += delta->blocks_before;
    total->blocks_after += delta->blocks_after;
    total->state_reads_forwarded += delta->state_reads_forwarded;
    total->state_writes_removed += delta->state_writes_removed;
    total->constants_folded += delta->constants_folded;
    total->values_numbered += delta->values_numbered;
    total->dead_removed += delta->dead_removed;
    total->boundary_recipes += delta->boundary_recipes;
    total->cr_fields_fused += delta->cr_fields_fused;
}

void dolvm_stats_report(const DolVMOptStats* stats, const char* label,
                        FILE* out) {
    double kept = stats->instructions_before
                      ? 100.0 * (double)stats->instructions_after /
                            (double)stats->instructions_before
                      : 0.0;
    double per_block = stats->blocks_after
                           ? (double)stats->instructions_after /
                                 (double)stats->blocks_after
                           : 0.0;
    fprintf(out,
            "%s: %u -> %u IR instructions (%.1f%% kept), %u -> %u blocks "
            "(%.1f per block)\n",
            label, stats->instructions_before, stats->instructions_after, kept,
            stats->blocks_before, stats->blocks_after, per_block);
    fprintf(out,
            "%s: %u state reads forwarded, %u state writes removed, %u folded, "
            "%u numbered, %u dead, %u cr fields fused, %u entry recipes\n",
            label, stats->state_reads_forwarded, stats->state_writes_removed,
            stats->constants_folded, stats->values_numbered,
            stats->dead_removed, stats->cr_fields_fused,
            stats->boundary_recipes);
}
