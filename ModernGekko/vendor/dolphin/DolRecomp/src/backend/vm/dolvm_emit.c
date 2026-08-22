// SPDX-License-Identifier: GPL-3.0-or-later
//
// DolIR -> DolVM lowering.
//
// Three things happen here that the interpreter would otherwise pay for on
// every execution:
//
//   Immediate forms. The IR materializes every constant into its own value
//   because a native backend folds them back into the instruction for free. An
//   interpreter cannot, so a constant operand is folded into the opcode instead
//   -- `add32i` rather than `const32` plus `add32` -- which removes a whole
//   dispatch, not just a register.
//
//   Displacement fusion. `lwz r3,8(r4)` builds an address with an add. When
//   that add feeds nothing but memory operations, the offset moves into the
//   load's own immediate and the add disappears.
//
//   Register recycling. DolIR values are SSA and a chunk has tens of thousands
//   of them; VM registers are recycled at last use, so a block needs a few
//   dozen rather than one per value.
//
// Vector values are legalized away rather than given a wide register: a v2f64
// occupies two ordinary registers, and build/extract/shuffle become moves. The
// register file stays 8 bytes wide, which is what every scalar op wants.

#include "backend/vm/dolvm_emit.h"
#include "cpu/cpu.h"
#include "vm/dolvm.h"
#include "vm/dolvm_state.h"

#include <stdlib.h>
#include <string.h>

#define DOLVM_NO_REG 0xFFu
// What block-local allocation may hand out. The top of the file is the homed
// guest state, which is not allocatable at all, and 0xFF above that is the "no
// register" sentinel. dolvm_opt.c caps a superblock's live values to this same
// number, which is what makes allocation unable to fail.
#define DOLVM_USABLE_REGISTERS DOLVM_LOCAL_REGISTERS

typedef enum {
    PATCH_BLOCK,     // operand is a block index within the current function
    // Same, but landing one instruction into the block: the branch has already
    // paid the cycle charge the block would have opened with.
    PATCH_BLOCK_BODY,
    PATCH_ADDRESS,   // operand is a guest address anywhere in the module
} PatchKind;

typedef struct {
    u32 code_index;
    u32 operand;
    u8 kind;
} Patch;

typedef struct {
    DolVMInst* code;
    u32 code_count;
    u32 code_capacity;

    u64* constants;
    u32 constant_count;
    u32 constant_capacity;

    DolVMRegion* regions;
    u32 region_count;
    u32 region_capacity;

    DolVMEntryPoint* map;
    u32 map_count;
    u32 map_capacity;

    Patch* patches;         // cross-function, resolved once everything exists
    u32 patch_count;
    u32 patch_capacity;

    bool direct_calls;
    // Lower against the homed registers: guest registers, LR and CTR live in
    // the register file for the length of a dispatch.
    bool homed;
    FILE* diagnostics;
    bool failed;
} Builder;

typedef struct {
    Builder* builder;
    DolIRFunction* function;
    DolVMLayout* layout;

    u32* block_start;       // bytecode index of each block's body
    u8* reg;                // VM register per IR value
    u8* reg_hi;             // second register for v2f64 values
    u32* last_use;
    u32* def_index;
    u32* use_count;
    u32* memory_use_count;  // uses as the address operand of a load or store
    u8* retired;            // register already returned to the free list
    u8* folded;             // instruction index: address add folded into a load
    u8* cr_form;            // instruction index: how a CR field update reads its
                            // operands, see CRFieldForm
    u8* mem_fused;          // instruction index: guest load/store reads and
                            // writes CPUState directly
    // Homed guest state, decided in analysis for the same reason the fusions
    // are: choosing to satisfy a read from a register is what drops the use
    // counts the choice was made from.
    bool homed;
    u8* home_read;          // state read: 1 = the value is the home, 2 = copy
    u8* home_write;         // state write: 1 = the producer wrote it, 2 = copy
    u8* home_dest;          // instruction index: home to compute straight into
    u8* supervisor;         // instruction index: the privilege test folded in
    u8* shift_fused;        // AND index: the fused opcode the shift before it
                            // collapsed into, or 0
    u8* shift_amount;       // AND index: that shift's constant amount
    u32* mem_write;         // for a fused load, the state write folded into it
    // Slot offsets decided during analysis. Like the CR forms, they cannot be
    // recomputed at lowering: choosing to fuse is what drops the use counts the
    // decision was made from.
    u32* mem_base_off;
    u32* mem_value_off;
    u32* inst_offset;       // bytecode index per IR instruction, plus terminator
    u32* pc_base_at;        // pc base in effect at each instruction
    u32 inst_offset_capacity;

    u8 free_registers[DOLVM_MAX_REGISTERS];
    u32 free_count;

    Patch* patches;         // block-local, resolved at end of function
    u32 patch_count;
    u32 patch_capacity;

    // A branch whose condition is a logical negation flips the branch instead
    // of computing the negation.
    DolIRValue terminator_condition;
    bool invert_condition;

    // A branch whose condition is a constant is not a branch at all.
    bool constant_branch;
    bool constant_branch_taken;

    // The condition-register update the branch reads, folded into it. Not an
    // index because it is only ever the block's last instruction, and the value
    // it compares has to be kept alive to the terminator by hand once it is.
    bool cr_compare;
    DolIRValue cr_compare_left;

    // `blr` and `bctr` mask their target's low two bits; the branch does it.
    bool indirect_mask;
    DolIRValue indirect_target;
    u64 cr_compare_right;
    u8 cr_compare_pack;

    // A branch on one condition-register bit reads CR itself rather than a
    // register, which folds the state load, the mask and the compare away.
    bool cr_branch;
    u8 cr_branch_bit;
    u8 cr_branch_sense;

    // The block is an idle loop: it branches back to itself and does nothing
    // but read. Its back edge is marked so the interpreter can skip to the next
    // timing event instead of polling through the rest of the slice.
    bool idle_loop;

    u32 region_index;
    u32 current_block;
    u32 map_base;           // where this function's entries start in the map
    u32 pc_base;
} FunctionEmitter;

// How a condition-register field update will read its two operands. The fused
// forms exist because a guest compare is nearly always "a register against a
// small constant" or "a register against a register", and going through VM
// registers costs a dispatch per operand for no benefit -- the interpreter is
// reading CPUState either way. There is no mixed form beyond the two below, so
// the choice is made for the instruction as a whole rather than per operand.
typedef enum {
    CR_FIELD_REGS = 0,   // SET_CR_FIELD: both operands in VM registers
    CR_FIELD_STATE_I,    // CMP_STATE_I: left from CPUState, right an immediate
    CR_FIELD_STATE,      // CMP_STATE: both from CPUState
    CR_FIELD_REG_I,      // SET_CR_FIELDI: left in a register, right an immediate
} CRFieldForm;


// ---------------------------------------------------------------------------
// Growable buffers
// ---------------------------------------------------------------------------

#define GROW(field, count, capacity, type)                                    \
    do {                                                                      \
        if ((count) == (capacity)) {                                          \
            u32 next = (capacity) ? (capacity)*2u : 64u;                      \
            type* grown = (type*)realloc(field, (size_t)next * sizeof(type)); \
            if (!grown)                                                       \
                return false;                                                 \
            field = grown;                                                    \
            capacity = next;                                                  \
        }                                                                     \
    } while (0)

static bool emit_raw(Builder* builder, u8 op, u8 a, u8 b, u8 c, u32 imm) {
    GROW(builder->code, builder->code_count, builder->code_capacity, DolVMInst);
    DolVMInst* inst = &builder->code[builder->code_count++];
    inst->op = op;
    inst->a = a;
    inst->b = b;
    inst->c = c;
    inst->imm = imm;
    return true;
}

static bool emit_payload(Builder* builder, u64 payload) {
    GROW(builder->code, builder->code_count, builder->code_capacity, DolVMInst);
    memcpy(&builder->code[builder->code_count++], &payload, sizeof(payload));
    return true;
}

static bool builder_patch(Builder* builder, u32 code_index, u8 kind,
                          u32 operand) {
    GROW(builder->patches, builder->patch_count, builder->patch_capacity, Patch);
    Patch* patch = &builder->patches[builder->patch_count++];
    patch->code_index = code_index;
    patch->kind = kind;
    patch->operand = operand;
    return true;
}

static bool function_patch(FunctionEmitter* fn, u32 code_index, u8 kind,
                           u32 operand) {
    GROW(fn->patches, fn->patch_count, fn->patch_capacity, Patch);
    Patch* patch = &fn->patches[fn->patch_count++];
    patch->code_index = code_index;
    patch->kind = kind;
    patch->operand = operand;
    return true;
}

static u32 intern_constant(Builder* builder, u64 value, bool* ok) {
    for (u32 i = 0; i < builder->constant_count; i++) {
        if (builder->constants[i] == value) {
            *ok = true;
            return i;
        }
    }
    if (builder->constant_count == builder->constant_capacity) {
        u32 next = builder->constant_capacity ? builder->constant_capacity * 2u : 64u;
        u64* grown = (u64*)realloc(builder->constants, (size_t)next * sizeof(u64));
        if (!grown) {
            *ok = false;
            return 0;
        }
        builder->constants = grown;
        builder->constant_capacity = next;
    }
    *ok = true;
    builder->constants[builder->constant_count] = value;
    return builder->constant_count++;
}

static void fail(Builder* builder, const char* format, u32 value) {
    if (!builder->failed && builder->diagnostics)
        fprintf(builder->diagnostics, format, value);
    builder->failed = true;
}

// CPUState slot offsets come from vm/dolvm_state.h so the interpreter and the
// loader's compatibility check are reading the same mapping.
#define state_offset dolvm_state_offset

static u8 state_load_op(DolIRType type) {
    switch (type) {
    case DOLIR_TYPE_I1: return DOLVM_OP_LOAD_STATE8;
    case DOLIR_TYPE_I64: return DOLVM_OP_LOAD_STATE64;
    case DOLIR_TYPE_F64: return DOLVM_OP_LOAD_STATEF;
    default: return DOLVM_OP_LOAD_STATE32;
    }
}

static u8 state_store_op(DolIRType type) {
    switch (type) {
    case DOLIR_TYPE_I1: return DOLVM_OP_STORE_STATE8;
    case DOLIR_TYPE_I64: return DOLVM_OP_STORE_STATE64;
    case DOLIR_TYPE_F64: return DOLVM_OP_STORE_STATEF;
    default: return DOLVM_OP_STORE_STATE32;
    }
}

static u32 width_selector(DolIRType type) {
    switch (type) {
    case DOLIR_TYPE_I1: return DOLVM_WIDTH_1;
    case DOLIR_TYPE_I8: return DOLVM_WIDTH_8;
    case DOLIR_TYPE_I16: return DOLVM_WIDTH_16;
    case DOLIR_TYPE_I64: return DOLVM_WIDTH_64;
    default: return DOLVM_WIDTH_32;
    }
}

// Instructions the emitter may drop when nothing reads their result.
static bool pure_emit_op(DolIROp op) {
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

static bool vector_type(DolIRType type) {
    return type == DOLIR_TYPE_V2F32 || type == DOLIR_TYPE_V2F64;
}

// ---------------------------------------------------------------------------
// Homed guest state
// ---------------------------------------------------------------------------

// The VM register that stands in for a guest slot for the length of a dispatch,
// or DOLVM_NO_REG for a slot that goes on living in CPUState.
//
// The general purpose registers, LR and CTR are homed and nothing else is. They
// are where the traffic is -- four fifths of every state access a real title
// makes -- and they are also the slots no interpreter handler reads out of
// CPUState by hand, so homing them costs the interpreter nothing but a fill and
// a flush. CR and XER are read directly by the fused condition-register opcodes
// and stay put; the timebase and the exception fields are the chassis's.
static u8 home_register(const FunctionEmitter* fn, u32 slot) {
    // XER is homed whatever else the module does; see dolvm.h.
    if (slot == DOLIR_STATE_XER)
        return (u8)DOLVM_HOME_XER;
    if (!fn->homed)
        return DOLVM_NO_REG;
    if (slot <= DOLIR_STATE_GPR31)
        return (u8)DOLVM_HOME_GPR(slot - DOLIR_STATE_GPR0);
    if (slot == DOLIR_STATE_LR)
        return (u8)DOLVM_HOME_LR;
    if (slot == DOLIR_STATE_CTR)
        return (u8)DOLVM_HOME_CTR;
    return DOLVM_NO_REG;
}

static bool homed_slot(const FunctionEmitter* fn, u32 slot) {
    return home_register(fn, slot) != DOLVM_NO_REG;
}

// ---------------------------------------------------------------------------
// Register allocation
// ---------------------------------------------------------------------------

static u8 allocate_register(FunctionEmitter* fn) {
    if (!fn->free_count)
        return DOLVM_NO_REG;
    return fn->free_registers[--fn->free_count];
}

static void release_register(FunctionEmitter* fn, u8 reg) {
    // A home outlives every value that happens to be sitting in it, so a value
    // retiring out of one must not put it back in the pool.
    if (reg < DOLVM_USABLE_REGISTERS && fn->free_count < DOLVM_MAX_REGISTERS)
        fn->free_registers[fn->free_count++] = reg;
}

static bool assign_result(FunctionEmitter* fn, DolIRValue value,
                          DolIRType type) {
    if (!value)
        return true;
    fn->reg[value] = allocate_register(fn);
    if (fn->reg[value] == DOLVM_NO_REG)
        return false;
    if (vector_type(type)) {
        fn->reg_hi[value] = allocate_register(fn);
        if (fn->reg_hi[value] == DOLVM_NO_REG)
            return false;
    }
    return true;
}

// Copy coalescing for the paired-single lane operations.
//
// A v2f64 lives in two registers, and building, extracting or shuffling one
// used to mean allocating a fresh pair and moving the lanes into it. On a title
// that lives in paired singles those moves are the single hottest thing the
// interpreter does -- 7.5% of Star Fox Assault's run time, 6% of every opcode
// Melee executes -- and they move nothing: the value is already in a register.
//
// So when every lane the result wants is in a register that dies right here,
// the result simply *is* those registers. Four conditions make that safe, and
// all four are checked rather than assumed:
//
//   - each source lane's last use is this instruction, so nothing reads it
//     afterwards under its own name;
//   - the two lanes are in *different* registers, or the result's halves would
//     alias and writing one would change the other;
//   - neither is a home, which outlives any value sitting in it and belongs to
//     a guest register rather than to this block;
//   - the result is not itself being computed straight into a home.
//
// A source register the result does not take is released as usual. The
// operands are marked retired so the normal retirement pass leaves the
// registers alone -- they have an owner now.
static bool try_coalesce_lanes(FunctionEmitter* fn, const DolIRInstruction* inst,
                               u32 index) {
    if (!inst->result || fn->home_dest[index] != DOLVM_NO_REG ||
        fn->home_read[index] == 1u)
        return false;

    const u32 dies = index + 1u;
    // Every source has to be an ordinary block-local register that dies here.
    for (u32 o = 0; o < inst->operand_count; o++) {
        DolIRValue v = inst->operands[o];
        if (!v || fn->retired[v] || fn->last_use[v] != dies ||
            fn->reg[v] >= DOLVM_USABLE_REGISTERS)
            return false;
        if (fn->reg_hi[v] != DOLVM_NO_REG &&
            fn->reg_hi[v] >= DOLVM_USABLE_REGISTERS)
            return false;
    }

    u8 low, high = DOLVM_NO_REG, spare = DOLVM_NO_REG;
    switch (inst->op) {
    case DOLIR_OP_VECTOR_BUILD:
        if (inst->operand_count != 2 || inst->operands[0] == inst->operands[1])
            return false;
        low = fn->reg[inst->operands[0]];
        high = fn->reg[inst->operands[1]];
        break;
    case DOLIR_OP_VECTOR_EXTRACT: {
        if (inst->operand_count != 1)
            return false;
        DolIRValue src = inst->operands[0];
        low = inst->aux ? fn->reg_hi[src] : fn->reg[src];
        spare = inst->aux ? fn->reg[src] : fn->reg_hi[src];
        if (low == DOLVM_NO_REG)
            return false;
        break;
    }
    case DOLIR_OP_VECTOR_SHUFFLE: {
        u32 lanes[2] = {inst->aux & 0xFFu, (inst->aux >> 8) & 0xFFu};
        u8 pick[2];
        for (u32 lane = 0; lane < 2; lane++) {
            DolIRValue source =
                lanes[lane] < 2 ? inst->operands[0] : inst->operands[1];
            if (!source)
                return false;
            pick[lane] = (lanes[lane] & 1u) ? fn->reg_hi[source]
                                            : fn->reg[source];
            if (pick[lane] == DOLVM_NO_REG)
                return false;
        }
        if (pick[0] == pick[1])
            return false;
        low = pick[0];
        high = pick[1];
        break;
    }
    default:
        return false;
    }
    if (low == DOLVM_NO_REG || (vector_type(inst->type) && high == DOLVM_NO_REG))
        return false;

    fn->reg[inst->result] = low;
    fn->reg_hi[inst->result] = vector_type(inst->type) ? high : DOLVM_NO_REG;
    for (u32 o = 0; o < inst->operand_count; o++)
        if (inst->operands[o])
            fn->retired[inst->operands[o]] = 1;
    // A half of a pair the result did not take has no owner left.
    if (spare != DOLVM_NO_REG && spare != low)
        release_register(fn, spare);
    // A shuffle that reads two pairs and keeps one lane from each leaves the
    // other two lanes ownerless.
    if (inst->op == DOLIR_OP_VECTOR_SHUFFLE) {
        for (u32 o = 0; o < inst->operand_count; o++) {
            DolIRValue v = inst->operands[o];
            if (!v)
                continue;
            if (fn->reg[v] != low && fn->reg[v] != high)
                release_register(fn, fn->reg[v]);
            if (fn->reg_hi[v] != DOLVM_NO_REG && fn->reg_hi[v] != low &&
                fn->reg_hi[v] != high)
                release_register(fn, fn->reg_hi[v]);
        }
    }
    return true;
}

static DolIRValue effective_operand(FunctionEmitter* fn,
                                    const DolIRBlock* block,
                                    const DolIRInstruction* inst, u32 slot);
static bool folds_to_immediate(FunctionEmitter* fn, const DolIRBlock* block,
                               const DolIRInstruction* inst, u32 slot);
static bool constant_in_block(FunctionEmitter* fn, const DolIRBlock* block,
                              DolIRValue value, u64* out);

static void retire_operands(FunctionEmitter* fn, const DolIRBlock* block,
                            const DolIRInstruction* inst, u32 index) {
    for (u32 o = 0; o < inst->operand_count; o++) {
        DolIRValue value = effective_operand(fn, block, inst, o);
        if (!value || fn->last_use[value] != index + 1u)
            continue;
        if (fn->retired[value])
            continue;
        fn->retired[value] = 1;
        release_register(fn, fn->reg[value]);
        release_register(fn, fn->reg_hi[value]);
        // The mapping and the live range both stay: a value live across a
        // mid-block entry has to be nameable when the entry stub reloads it,
        // and the entry stubs are laid out after the whole block.
    }
}

// ---------------------------------------------------------------------------
// Block analysis
// ---------------------------------------------------------------------------

// The address operand of a memory operation may name an add that was folded
// into the displacement; liveness and retirement have to see through it to the
// register the memory operation will really read.
static DolIRValue effective_operand(FunctionEmitter* fn,
                                    const DolIRBlock* block,
                                    const DolIRInstruction* inst, u32 slot) {
    DolIRValue value = inst->operands[slot];
    if (!value)
        return 0;
    // A constant the opcode will carry inline is not read from a register, so
    // it is not a use -- which is usually what lets the constant die entirely.
    if (folds_to_immediate(fn, block, inst, slot))
        return 0;
    // Same for a condition-register update that reads CPUState itself, and for
    // a guest load or store that does: the state loads that used to feed them
    // stop being uses, and then stop being emitted at all.
    u32 index = (u32)(inst - block->instructions);
    if (fn->cr_form[index] == CR_FIELD_REG_I ? slot == 1u
                                             : (slot < 2u && fn->cr_form[index] != 0u))
        return 0;
    // A fused guest load or store reads neither operand from a register,
    // except the base-in-a-register variants, which still read slot 0.
    if (fn->mem_fused[index] == 1u && slot < 2u)
        return 0;
    if (fn->mem_fused[index] == 2u && slot == 1u)
        return 0;
    // A shift folded into the mask that consumed it: the mask reads what the
    // shift read, not what it wrote.
    if (fn->shift_fused[index] && slot == 0u) {
        u32 def = fn->def_index[value];
        if (def < block->instruction_count && fn->folded[def])
            return block->instructions[def].operands[0];
    }
    if (slot != 0)
        return value;
    if (inst->op != DOLIR_OP_GUEST_LOAD && inst->op != DOLIR_OP_GUEST_STORE)
        return value;
    u32 def = fn->def_index[value];
    if (def >= block->instruction_count || !fn->folded[def])
        return value;
    return block->instructions[def].operands[0];
}

// A value that came out of CPUState can only be re-read at its consumer if
// nothing in between could have changed the slot. State writes are the obvious
// hazard and a helper that writes state is the less obvious one; both are
// rejected outright rather than reasoned about.
static bool state_stable_between(const DolIRBlock* block, u32 from, u32 to) {
    for (u32 i = from + 1u; i < to; i++) {
        const DolIRInstruction* inst = &block->instructions[i];
        if (inst->effects & (DOLIR_EFFECT_WRITE_STATE | DOLIR_EFFECT_BARRIER))
            return false;
    }
    return true;
}

// The single instruction in this block that defines `value`, or NULL if it
// comes from somewhere else.
static const DolIRInstruction* single_use_def(FunctionEmitter* fn,
                                              const DolIRBlock* block,
                                              DolIRValue value, u32* index) {
    if (!value || fn->use_count[value] != 1u)
        return NULL;
    u32 def = fn->def_index[value];
    if (def >= block->instruction_count || fn->folded[def])
        return NULL;
    *index = def;
    return &block->instructions[def];
}

static bool cr_field_helper_inst(const DolIRInstruction* inst) {
    return inst->op == DOLIR_OP_HELPER_CALL &&
           inst->aux == DOLIR_HELPER_CR_FIELD;
}

// Whether the condition-register field update at `index` can take operand
// `slot` straight out of CPUState instead of out of a register. A 32-bit state
// read that feeds nothing else is exactly what `cmpw` leaves behind.
static bool cr_field_state_operand(FunctionEmitter* fn, const DolIRBlock* block,
                                   const DolIRInstruction* inst, u32 index,
                                   u32 slot, u32* offset) {
    if (!cr_field_helper_inst(inst) || slot >= 2u)
        return false;
    u32 def_index = 0;
    const DolIRInstruction* def =
        single_use_def(fn, block, inst->operands[slot], &def_index);
    if (!def || def->op != DOLIR_OP_STATE_READ ||
        dolir_state_type(def->aux) != DOLIR_TYPE_I32)
        return false;
    // A homed slot is not in CPUState to be read from. The plain register form
    // reads its home instead, and costs the same one dispatch.
    if (homed_slot(fn, def->aux))
        return false;
    if (!state_stable_between(block, def_index, index))
        return false;
    if (offset)
        *offset = state_offset(def->aux);
    return true;
}

// The value a memory operation really reads for its address, seeing through an
// address add that has already been folded into the displacement.
static DolIRValue memory_base_value(FunctionEmitter* fn, const DolIRBlock* block,
                                    DolIRValue address) {
    if (!address)
        return 0;
    u32 def = fn->def_index[address];
    if (def >= block->instruction_count || !fn->folded[def])
        return address;
    const DolIRInstruction* add = &block->instructions[def];
    return add->op == DOLIR_OP_ADD ? add->operands[0] : address;
}

// A 32-bit state read that feeds nothing but this instruction, and that nothing
// between could have invalidated. The offset it would be read from comes back
// in `offset`.
static bool private_state_read(FunctionEmitter* fn, const DolIRBlock* block,
                               DolIRValue value, u32 at, u32* offset) {
    u32 def_index = 0;
    const DolIRInstruction* def = single_use_def(fn, block, value, &def_index);
    if (!def || def->op != DOLIR_OP_STATE_READ ||
        dolir_state_type(def->aux) != DOLIR_TYPE_I32)
        return false;
    if (homed_slot(fn, def->aux))
        return false;
    if (!state_stable_between(block, def_index, at))
        return false;
    if (offset)
        *offset = state_offset(def->aux);
    return true;
}

// How a condition-register field update will read its two operands. The fused
// forms exist because a guest compare is nearly always "a register against a
// small constant" or "a register against a register", and going through VM
// registers costs a dispatch per operand for no benefit -- the interpreter is
// reading CPUState either way. There is no mixed form, so the choice is made
// for the instruction as a whole rather than per operand.
// Decided once, from analyze_block, while the use counts still describe the IR
// rather than what folding has already removed from it. Choosing a fused form
// is what drops those uses to zero, so asking again later would always answer
// CR_FIELD_REGS -- and then read registers that were never allocated.
static CRFieldForm cr_field_form(FunctionEmitter* fn, const DolIRBlock* block,
                                 const DolIRInstruction* inst, u32 index) {
    if (!cr_field_helper_inst(inst))
        return CR_FIELD_REGS;
    u64 constant = 0;
    bool immediate = constant_in_block(fn, block, inst->operands[1], &constant) &&
                     constant <= 0xFFFFFFFFull;
    if (!cr_field_state_operand(fn, block, inst, index, 0, NULL))
        return immediate ? CR_FIELD_REG_I : CR_FIELD_REGS;
    if (immediate)
        return CR_FIELD_STATE_I;
    if (!cr_field_state_operand(fn, block, inst, index, 1, NULL))
        return CR_FIELD_REGS;
    return CR_FIELD_STATE;
}

// The operand detail behind a decision already made. No use counts are
// consulted, so this answers the same way before and after folding.
static void cr_field_operands(FunctionEmitter* fn, const DolIRBlock* block,
                              const DolIRInstruction* inst, CRFieldForm form,
                              u32* left, u32* right, u64* immediate) {
    *left = 0;
    *right = 0;
    *immediate = 0;
    if (form == CR_FIELD_REGS)
        return;
    if (form == CR_FIELD_REG_I) {
        constant_in_block(fn, block, inst->operands[1], immediate);
        return;
    }
    *left = state_offset(block->instructions[fn->def_index[inst->operands[0]]].aux);
    if (constant_in_block(fn, block, inst->operands[1], immediate))
        return;
    *right = state_offset(block->instructions[fn->def_index[inst->operands[1]]].aux);
}

// `lwz rD,off(rA)` is a state read, a guest load and a state write, and it is
// the single most common thing a game does. Fused, it is one dispatch: the base
// comes out of CPUState, the loaded value goes back into it, and neither the
// read nor the write is emitted. The write has to be the very next thing
// emitted and part of the same guest instruction, so that folding it forward
// cannot move an architectural update across anything that could exit.
// The destination half of the load fusion on its own: the value goes straight
// into its slot, but the address base stays in a register. This is the case
// that actually fires on real code, because state forwarding hands one block's
// stack-pointer read to every access in the block, so the read is never private
// to a single load.
static bool guest_load_write_fusible(FunctionEmitter* fn, const DolIRBlock* block,
                                     u32 index, u32* write_index) {
    const DolIRInstruction* inst = &block->instructions[index];
    u32 width = inst->aux & 0xFFu;
    bool sign = (inst->aux & 0x100u) != 0;
    if (width != 1u && width != 2u && !(width == 4u && !sign))
        return false;
    if (!inst->result)
        return false;
    u32 next = index + 1u;
    while (next < block->instruction_count && fn->folded[next])
        next++;
    if (next >= block->instruction_count)
        return false;
    const DolIRInstruction* write = &block->instructions[next];
    if (write->op != DOLIR_OP_STATE_WRITE || write->operands[0] != inst->result ||
        write->guest_pc != inst->guest_pc ||
        dolir_state_type(write->aux) != DOLIR_TYPE_I32 ||
        homed_slot(fn, write->aux))
        return false;
    *write_index = next;
    return true;
}

static bool guest_load_state_fusible(FunctionEmitter* fn, const DolIRBlock* block,
                                     u32 index, u32* write_index,
                                     u32* base_offset) {
    const DolIRInstruction* inst = &block->instructions[index];
    u32 width = inst->aux & 0xFFu;
    bool sign = (inst->aux & 0x100u) != 0;
    // A 32-bit signed or a 64-bit load produces more than the 32 bits a GPR
    // slot holds, so those keep the general path.
    if (width != 1u && width != 2u && !(width == 4u && !sign))
        return false;
    if (!private_state_read(fn, block, memory_base_value(fn, block, inst->operands[0]),
                            index, base_offset))
        return false;
    if (!inst->result)
        return false;
    u32 next = index + 1u;
    while (next < block->instruction_count && fn->folded[next])
        next++;
    if (next >= block->instruction_count)
        return false;
    const DolIRInstruction* write = &block->instructions[next];
    if (write->op != DOLIR_OP_STATE_WRITE || write->operands[0] != inst->result ||
        write->guest_pc != inst->guest_pc ||
        dolir_state_type(write->aux) != DOLIR_TYPE_I32 ||
        homed_slot(fn, write->aux))
        return false;
    *write_index = next;
    return true;
}

// The mirror image for `stw rS,off(rA)`: both the address base and the value
// stored are slots nothing else reads.
static bool guest_store_state_fusible(FunctionEmitter* fn, const DolIRBlock* block,
                                      u32 index, u32* base_offset,
                                      u32* value_offset) {
    const DolIRInstruction* inst = &block->instructions[index];
    u32 width = inst->aux & 0xFFu;
    if (width != 1u && width != 2u && width != 4u)
        return false;
    return private_state_read(fn, block,
                              memory_base_value(fn, block, inst->operands[0]),
                              index, base_offset) &&
           private_state_read(fn, block, inst->operands[1], index, value_offset);
}

// Every privileged instruction opens with a check that the guest is in
// supervisor mode, which the builder writes out as an MSR read, a mask, a
// compare against zero and a conditional raise. That is four dispatches on
// `mfmsr`, and a game's operating system runs `mfmsr`/`mtmsr` around every
// interrupt mask it takes -- on the SpongeBob movie the four together came to a
// tenth of everything the interpreter executed. One opcode does the same test.
static bool supervisor_fusible(FunctionEmitter* fn, const DolIRBlock* block,
                               u32 index, u32* compare_out, u32* mask_out,
                               u32* read_out) {
    const DolIRInstruction* inst = &block->instructions[index];
    if (inst->op != DOLIR_OP_HELPER_CALL ||
        inst->aux != DOLIR_HELPER_PROGRAM_EXCEPTION ||
        inst->immediate != (u64)DOLIR_PROGRAM_PRIV || inst->operand_count != 1)
        return false;
    u32 compare_index = 0;
    const DolIRInstruction* compare =
        single_use_def(fn, block, inst->operands[0], &compare_index);
    u64 zero = 1;
    if (!compare || compare->op != DOLIR_OP_ICMP_NE ||
        compare->operand_count != 2 ||
        !constant_in_block(fn, block, compare->operands[1], &zero) || zero != 0)
        return false;
    u32 mask_index = 0;
    const DolIRInstruction* mask =
        single_use_def(fn, block, compare->operands[0], &mask_index);
    u64 bit = 0;
    if (!mask || mask->op != DOLIR_OP_AND || mask->operand_count != 2 ||
        !constant_in_block(fn, block, mask->operands[1], &bit) || bit != 0x4000u)
        return false;
    u32 read_index = 0;
    const DolIRInstruction* read =
        single_use_def(fn, block, mask->operands[0], &read_index);
    if (!read || read->op != DOLIR_OP_STATE_READ || read->aux != DOLIR_STATE_MSR)
        return false;
    // The fused form reads MSR where the check is rather than where the read
    // was, so nothing in between may have been able to change it.
    if (!state_stable_between(block, read_index, index))
        return false;
    *compare_out = compare_index;
    *mask_out = mask_index;
    *read_out = read_index;
    return true;
}

static bool address_add_foldable(FunctionEmitter* fn, const DolIRBlock* block,
                                 u32 index) {
    const DolIRInstruction* inst = &block->instructions[index];
    if (inst->op != DOLIR_OP_ADD || inst->type != DOLIR_TYPE_I32 ||
        inst->operand_count != 2 || !inst->result)
        return false;
    if (!fn->use_count[inst->result] ||
        fn->use_count[inst->result] != fn->memory_use_count[inst->result])
        return false;
    DolIRValue rhs = inst->operands[1];
    u32 rhs_def = rhs ? fn->def_index[rhs] : block->instruction_count;
    if (rhs_def >= block->instruction_count)
        return false;
    return block->instructions[rhs_def].op == DOLIR_OP_CONSTANT;
}

// Computing a value straight into its home moves the architectural update from
// where the IR wrote it to where the value was produced. Nothing can tell the
// difference unless something in between could have let the outside see
// CPUState -- an exit, a raise, a helper, or a guest memory access, since any of
// those flushes the homes back.
static bool homes_private_between(const DolIRBlock* block, u32 from, u32 to) {
    for (u32 i = from + 1u; i < to; i++) {
        const DolIRInstruction* inst = &block->instructions[i];
        if (inst->op == DOLIR_OP_GUEST_LOAD || inst->op == DOLIR_OP_GUEST_STORE ||
            inst->op == DOLIR_OP_HELPER_CALL)
            return false;
        if (inst->effects & (DOLIR_EFFECT_MAY_EXIT | DOLIR_EFFECT_MAY_RAISE |
                             DOLIR_EFFECT_BARRIER))
            return false;
    }
    return true;
}

// May the state write move onto the instruction that computed the value?
//
// Not if that instruction can fail. A helper that raises has already written
// its destination register by the time it does, and the IR puts the state write
// after the raise check precisely so that a faulted guest instruction leaves
// the register alone -- `eciwx` into a disabled EAR is the case the differential
// test catches. A guest load is the exception, and the one that matters: its
// handler leaves the destination untouched unless the access succeeded.
static bool home_dest_safe(const DolIRInstruction* inst) {
    if (inst->op == DOLIR_OP_GUEST_LOAD)
        return true;
    return (inst->effects & (DOLIR_EFFECT_MAY_RAISE | DOLIR_EFFECT_MAY_EXIT |
                             DOLIR_EFFECT_BARRIER)) == 0;
}

// Where in the block the home for `slot` is next overwritten, or past the end
// when it is not. A value taken out of a home stays valid exactly that long.
static u32 next_home_write(const FunctionEmitter* fn, const DolIRBlock* block,
                           u32 slot, u32 after) {
    for (u32 i = after + 1u; i < block->instruction_count; i++) {
        if (fn->folded[i])
            continue;
        const DolIRInstruction* inst = &block->instructions[i];
        if (inst->op == DOLIR_OP_STATE_WRITE && inst->aux == slot)
            return i;
    }
    return block->instruction_count + 1u;
}

// Decide, for every state access this block makes to a homed slot, whether it
// needs any code at all.
//
// A read normally needs none: the value it produces *is* the home register. The
// exception is a value that outlives the next write to the same slot, which has
// to be copied out first -- one dispatch, the same as the load it replaces.
//
// A write normally needs none either, because the instruction that computed the
// value can be told to compute it into the home directly. That is only sound
// while the old contents are still wanted by nothing and nothing in between
// could have flushed, so both are checked; when either fails the write becomes
// a copy, which again costs what the store did.
static void analyze_homes(FunctionEmitter* fn, const DolIRBlock* block) {
    u32 count = block->instruction_count;
    for (u32 i = 0; i < count; i++) {
        fn->home_read[i] = 0;
        fn->home_write[i] = 0;
        fn->home_dest[i] = DOLVM_NO_REG;
    }
    // No early exit for a module that homes nothing else: XER is homed
    // whatever it does, and home_register is what decides per slot.
    // How far past its read each home's current contents are still wanted.
    u32 wanted_until[DOLVM_HOME_COUNT];
    for (u32 h = 0; h < DOLVM_HOME_COUNT; h++)
        wanted_until[h] = 0;
    for (u32 i = 0; i < count; i++) {
        if (fn->folded[i])
            continue;
        const DolIRInstruction* inst = &block->instructions[i];
        u8 home = (inst->op == DOLIR_OP_STATE_READ ||
                   inst->op == DOLIR_OP_STATE_WRITE)
                      ? home_register(fn, inst->aux)
                      : DOLVM_NO_REG;
        if (home == DOLVM_NO_REG)
            continue;
        u32 index = home - DOLVM_HOME_BASE;
        if (inst->op == DOLIR_OP_STATE_READ) {
            if (!inst->result)
                continue;
            u32 last = fn->last_use[inst->result];
            if (last <= next_home_write(fn, block, inst->aux, i) + 1u) {
                fn->home_read[i] = 1;
                if (last > wanted_until[index])
                    wanted_until[index] = last;
            } else {
                fn->home_read[i] = 2;
            }
            continue;
        }
        DolIRValue value = inst->operands[0];
        u32 def = value ? fn->def_index[value] : 0xFFFFFFFFu;
        bool direct =
            def < count && !fn->folded[def] && fn->home_read[def] == 0 &&
            block->instructions[def].result == value &&
            block->instructions[def].op != DOLIR_OP_STATE_READ &&
            home_dest_safe(&block->instructions[def]) &&
            block->instructions[def].type == dolir_state_type(inst->aux) &&
            block->instructions[def].guest_pc == inst->guest_pc &&
            fn->home_dest[def] == DOLVM_NO_REG &&
            wanted_until[index] <= def + 1u &&
            fn->last_use[value] <=
                next_home_write(fn, block, inst->aux, i) + 1u &&
            homes_private_between(block, def, i);
        if (direct)
            fn->home_dest[def] = home;
        fn->home_write[i] = direct ? 1u : 2u;
        wanted_until[index] = 0;
    }
}

// Whether a block is an idle loop, by the rule Dolphin's JITs use
// (PPCAnalyst::IsBusyWaitLoop): it branches back to its own head, it writes no
// memory, it is integer and load work only, and it carries no state from one
// iteration to the next -- every register it reads is one it either never
// writes or wrote earlier in the same iteration. Each pass is then the same
// pure function of memory, nothing the loop does can change memory, and only
// an interrupt handler or the other processor can end it. So the time spent
// there can be skipped: the interpreter charges the rest of the slice and
// returns, the chassis runs whatever event was due, and the loop re-polls.
// `lwz rX,d(rY); cmp[l]wi rX,n; bc` -- GXDrawDone's wait for the GPU -- is the
// common shape. A counted loop reads a register before it writes it and is
// left to run, as is anything with a store.
static bool idle_loop_block(const DolIRBlock* block, u32 block_index) {
    const DolIRTerminator* term = &block->terminator;
    if (term->kind != DOLIR_TERM_COND_BRANCH && term->kind != DOLIR_TERM_BRANCH)
        return false;
    if (term->targets[0] != block_index)
        return false;
    u8 written[DOLIR_STATE_COUNT];
    u8 read_first[DOLIR_STATE_COUNT];
    memset(written, 0, sizeof(written));
    memset(read_first, 0, sizeof(read_first));
    u32 loads = 0;
    for (u32 i = 0; i < block->instruction_count; i++) {
        const DolIRInstruction* inst = &block->instructions[i];
        switch (inst->op) {
        case DOLIR_OP_CONSTANT:
        case DOLIR_OP_ADD:
        case DOLIR_OP_SUB:
        case DOLIR_OP_MUL:
        case DOLIR_OP_AND:
        case DOLIR_OP_OR:
        case DOLIR_OP_XOR:
        case DOLIR_OP_NOT:
        case DOLIR_OP_SHL:
        case DOLIR_OP_LSHR:
        case DOLIR_OP_ASHR:
        case DOLIR_OP_ROTL:
        case DOLIR_OP_CLZ:
        case DOLIR_OP_BSWAP:
        case DOLIR_OP_TRUNC:
        case DOLIR_OP_ZEXT:
        case DOLIR_OP_SEXT:
        case DOLIR_OP_BITCAST:
        case DOLIR_OP_ICMP_EQ:
        case DOLIR_OP_ICMP_NE:
        case DOLIR_OP_ICMP_ULT:
        case DOLIR_OP_ICMP_ULE:
        case DOLIR_OP_ICMP_SLT:
        case DOLIR_OP_ICMP_SLE:
        case DOLIR_OP_SELECT:
            break;
        case DOLIR_OP_GUEST_LOAD:
            loads++;
            break;
        case DOLIR_OP_STATE_READ:
            if (inst->aux >= DOLIR_STATE_COUNT)
                return false;
            if (!written[inst->aux])
                read_first[inst->aux] = 1;
            break;
        case DOLIR_OP_STATE_WRITE:
            if (inst->aux >= DOLIR_STATE_COUNT || read_first[inst->aux])
                return false;
            // The loop may only leave behind what a compare and a load leave
            // behind; anything else is a side effect the wait is producing.
            if (!(inst->aux <= DOLIR_STATE_GPR31 || inst->aux == DOLIR_STATE_CR ||
                  inst->aux == DOLIR_STATE_XER))
                return false;
            written[inst->aux] = 1;
            break;
        case DOLIR_OP_HELPER_CALL:
            // The condition-register field update reads two values and writes
            // the field; the only helper a compare becomes.
            if (!cr_field_helper_inst(inst))
                return false;
            written[DOLIR_STATE_CR] = 1;
            break;
        default:
            return false;
        }
        if (inst->effects & (DOLIR_EFFECT_WRITE_MEMORY | DOLIR_EFFECT_MAY_RAISE |
                             DOLIR_EFFECT_BARRIER))
            return false;
    }
    // `b .` waits for an interrupt with nothing to read. An unconditional loop
    // around a load is left to run: the load may be of a hardware register
    // whose reading is the point, and Dolphin's rule would skip it, but it is
    // rare enough that the cautious reading costs nothing.
    if (term->kind == DOLIR_TERM_BRANCH)
        return block->instruction_count == 0 || loads == 0;
    return true;
}

static void analyze_block(FunctionEmitter* fn, const DolIRBlock* block) {
    u32 count = block->instruction_count;
    for (u32 i = 0; i < count; i++) {
        DolIRValue result = block->instructions[i].result;
        if (result) {
            fn->last_use[result] = 0;
            fn->use_count[result] = 0;
            fn->memory_use_count[result] = 0;
            fn->def_index[result] = i;
            fn->reg[result] = DOLVM_NO_REG;
            fn->reg_hi[result] = DOLVM_NO_REG;
            fn->retired[result] = 0;
        }
        fn->folded[i] = 0;
        fn->cr_form[i] = CR_FIELD_REGS;
        fn->mem_fused[i] = 0;
        fn->supervisor[i] = 0;
        fn->shift_fused[i] = 0;
        fn->shift_amount[i] = 0;
    }

    // A first, literal count decides which address adds can fold.
    for (u32 i = 0; i < count; i++) {
        const DolIRInstruction* inst = &block->instructions[i];
        for (u32 o = 0; o < inst->operand_count; o++) {
            DolIRValue value = inst->operands[o];
            if (!value)
                continue;
            fn->use_count[value]++;
            if (o == 0 && (inst->op == DOLIR_OP_GUEST_LOAD ||
                           inst->op == DOLIR_OP_GUEST_STORE))
                fn->memory_use_count[value]++;
        }
    }
    if (block->terminator.condition)
        fn->use_count[block->terminator.condition]++;
    if (block->terminator.target_value)
        fn->use_count[block->terminator.target_value]++;
    for (u32 i = 0; i < count; i++)
        fn->folded[i] = address_add_foldable(fn, block, i) ? 1u : 0u;
    for (u32 i = 0; i < count; i++) {
        if (fn->folded[i])
            continue;
        fn->cr_form[i] = (u8)cr_field_form(fn, block, &block->instructions[i], i);
    }
    for (u32 i = 0; i < count; i++) {
        const DolIRInstruction* inst = &block->instructions[i];
        if (fn->folded[i])
            continue;
        u32 write_index = 0;
        u32 base_offset = 0;
        u32 value_offset = 0;
        if (inst->op == DOLIR_OP_GUEST_LOAD &&
            guest_load_state_fusible(fn, block, i, &write_index, &base_offset)) {
            fn->mem_fused[i] = 1;
            fn->mem_write[i] = write_index;
            fn->mem_base_off[i] = base_offset;
            fn->folded[write_index] = 1;
        } else if (inst->op == DOLIR_OP_GUEST_LOAD &&
                   guest_load_write_fusible(fn, block, i, &write_index)) {
            fn->mem_fused[i] = 2;
            fn->mem_write[i] = write_index;
            fn->folded[write_index] = 1;
        } else if (inst->op == DOLIR_OP_GUEST_STORE &&
                   guest_store_state_fusible(fn, block, i, &base_offset,
                                             &value_offset)) {
            fn->mem_fused[i] = 1;
            fn->mem_base_off[i] = base_offset;
            fn->mem_value_off[i] = value_offset;
        } else if (inst->op == DOLIR_OP_GUEST_STORE &&
                   (inst->aux & 0xFFu) != 8u &&
                   private_state_read(fn, block, inst->operands[1], i,
                                      &value_offset)) {
            fn->mem_fused[i] = 2;
            fn->mem_value_off[i] = value_offset;
        }
    }

    for (u32 i = 0; i < count; i++) {
        if (fn->folded[i])
            continue;
        u32 compare_index = 0, mask_index = 0, read_index = 0;
        if (!supervisor_fusible(fn, block, i, &compare_index, &mask_index,
                                &read_index))
            continue;
        fn->supervisor[i] = 1;
        fn->folded[compare_index] = 1;
        fn->folded[mask_index] = 1;
        fn->folded[read_index] = 1;
    }

    // `bne` and friends arrive as "not (bit set)". Inverting the branch costs
    // nothing and removes the negation, which is one dispatch on the hottest
    // path in any loop.
    fn->terminator_condition = block->terminator.condition;
    fn->invert_condition = false;
    if (fn->terminator_condition) {
        u32 def = fn->def_index[fn->terminator_condition];
        if (def < count && !fn->folded[def] &&
            block->instructions[def].op == DOLIR_OP_NOT &&
            block->instructions[def].type == DOLIR_TYPE_I1 &&
            fn->use_count[fn->terminator_condition] == 1u) {
            fn->folded[def] = 1;
            fn->terminator_condition = block->instructions[def].operands[0];
            fn->invert_condition = true;
        }
    }

    // `blr` and `bctr` arrive as a conditional indirect whose BO field says
    // "always", so the condition folds to the constant 1 and the branch on it is
    // never taken either way. Materializing that constant and testing it were
    // two dispatches on every function return in the program.
    fn->constant_branch = false;
    fn->constant_branch_taken = false;
    if (fn->terminator_condition &&
        (block->terminator.kind == DOLIR_TERM_COND_BRANCH ||
         block->terminator.kind == DOLIR_TERM_INDIRECT)) {
        u64 value = 0;
        if (constant_in_block(fn, block, fn->terminator_condition, &value)) {
            fn->constant_branch = true;
            fn->constant_branch_taken =
                fn->invert_condition ? value == 0 : value != 0;
            fn->terminator_condition = 0;
        }
    }

    // `bc` on a condition-register bit is four instructions in the IR -- read
    // CR, mask one bit, compare it against zero, branch -- and one opcode here.
    // The chain has to be private to the branch and CR has to be untouched
    // between the read and the end of the block, since the fused branch reads
    // CR where the branch is rather than where the read was.
    fn->cr_branch = false;
    if (fn->terminator_condition &&
        (block->terminator.kind == DOLIR_TERM_COND_BRANCH ||
         block->terminator.kind == DOLIR_TERM_INDIRECT)) {
        u32 compare_index = 0;
        const DolIRInstruction* compare =
            single_use_def(fn, block, fn->terminator_condition, &compare_index);
        u64 zero = 1;
        if (compare && compare->op == DOLIR_OP_ICMP_NE &&
            compare->operand_count == 2 &&
            constant_in_block(fn, block, compare->operands[1], &zero) &&
            zero == 0) {
            u32 mask_index = 0;
            const DolIRInstruction* mask_inst =
                single_use_def(fn, block, compare->operands[0], &mask_index);
            u64 mask = 0;
            if (mask_inst && mask_inst->op == DOLIR_OP_AND &&
                mask_inst->operand_count == 2 &&
                constant_in_block(fn, block, mask_inst->operands[1], &mask) &&
                mask != 0 && (mask & (mask - 1u)) == 0 && mask <= 0x80000000ull) {
                u32 read_index = 0;
                const DolIRInstruction* read =
                    single_use_def(fn, block, mask_inst->operands[0], &read_index);
                if (read && read->op == DOLIR_OP_STATE_READ &&
                    read->aux == DOLIR_STATE_CR &&
                    state_stable_between(block, read_index, count)) {
                    u32 bit = 0;
                    while (((mask << bit) & 0x80000000ull) == 0)
                        bit++;
                    fn->folded[compare_index] = 1;
                    fn->folded[mask_index] = 1;
                    fn->folded[read_index] = 1;
                    fn->cr_branch = true;
                    fn->cr_branch_bit = (u8)bit;
                    // COND_BRANCH takes its edge when the condition holds;
                    // INDIRECT is emitted with the opposite polarity, and the
                    // folded negation flips either one again.
                    bool taken_when_set =
                        block->terminator.kind == DOLIR_TERM_COND_BRANCH
                            ? !fn->invert_condition
                            : fn->invert_condition;
                    fn->cr_branch_sense = taken_when_set ? 1u : 0u;
                    fn->terminator_condition = 0;
                }
            }
        }
    }

    // A shift or rotate by a constant whose only reader is the mask that
    // follows it. That pair is `rlwinm`, and `rlwinm` is how PowerPC extracts
    // every bitfield there is: on this title the two opcodes were 9% of
    // everything executed, with the second waiting on the first through the
    // register file. Run last, so an AND some earlier fold has already claimed
    // -- the condition-register branch's bit mask, the privilege test's, the
    // indirect branch's alignment mask -- is left alone.
    for (u32 i = 0; i < count; i++) {
        if (fn->folded[i] || fn->cr_form[i] != CR_FIELD_REGS)
            continue;
        const DolIRInstruction* inst = &block->instructions[i];
        if (inst->op != DOLIR_OP_AND || inst->operand_count != 2 ||
            !inst->result)
            continue;
        u64 mask = 0;
        if (!constant_in_block(fn, block, inst->operands[1], &mask) ||
            mask > 0xFFFFFFFFull)
            continue;
        u32 shift_index = 0;
        const DolIRInstruction* shift =
            single_use_def(fn, block, inst->operands[0], &shift_index);
        if (!shift || shift->type != DOLIR_TYPE_I32 ||
            shift->operand_count != 2 || !shift->result)
            continue;
        u64 amount = 0;
        if (!constant_in_block(fn, block, shift->operands[1], &amount) ||
            amount > 31ull)
            continue;
        u8 fused;
        switch (shift->op) {
        case DOLIR_OP_ROTL: fused = DOLVM_OP_ROTL32I_AND; break;
        case DOLIR_OP_SHL: fused = DOLVM_OP_SHL32I_AND; break;
        case DOLIR_OP_LSHR: fused = DOLVM_OP_LSHR32I_AND; break;
        case DOLIR_OP_ASHR: fused = DOLVM_OP_ASHR32I_AND; break;
        default: continue;
        }
        fn->shift_fused[i] = fused;
        fn->shift_amount[i] = (u8)amount;
        fn->folded[shift_index] = 1;
    }

    // Recount against what will actually be emitted, then drop whatever the
    // folding just orphaned -- typically the constant the add was carrying.
    for (u32 i = 0; i < count; i++) {
        DolIRValue result = block->instructions[i].result;
        if (result)
            fn->use_count[result] = 0;
    }
    for (u32 i = 0; i < count; i++) {
        if (fn->folded[i])
            continue;
        const DolIRInstruction* inst = &block->instructions[i];
        for (u32 o = 0; o < inst->operand_count; o++) {
            DolIRValue value = effective_operand(fn, block, inst, o);
            if (value)
                fn->use_count[value]++;
        }
    }
    if (fn->terminator_condition)
        fn->use_count[fn->terminator_condition]++;
    if (block->terminator.target_value)
        fn->use_count[block->terminator.target_value]++;
    for (u32 i = count; i-- > 0;) {
        const DolIRInstruction* inst = &block->instructions[i];
        // A state read that nothing uses any more is dead in the ordinary sense
        // -- reading CPUState has no effect -- but it is excluded from
        // pure_emit_op, so say so here. The fused compares and branches above
        // are what usually orphan one.
        bool droppable = pure_emit_op(inst->op) || inst->op == DOLIR_OP_STATE_READ;
        if (fn->folded[i] || !inst->result || !droppable ||
            fn->use_count[inst->result])
            continue;
        fn->folded[i] = 1;
        for (u32 o = 0; o < inst->operand_count; o++) {
            DolIRValue value = effective_operand(fn, block, inst, o);
            if (value && fn->use_count[value])
                fn->use_count[value]--;
        }
    }

    // `cmpwi rX,n; bne` is the shape of every counted loop and every search
    // loop there is, and the two halves are adjacent: the compare writes a
    // condition-register field and the branch immediately reads it back out of
    // CPUState. Folding them together takes a dispatch off the loop's carried
    // dependency and the store-to-load round trip with it. The compare has to
    // be the last thing the block does, and it has to be writing the very field
    // the branch tests.
    fn->cr_compare = false;
    fn->cr_compare_left = 0;
    if (fn->cr_branch && block->terminator.kind == DOLIR_TERM_COND_BRANCH &&
        block->terminator.targets[0] != DOLIR_NO_BLOCK) {
        // Scan back past anything that cannot have touched the condition
        // register. Folding the update onto the terminator moves it after
        // whatever is left between them, and a pure value definition -- usually
        // the constant the branch's own comparison was carrying, which local
        // value numbering kept alive for a later use -- cannot tell.
        u32 index = count;
        while (index > 0) {
            index--;
            if (fn->folded[index] || pure_emit_op(block->instructions[index].op))
                continue;
            break;
        }
        if (index < count && !fn->folded[index] &&
            !pure_emit_op(block->instructions[index].op)) {
            const DolIRInstruction* update = &block->instructions[index];
            u64 immediate = 0;
            if (cr_field_helper_inst(update) &&
                fn->cr_form[index] == CR_FIELD_REG_I &&
                (u32)(update->immediate & 0xFFu) ==
                    (u32)(fn->cr_branch_bit / 4u) &&
                constant_in_block(fn, block, update->operands[1], &immediate) &&
                immediate <= 0xFFFFFFFFull) {
                fn->cr_compare = true;
                fn->cr_compare_left = update->operands[0];
                fn->cr_compare_right = immediate;
                fn->cr_compare_pack =
                    (u8)((update->immediate & 7u) |
                         (((update->immediate >> 8) & 1u) << 3) |
                         ((u32)(fn->cr_branch_bit & 3u) << 4) |
                         ((u32)fn->cr_branch_sense << 6));
                fn->folded[index] = 1;
            }
        }
    }

    // `blr` and `bctr` land at LR or CTR with the low two bits cleared, which
    // the builder writes out as a mask of its own. The branch has to reject an
    // unaligned target anyway, so it does the masking itself and the `andi`
    // stops being emitted -- one dispatch off every function return.
    fn->indirect_mask = false;
    fn->indirect_target = block->terminator.target_value;
    if (block->terminator.kind == DOLIR_TERM_INDIRECT &&
        block->terminator.target_value) {
        u32 def_index = 0;
        const DolIRInstruction* def =
            single_use_def(fn, block, block->terminator.target_value, &def_index);
        u64 mask = 0;
        if (def && def->op == DOLIR_OP_AND && def->operand_count == 2 &&
            !fn->shift_fused[def_index] &&
            constant_in_block(fn, block, def->operands[1], &mask) &&
            mask == 0xFFFFFFFCull) {
            fn->indirect_mask = true;
            fn->indirect_target = def->operands[0];
            fn->folded[def_index] = 1;
        }
    }

    for (u32 i = 0; i < count; i++) {
        DolIRValue result = block->instructions[i].result;
        if (result)
            fn->last_use[result] = 0;
    }
    for (u32 i = 0; i < count; i++) {
        if (fn->folded[i])
            continue;
        const DolIRInstruction* inst = &block->instructions[i];
        for (u32 o = 0; o < inst->operand_count; o++) {
            DolIRValue value = effective_operand(fn, block, inst, o);
            if (value && fn->last_use[value] < i + 1u)
                fn->last_use[value] = i + 1u;
        }
    }
    if (fn->terminator_condition)
        fn->last_use[fn->terminator_condition] = count + 1u;
    if (fn->cr_compare_left)
        fn->last_use[fn->cr_compare_left] = count + 1u;
    if (fn->indirect_target)
        fn->last_use[fn->indirect_target] = count + 1u;

    analyze_homes(fn, block);
}

// Resolve the base register and displacement a memory operation should use,
// following the address add into the immediate when it was folded away.
static void memory_address(FunctionEmitter* fn, const DolIRBlock* block,
                           DolIRValue address, u8* base, u32* displacement) {
    *displacement = 0;
    *base = address ? fn->reg[address] : 0u;
    if (!address)
        return;
    u32 def = fn->def_index[address];
    if (def >= block->instruction_count || !fn->folded[def])
        return;
    const DolIRInstruction* add = &block->instructions[def];
    if (add->op != DOLIR_OP_ADD)
        return;
    const DolIRInstruction* constant =
        &block->instructions[fn->def_index[add->operands[1]]];
    *base = fn->reg[add->operands[0]];
    *displacement = (u32)constant->immediate;
}

// ---------------------------------------------------------------------------
// Instruction lowering
// ---------------------------------------------------------------------------

typedef struct {
    u8 reg_form;
    u8 imm_form;
} OpPair;

// Which VM opcode takes operand 1 as an immediate, if any. Liveness and
// lowering both go through this: if they disagreed, either a constant would be
// emitted that nothing reads, or a register would be read that nothing wrote.
static u8 immediate_form(FunctionEmitter* fn, const DolIRInstruction* inst) {
    bool wide = inst->type == DOLIR_TYPE_I64;
    switch (inst->op) {
    case DOLIR_OP_ADD: return wide ? DOLVM_OP_NOP : DOLVM_OP_ADD32I;
    case DOLIR_OP_MUL: return wide ? DOLVM_OP_NOP : DOLVM_OP_MUL32I;
    case DOLIR_OP_AND: return DOLVM_OP_ANDI;
    case DOLIR_OP_OR: return DOLVM_OP_ORI;
    case DOLIR_OP_XOR: return DOLVM_OP_XORI;
    case DOLIR_OP_SHL: return wide ? DOLVM_OP_SHL64I : DOLVM_OP_SHL32I;
    case DOLIR_OP_LSHR: return wide ? DOLVM_OP_LSHR64I : DOLVM_OP_LSHR32I;
    case DOLIR_OP_ASHR: return wide ? DOLVM_OP_ASHR64I : DOLVM_OP_ASHR32I;
    case DOLIR_OP_ROTL: return DOLVM_OP_ROTL32I;
    case DOLIR_OP_ICMP_EQ: return DOLVM_OP_ICMP_EQI;
    case DOLIR_OP_ICMP_NE: return DOLVM_OP_ICMP_NEI;
    case DOLIR_OP_ICMP_ULT: return DOLVM_OP_ICMP_ULTI;
    case DOLIR_OP_ICMP_ULE: return DOLVM_OP_ICMP_ULEI;
    case DOLIR_OP_ICMP_SLT:
    case DOLIR_OP_ICMP_SLE: {
        bool operand_wide = (DolIRType)inst->aux == DOLIR_TYPE_I64;
        if (operand_wide)
            return DOLVM_OP_NOP;
        return inst->op == DOLIR_OP_ICMP_SLT ? DOLVM_OP_ICMP_SLT32I
                                             : DOLVM_OP_ICMP_SLE32I;
    }
    default: return DOLVM_OP_NOP;
    }
}

static bool constant_in_block(FunctionEmitter* fn, const DolIRBlock* block,
                              DolIRValue value, u64* out) {
    if (!value)
        return false;
    u32 def = fn->def_index[value];
    if (def >= block->instruction_count)
        return false;
    const DolIRInstruction* inst = &block->instructions[def];
    if (inst->op != DOLIR_OP_CONSTANT)
        return false;
    *out = inst->immediate;
    return true;
}

static bool folds_to_immediate(FunctionEmitter* fn, const DolIRBlock* block,
                               const DolIRInstruction* inst, u32 slot) {
    u64 immediate = 0;
    return slot == 1u && inst->operand_count == 2 &&
           immediate_form(fn, inst) != DOLVM_OP_NOP &&
           constant_in_block(fn, block, inst->operands[1], &immediate) &&
           immediate <= 0xFFFFFFFFull;
}

static bool lower_binary(FunctionEmitter* fn, const DolIRBlock* block,
                         const DolIRInstruction* inst, OpPair ops) {
    Builder* builder = fn->builder;
    u8 dst = fn->reg[inst->result];
    u8 lhs = fn->reg[inst->operands[0]];
    u64 immediate = 0;
    if (folds_to_immediate(fn, block, inst, 1u)) {
        constant_in_block(fn, block, inst->operands[1], &immediate);
        return emit_raw(builder, ops.imm_form, dst, lhs, 0, (u32)immediate);
    }
    return emit_raw(builder, ops.reg_form, dst, lhs, fn->reg[inst->operands[1]],
                    0);
}

static bool lower_vector_unary(FunctionEmitter* fn,
                               const DolIRInstruction* inst, u8 op) {
    Builder* builder = fn->builder;
    DolIRValue source = inst->operands[0];
    return emit_raw(builder, op, fn->reg[inst->result], fn->reg[source], 0, 0) &&
           emit_raw(builder, op, fn->reg_hi[inst->result], fn->reg_hi[source], 0,
                    0);
}

static bool lower_instruction(FunctionEmitter* fn, const DolIRBlock* block,
                              const DolIRInstruction* inst, u32 index) {
    Builder* builder = fn->builder;
    u8 dst = inst->result ? fn->reg[inst->result] : 0;
    u32 pc_word = (inst->guest_pc - fn->pc_base) / 4u;

    switch (inst->op) {
    case DOLIR_OP_CONSTANT:
        if (inst->immediate <= 0xFFFFFFFFull)
            return emit_raw(builder, DOLVM_OP_CONST32, dst, 0, 0,
                            (u32)inst->immediate);
        else {
            bool ok = false;
            u32 pool = intern_constant(builder, inst->immediate, &ok);
            return ok && emit_raw(builder, DOLVM_OP_CONST64, dst, 0, 0, pool);
        }
    case DOLIR_OP_STATE_READ: {
        u8 home = home_register(fn, inst->aux);
        if (home != DOLVM_NO_REG)
            // The value the read produces *is* the home register, so there is
            // nothing to emit -- unless it has to outlive the home's next
            // write, in which case it is copied out for one dispatch, which is
            // what the load cost anyway.
            return fn->home_read[index] != 2u ||
                   emit_raw(builder, DOLVM_OP_MOV, dst, home, 0, 0);
        return emit_raw(builder, state_load_op(dolir_state_type(inst->aux)), dst,
                        0, 0, state_offset(inst->aux));
    }
    case DOLIR_OP_STATE_WRITE: {
        u8 source = fn->reg[inst->operands[0]];
        u8 home = home_register(fn, inst->aux);
        if (home != DOLVM_NO_REG) {
            // Either the instruction that computed the value already put it in
            // the home, or it is already there because that is where it was
            // read from; both leave nothing to do.
            if (fn->home_write[index] == 1u || source == home)
                return true;
            // The slot's width, not the value's, for the reason the store below
            // gives -- a home for a 32-bit slot holds a zero-extended value and
            // everything downstream is entitled to assume so.
            return emit_raw(builder, DOLVM_OP_TRUNC, home, source, 0,
                            width_selector(dolir_state_type(inst->aux)));
        }
        // The slot's width, not the value's. An elided truncate can leave a
        // 32-bit register's value declared i64, and storing eight bytes there
        // would take the next register with it.
        return emit_raw(builder,
                        state_store_op(dolir_state_type(inst->aux)), 0,
                        source, 0, state_offset(inst->aux));
    }
    case DOLIR_OP_ADD: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_ADD64
                                                   : DOLVM_OP_ADD32,
                      inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_NOP
                                                   : DOLVM_OP_ADD32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_SUB: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_SUB64
                                                   : DOLVM_OP_SUB32,
                      DOLVM_OP_NOP};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_MUL: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_MUL64
                                                   : DOLVM_OP_MUL32,
                      inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_NOP
                                                   : DOLVM_OP_MUL32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_UDIV: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_UDIV64
                                                   : DOLVM_OP_UDIV32,
                      DOLVM_OP_NOP};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_SDIV: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_SDIV64
                                                   : DOLVM_OP_SDIV32,
                      DOLVM_OP_NOP};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_AND: {
        if (fn->shift_fused[index]) {
            u32 shift_index = fn->def_index[inst->operands[0]];
            u64 mask = 0;
            constant_in_block(fn, block, inst->operands[1], &mask);
            return emit_raw(builder, fn->shift_fused[index], dst,
                            fn->reg[block->instructions[shift_index].operands[0]],
                            fn->shift_amount[index], (u32)mask);
        }
        OpPair ops = {DOLVM_OP_AND, DOLVM_OP_ANDI};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_OR: {
        OpPair ops = {DOLVM_OP_OR, DOLVM_OP_ORI};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_XOR: {
        OpPair ops = {DOLVM_OP_XOR, DOLVM_OP_XORI};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_NOT:
        return emit_raw(builder, DOLVM_OP_NOT, dst, fn->reg[inst->operands[0]],
                        0, width_selector(inst->type));
    case DOLIR_OP_SHL: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_SHL64
                                                   : DOLVM_OP_SHL32,
                      inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_SHL64I
                                                   : DOLVM_OP_SHL32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_LSHR: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_LSHR64
                                                   : DOLVM_OP_LSHR32,
                      inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_LSHR64I
                                                   : DOLVM_OP_LSHR32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_ASHR: {
        OpPair ops = {inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_ASHR64
                                                   : DOLVM_OP_ASHR32,
                      inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_ASHR64I
                                                   : DOLVM_OP_ASHR32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_ROTL: {
        OpPair ops = {DOLVM_OP_ROTL32, DOLVM_OP_ROTL32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_CLZ:
        return emit_raw(builder,
                        inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_CLZ64
                                                     : DOLVM_OP_CLZ32,
                        dst, fn->reg[inst->operands[0]], 0, 0);
    case DOLIR_OP_BSWAP:
        return emit_raw(builder,
                        inst->type == DOLIR_TYPE_I16   ? DOLVM_OP_BSWAP16
                        : inst->type == DOLIR_TYPE_I64 ? DOLVM_OP_BSWAP64
                                                       : DOLVM_OP_BSWAP32,
                        dst, fn->reg[inst->operands[0]], 0, 0);
    case DOLIR_OP_TRUNC:
        return emit_raw(builder, DOLVM_OP_TRUNC, dst, fn->reg[inst->operands[0]],
                        0, width_selector(inst->type));
    case DOLIR_OP_ZEXT:
    case DOLIR_OP_BITCAST:
        // Registers are untyped storage and integers are held zero-extended,
        // so neither of these moves a bit; the optimizer usually removes them.
        return emit_raw(builder, DOLVM_OP_MOV, dst, fn->reg[inst->operands[0]],
                        0, 0);
    case DOLIR_OP_SEXT:
        // aux carries the width the operand was written at; see
        // stamp_operand_widths.
        return emit_raw(builder, DOLVM_OP_SEXT, dst, fn->reg[inst->operands[0]],
                        0,
                        width_selector((DolIRType)inst->aux) |
                            (width_selector(inst->type) << 8));
    case DOLIR_OP_ICMP_EQ: {
        OpPair ops = {DOLVM_OP_ICMP_EQ, DOLVM_OP_ICMP_EQI};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_ICMP_NE: {
        OpPair ops = {DOLVM_OP_ICMP_NE, DOLVM_OP_ICMP_NEI};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_ICMP_ULT: {
        OpPair ops = {DOLVM_OP_ICMP_ULT, DOLVM_OP_ICMP_ULTI};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_ICMP_ULE: {
        OpPair ops = {DOLVM_OP_ICMP_ULE, DOLVM_OP_ICMP_ULEI};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_ICMP_SLT: {
        bool wide = (DolIRType)inst->aux == DOLIR_TYPE_I64;
        OpPair ops = {wide ? DOLVM_OP_ICMP_SLT64 : DOLVM_OP_ICMP_SLT32,
                      wide ? DOLVM_OP_NOP : DOLVM_OP_ICMP_SLT32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_ICMP_SLE: {
        bool wide = (DolIRType)inst->aux == DOLIR_TYPE_I64;
        OpPair ops = {wide ? DOLVM_OP_ICMP_SLE64 : DOLVM_OP_ICMP_SLE32,
                      wide ? DOLVM_OP_NOP : DOLVM_OP_ICMP_SLE32I};
        return lower_binary(fn, block, inst, ops);
    }
    case DOLIR_OP_FCMP_OEQ:
        return emit_raw(builder, DOLVM_OP_FCMP_OEQ, dst,
                        fn->reg[inst->operands[0]], fn->reg[inst->operands[1]], 0);
    case DOLIR_OP_FCMP_OLT:
        return emit_raw(builder, DOLVM_OP_FCMP_OLT, dst,
                        fn->reg[inst->operands[0]], fn->reg[inst->operands[1]], 0);
    case DOLIR_OP_FCMP_OGE:
        return emit_raw(builder, DOLVM_OP_FCMP_OGE, dst,
                        fn->reg[inst->operands[0]], fn->reg[inst->operands[1]], 0);
    case DOLIR_OP_SELECT:
        if (vector_type(inst->type)) {
            fail(builder, "dolvm: vector select at 0x%08X\n", inst->guest_pc);
            return false;
        }
        return emit_raw(builder, DOLVM_OP_SELECT, dst,
                        fn->reg[inst->operands[0]], fn->reg[inst->operands[1]],
                        fn->reg[inst->operands[2]]);
    case DOLIR_OP_FADD:
    case DOLIR_OP_FSUB:
    case DOLIR_OP_FMUL:
    case DOLIR_OP_FDIV: {
        u8 op = inst->op == DOLIR_OP_FADD   ? DOLVM_OP_FADD
                : inst->op == DOLIR_OP_FSUB ? DOLVM_OP_FSUB
                : inst->op == DOLIR_OP_FMUL ? DOLVM_OP_FMUL
                                            : DOLVM_OP_FDIV;
        if (!vector_type(inst->type))
            return emit_raw(builder, op, dst, fn->reg[inst->operands[0]],
                            fn->reg[inst->operands[1]], 0);
        return emit_raw(builder, op, dst, fn->reg[inst->operands[0]],
                        fn->reg[inst->operands[1]], 0) &&
               emit_raw(builder, op, fn->reg_hi[inst->result],
                        fn->reg_hi[inst->operands[0]],
                        fn->reg_hi[inst->operands[1]], 0);
    }
    case DOLIR_OP_FNEG:
    case DOLIR_OP_FABS: {
        u8 op = inst->op == DOLIR_OP_FNEG ? DOLVM_OP_FNEG : DOLVM_OP_FABS;
        if (vector_type(inst->type))
            return lower_vector_unary(fn, inst, op);
        return emit_raw(builder, op, dst, fn->reg[inst->operands[0]], 0, 0);
    }
    case DOLIR_OP_FPTRUNC:
        return emit_raw(builder, DOLVM_OP_FPTRUNC, dst,
                        fn->reg[inst->operands[0]], 0, 0);
    case DOLIR_OP_FPEXT:
        return emit_raw(builder, DOLVM_OP_FPEXT, dst,
                        fn->reg[inst->operands[0]], 0, 0);
    case DOLIR_OP_VECTOR_BUILD:
        return emit_raw(builder, DOLVM_OP_MOV, dst, fn->reg[inst->operands[0]],
                        0, 0) &&
               emit_raw(builder, DOLVM_OP_MOV, fn->reg_hi[inst->result],
                        fn->reg[inst->operands[1]], 0, 0);
    case DOLIR_OP_VECTOR_EXTRACT:
        return emit_raw(builder, DOLVM_OP_MOV, dst,
                        inst->aux ? fn->reg_hi[inst->operands[0]]
                                  : fn->reg[inst->operands[0]],
                        0, 0);
    case DOLIR_OP_VECTOR_SHUFFLE: {
        u32 lanes[2] = {inst->aux & 0xFFu, (inst->aux >> 8) & 0xFFu};
        u8 pick[2];
        for (u32 lane = 0; lane < 2; lane++) {
            DolIRValue source = lanes[lane] < 2 ? inst->operands[0]
                                                : inst->operands[1];
            pick[lane] = (lanes[lane] & 1u) ? fn->reg_hi[source]
                                            : fn->reg[source];
        }
        // Both moves read before either writes only if the destination pair is
        // distinct from the sources; the allocator hands out a fresh pair for
        // every result, so it is.
        return emit_raw(builder, DOLVM_OP_MOV, dst, pick[0], 0, 0) &&
               emit_raw(builder, DOLVM_OP_MOV, fn->reg_hi[inst->result], pick[1],
                        0, 0);
    }
    case DOLIR_OP_GUEST_LOAD: {
        u32 width = inst->aux & 0xFFu;
        bool sign = (inst->aux & 0x100u) != 0;
        u8 op = width == 1u   ? (sign ? DOLVM_OP_LOAD8S : DOLVM_OP_LOAD8)
                : width == 2u ? (sign ? DOLVM_OP_LOAD16S : DOLVM_OP_LOAD16)
                : width == 4u ? (sign ? DOLVM_OP_LOAD32S : DOLVM_OP_LOAD32)
                              : DOLVM_OP_LOAD64;
        u8 base = 0;
        u32 displacement = 0;
        memory_address(fn, block, inst->operands[0], &base, &displacement);
        if (fn->mem_fused[index]) {
            u32 destination =
                state_offset(block->instructions[fn->mem_write[index]].aux);
            if (fn->mem_fused[index] == 2u)
                return emit_raw(builder, DOLVM_OP_LOAD_MEM_TO_STATE, (u8)pc_word,
                                op, base, displacement) &&
                       emit_payload(builder, ((u64)destination << 32) | dst);
            return emit_raw(builder, DOLVM_OP_LOAD_MEM_STATE, (u8)pc_word, op, dst,
                            displacement) &&
                   emit_payload(builder,
                                ((u64)destination << 32) | fn->mem_base_off[index]);
        }
        return emit_raw(builder, op, dst, base, (u8)pc_word, displacement);
    }
    case DOLIR_OP_GUEST_STORE: {
        u32 width = inst->aux & 0xFFu;
        if (fn->mem_fused[index]) {
            u8 fused = width == 1u   ? DOLVM_OP_STORE8
                       : width == 2u ? DOLVM_OP_STORE16
                                     : DOLVM_OP_STORE32;
            u32 fused_displacement = 0;
            u8 fused_base = 0;
            memory_address(fn, block, inst->operands[0], &fused_base,
                           &fused_displacement);
            if (fn->mem_fused[index] == 2u)
                return emit_raw(builder, DOLVM_OP_STORE_MEM_FROM_STATE,
                                (u8)pc_word, fused, fused_base,
                                fused_displacement) &&
                       emit_payload(builder, fn->mem_value_off[index]);
            return emit_raw(builder, DOLVM_OP_STORE_MEM_STATE, (u8)pc_word, fused,
                            0, fused_displacement) &&
                   emit_payload(builder, ((u64)fn->mem_value_off[index] << 32) |
                                             fn->mem_base_off[index]);
        }
        u8 op = width == 1u   ? DOLVM_OP_STORE8
                : width == 2u ? DOLVM_OP_STORE16
                : width == 4u ? DOLVM_OP_STORE32
                              : DOLVM_OP_STORE64;
        u8 base = 0;
        u32 displacement = 0;
        memory_address(fn, block, inst->operands[0], &base, &displacement);
        return emit_raw(builder, op, (u8)pc_word, base,
                        fn->reg[inst->operands[1]], displacement);
    }
    case DOLIR_OP_HELPER_CALL:
        switch (inst->aux) {
        case DOLIR_HELPER_FP_AVAILABLE:
            return emit_raw(builder, DOLVM_OP_FP_AVAILABLE, 0, 0, 0,
                            inst->guest_pc);
        case DOLIR_HELPER_MEMORY_FENCE:
            return emit_raw(builder, DOLVM_OP_FENCE, 0, 0, 0, 0);
        case DOLIR_HELPER_CR_FIELD: {
            u32 left = 0;
            u32 right = 0;
            u64 immediate = 0;
            cr_field_operands(fn, block, inst, (CRFieldForm)fn->cr_form[index],
                              &left, &right, &immediate);
            switch ((CRFieldForm)fn->cr_form[index]) {
            case CR_FIELD_REG_I:
                return emit_raw(builder, DOLVM_OP_SET_CR_FIELDI, 0,
                                fn->reg[inst->operands[0]], 0,
                                (u32)inst->immediate) &&
                       emit_payload(builder, (u32)immediate);
            case CR_FIELD_STATE_I:
                return emit_raw(builder, DOLVM_OP_CMP_STATE_I, 0, 0, 0,
                                (u32)inst->immediate) &&
                       emit_payload(builder, ((u64)left << 32) | (u32)immediate);
            case CR_FIELD_STATE:
                return emit_raw(builder, DOLVM_OP_CMP_STATE, 0, 0, 0,
                                (u32)inst->immediate) &&
                       emit_payload(builder, ((u64)left << 32) | right);
            default:
                return emit_raw(builder, DOLVM_OP_SET_CR_FIELD, 0,
                                fn->reg[inst->operands[0]],
                                fn->reg[inst->operands[1]],
                                (u32)inst->immediate);
            }
        }
        case DOLIR_HELPER_EXACT_FLOAT:
            return emit_raw(builder, DOLVM_OP_EXACT_FLOAT, 0, 0, 0, 0) &&
                   emit_payload(builder, inst->immediate);
        case DOLIR_HELPER_EXACT_PAIRED:
            return emit_raw(builder, DOLVM_OP_EXACT_PAIRED, 0, 0, 0, 0) &&
                   emit_payload(builder, inst->immediate);
        case DOLIR_HELPER_PSQ_LOAD:
        case DOLIR_HELPER_PSQ_STORE:
            return emit_raw(builder,
                            inst->aux == DOLIR_HELPER_PSQ_LOAD
                                ? DOLVM_OP_PSQ_LOAD
                                : DOLVM_OP_PSQ_STORE,
                            dst, fn->reg[inst->operands[0]], 0,
                            inst->guest_pc) &&
                   emit_payload(builder, inst->immediate);
        case DOLIR_HELPER_STORE_CONDITIONAL:
            return emit_raw(builder, DOLVM_OP_STWCX, 0,
                            fn->reg[inst->operands[0]],
                            (u8)(inst->immediate & 0xFFu), inst->guest_pc);
        case DOLIR_HELPER_FPSCR_UPDATED:
            return emit_raw(builder, DOLVM_OP_FPSCR_UPDATED, 0, 0, 0, 0);
        case DOLIR_HELPER_FPSCR_BIT:
            return emit_raw(builder, DOLVM_OP_FPSCR_BIT, 0, 0, 0,
                            (u32)(inst->immediate & 0xFFFFu));
        case DOLIR_HELPER_PROGRAM_EXCEPTION:
            if (fn->supervisor[index])
                return emit_raw(builder, DOLVM_OP_SUPERVISOR, 0, 0, 0,
                                inst->guest_pc);
            return emit_raw(builder, DOLVM_OP_PROGRAM_EXC, 0,
                            fn->reg[inst->operands[0]], 0, inst->guest_pc) &&
                   emit_payload(builder, inst->immediate);
        case DOLIR_HELPER_SPR_READ:
            return emit_raw(builder, DOLVM_OP_SPR_READ, dst, 0, 0,
                            inst->guest_pc) &&
                   emit_payload(builder, inst->immediate);
        case DOLIR_HELPER_SPR_WRITE:
            return emit_raw(builder, DOLVM_OP_SPR_WRITE, 0,
                            fn->reg[inst->operands[0]], 0, inst->guest_pc) &&
                   emit_payload(builder, inst->immediate);
        case DOLIR_HELPER_LSWX:
            return emit_raw(builder, DOLVM_OP_LSWX, 0, 0, 0, inst->guest_pc) &&
                   emit_payload(builder, inst->immediate);
        case DOLIR_HELPER_DCBZ_L:
            return emit_raw(builder, DOLVM_OP_DCBZ_L, 0,
                            fn->reg[inst->operands[0]], 0, inst->guest_pc);
        case DOLIR_HELPER_ECIWX:
            return emit_raw(builder, DOLVM_OP_ECIWX, dst,
                            fn->reg[inst->operands[0]], 0, inst->guest_pc);
        case DOLIR_HELPER_ECOWX:
            return emit_raw(builder, DOLVM_OP_ECOWX, 0,
                            fn->reg[inst->operands[0]],
                            fn->reg[inst->operands[1]], inst->guest_pc);
        case DOLIR_HELPER_TLBIE:
            return emit_raw(builder, DOLVM_OP_TLBIE, 0,
                            fn->reg[inst->operands[0]], 0, inst->guest_pc);
        case DOLIR_HELPER_CACHE_CONTROL:
            return emit_raw(builder, DOLVM_OP_CACHE_CONTROL, 0,
                            fn->reg[inst->operands[0]], 0, inst->guest_pc) &&
                   emit_payload(builder, inst->immediate);
        default:
            fail(builder, "dolvm: unsupported helper at 0x%08X\n",
                 inst->guest_pc);
            return false;
        }
    default:
        (void)index;
        fail(builder, "dolvm: unsupported DolIR op at 0x%08X\n", inst->guest_pc);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Terminators
// ---------------------------------------------------------------------------

// Where control goes when a terminator edge is taken. A target inside this
// function is a jump; anything else is either an in-module call (opt-in) or a
// return to the chassis with ctx->pc naming the address.
static bool emit_destination(FunctionEmitter* fn, const DolIRTerminator* term,
                             u32 slot) {
    Builder* builder = fn->builder;
    u32 target_block = term->targets[slot];
    if (target_block != DOLIR_NO_BLOCK) {
        // A back edge is where an interpreted loop would otherwise hold the
        // dispatch forever. The guard goes on the edge rather than on the loop
        // header so that entering the header with the budget already spent
        // still runs the body once and makes progress.
        // The target opens with its cycle charge unless it costs nothing, and a
        // branch that pays that charge itself lands past it.
        u32 charge = fn->function->blocks[target_block].cycle_cost;
        PatchKind kind = charge ? PATCH_BLOCK_BODY : PATCH_BLOCK;
        if (target_block <= fn->current_block) {
            u32 index = builder->code_count;
            return emit_raw(builder, DOLVM_OP_JMP_GUARD,
                            fn->idle_loop && slot == 0 ? 1u : 0u, 0, 0, 0) &&
                   emit_payload(builder,
                                ((u64)charge << 32) |
                                    fn->function->blocks[target_block].guest_address) &&
                   function_patch(fn, index, kind, target_block);
        }
        u32 index = builder->code_count;
        if (charge)
            return emit_raw(builder, DOLVM_OP_JMP_CHARGE, 0, 0, 0, 0) &&
                   emit_payload(builder, charge) &&
                   function_patch(fn, index, kind, target_block);
        return emit_raw(builder, DOLVM_OP_JMP, 0, 0, 0, 0) &&
               function_patch(fn, index, PATCH_BLOCK, target_block);
    }
    u32 address = term->target_addresses[slot];
    if (builder->direct_calls) {
        u32 index = builder->code_count;
        return emit_raw(builder, DOLVM_OP_CALL, 0, 0, 0, 0) &&
               emit_payload(builder, (u64)address << 32) &&
               builder_patch(builder, index, PATCH_ADDRESS, address);
    }
    return emit_raw(builder, DOLVM_OP_EXIT, 0, 0, 0, address);
}

static bool emit_terminator(FunctionEmitter* fn, const DolIRBlock* block) {
    Builder* builder = fn->builder;
    const DolIRTerminator* term = &block->terminator;

    switch (term->kind) {
    case DOLIR_TERM_BRANCH:
        return emit_destination(fn, term, 0);
    case DOLIR_TERM_COND_BRANCH: {
        if (fn->constant_branch)
            return emit_destination(fn, term, fn->constant_branch_taken ? 0u : 1u);
        // A fused condition-register branch can name its target block directly
        // instead of jumping over the other edge's code, which takes the
        // unconditional jump off the taken path entirely. Backwards, that jump
        // was the guarded one, so the guard comes along too -- which is a loop's
        // whole back edge in a single dispatch.
        u32 taken = term->targets[0];
        if (fn->cr_compare) {
            u32 charge = fn->function->blocks[taken].cycle_cost;
            u32 index = builder->code_count;
            bool backward = taken <= fn->current_block;
            u8 left = fn->reg[fn->cr_compare_left];
            bool emitted;
            if (backward) {
                emitted = emit_raw(builder, DOLVM_OP_CMP_JMP_IF_CR_GUARD, left,
                                   fn->cr_compare_pack |
                                       (fn->idle_loop ? 0x80u : 0u),
                                   0, 0) &&
                          emit_payload(builder, ((u64)charge << 32) |
                                                    (u32)fn->cr_compare_right) &&
                          emit_payload(builder,
                                       fn->function->blocks[taken].guest_address);
            } else if (charge) {
                emitted = emit_raw(builder, DOLVM_OP_CMP_JMP_IF_CR_CHARGE, left,
                                   fn->cr_compare_pack, 0, 0) &&
                          emit_payload(builder, ((u64)charge << 32) |
                                                    (u32)fn->cr_compare_right);
            } else {
                emitted = emit_raw(builder, DOLVM_OP_CMP_JMP_IF_CR, left,
                                   fn->cr_compare_pack, 0, 0) &&
                          emit_payload(builder, (u32)fn->cr_compare_right);
            }
            return emitted &&
                   function_patch(fn, index,
                                  charge ? PATCH_BLOCK_BODY : PATCH_BLOCK, taken) &&
                   emit_destination(fn, term, 1);
        }
        if (fn->cr_branch && taken != DOLIR_NO_BLOCK) {
            u32 charge = fn->function->blocks[taken].cycle_cost;
            u32 index = builder->code_count;
            bool backward = taken <= fn->current_block;
            bool emitted;
            if (backward) {
                emitted = emit_raw(builder, DOLVM_OP_JMP_IF_CR_GUARD,
                                   fn->cr_branch_bit,
                                   fn->cr_branch_sense | (fn->idle_loop ? 0x80u : 0u),
                                   0, 0) &&
                          emit_payload(builder,
                                       ((u64)charge << 32) |
                                           fn->function->blocks[taken].guest_address);
            } else if (charge) {
                emitted = emit_raw(builder, DOLVM_OP_JMP_IF_CR_CHARGE,
                                   fn->cr_branch_bit, fn->cr_branch_sense, 0, 0) &&
                          emit_payload(builder, charge);
            } else {
                emitted = emit_raw(builder, DOLVM_OP_JMP_IF_CR, fn->cr_branch_bit,
                                   fn->cr_branch_sense, 0, 0);
            }
            return emitted &&
                   function_patch(fn, index,
                                  charge ? PATCH_BLOCK_BODY : PATCH_BLOCK, taken) &&
                   emit_destination(fn, term, 1);
        }
        u32 branch = builder->code_count;
        bool emitted = fn->cr_branch
                           ? emit_raw(builder, DOLVM_OP_JMP_IF_CR,
                                      fn->cr_branch_bit, fn->cr_branch_sense, 0, 0)
                           : emit_raw(builder,
                                      fn->invert_condition ? DOLVM_OP_JMP_IFNOT
                                                           : DOLVM_OP_JMP_IF,
                                      0, fn->reg[fn->terminator_condition], 0, 0);
        if (!emitted)
            return false;
        if (!emit_destination(fn, term, 1))
            return false;
        builder->code[branch].imm = builder->code_count;
        return emit_destination(fn, term, 0);
    }
    case DOLIR_TERM_INDIRECT: {
        if (fn->constant_branch)
            return fn->constant_branch_taken
                       ? emit_raw(builder, DOLVM_OP_INDIRECT,
                                  fn->indirect_mask ? 1u : 0u,
                                  fn->reg[fn->indirect_target], 0,
                                  fn->region_index)
                       : emit_destination(fn, term, 1);
        u32 branch = builder->code_count;
        bool emitted = fn->cr_branch
                           ? emit_raw(builder, DOLVM_OP_JMP_IF_CR,
                                      fn->cr_branch_bit, fn->cr_branch_sense, 0, 0)
                           : emit_raw(builder,
                                      fn->invert_condition ? DOLVM_OP_JMP_IF
                                                           : DOLVM_OP_JMP_IFNOT,
                                      0, fn->reg[fn->terminator_condition], 0, 0);
        if (!emitted)
            return false;
        if (!emit_raw(builder, DOLVM_OP_INDIRECT, fn->indirect_mask ? 1u : 0u,
                      fn->reg[fn->indirect_target], 0, fn->region_index))
            return false;
        builder->code[branch].imm = builder->code_count;
        return emit_destination(fn, term, 1);
    }
    case DOLIR_TERM_SIDE_EXIT:
    case DOLIR_TERM_RETURN:
        return emit_raw(builder, DOLVM_OP_EXIT, 0, 0, 0,
                        term->target_addresses[0]);
    case DOLIR_TERM_FALLBACK: {
        if (!emit_raw(builder, DOLVM_OP_FALLBACK, 0, 0, 0, term->guest_pc) ||
            !emit_payload(builder, term->raw))
            return false;
        // The fallback resumes at the next instruction, which merging always
        // left as its own block, so name it by address.
        u32 next = term->guest_pc + 4u;
        if (next >= fn->function->guest_end)
            return emit_raw(builder, DOLVM_OP_EXIT, 0, 0, 0, next);
        u32 index = builder->code_count;
        return emit_raw(builder, DOLVM_OP_JMP, 0, 0, 0, 0) &&
               function_patch(fn, index, PATCH_ADDRESS, next);
    }
    case DOLIR_TERM_SYSTEM_CALL:
        return emit_raw(builder, DOLVM_OP_SYSCALL, 0, 0, 0, term->guest_pc);
    case DOLIR_TERM_RFI:
        return emit_raw(builder, DOLVM_OP_RFI, 0, 0, 0, term->guest_pc);
    default:
        fail(builder, "dolvm: missing terminator at 0x%08X\n", term->guest_pc);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Function emission
// ---------------------------------------------------------------------------

// Is `value` in a register of its own at instruction `at`, by the emitter's
// accounting rather than the optimizer's?
static bool recipe_live_at(FunctionEmitter* fn, DolIRValue value, u32 at) {
    return value && fn->reg[value] != DOLVM_NO_REG &&
           fn->def_index[value] < at && fn->last_use[value] > at;
}

// Does replaying this recipe cost anything? A value that lives in a home is
// already there the moment the dispatch begins, because filling the homes is
// how a dispatch begins -- so its recipe is a no-op, and an entry whose recipes
// are all no-ops needs no stub at all.
static bool recipe_needs_code(FunctionEmitter* fn, const DolVMRecipe* recipe,
                              u32 at) {
    if (!recipe_live_at(fn, recipe->value, at))
        return false;
    if (recipe->kind != DOLVM_RECIPE_STATE)
        return true;
    u8 home = home_register(fn, recipe->slot);
    return home == DOLVM_NO_REG || fn->reg[recipe->value] != home;
}

static bool emit_recipe(FunctionEmitter* fn, const DolVMRecipe* recipe) {
    Builder* builder = fn->builder;
    u8 reg = fn->reg[recipe->value];
    if (reg == DOLVM_NO_REG)
        return true;
    if (recipe->kind == DOLVM_RECIPE_STATE) {
        u8 home = home_register(fn, recipe->slot);
        if (home != DOLVM_NO_REG)
            return reg == home ||
                   emit_raw(builder, DOLVM_OP_MOV, reg, home, 0, 0);
        return emit_raw(builder, state_load_op(dolir_state_type(recipe->slot)),
                        reg, 0, 0, state_offset(recipe->slot));
    }
    if (recipe->immediate <= 0xFFFFFFFFull)
        return emit_raw(builder, DOLVM_OP_CONST32, reg, 0, 0,
                        (u32)recipe->immediate);
    bool ok = false;
    u32 pool = intern_constant(builder, recipe->immediate, &ok);
    return ok && emit_raw(builder, DOLVM_OP_CONST64, reg, 0, 0, pool);
}

static bool emit_block(FunctionEmitter* fn, u32 block_index) {
    Builder* builder = fn->builder;
    DolIRFunction* function = fn->function;
    DolIRBlock* block = &function->blocks[block_index];
    u32 count = block->instruction_count;

    analyze_block(fn, block);

    // 0xFF is the "no register" sentinel, so handing it out as a real register
    // would make an allocated value indistinguishable from an unallocated one.
    fn->free_count = 0;
    for (u32 r = DOLVM_USABLE_REGISTERS; r-- > 0;)
        fn->free_registers[fn->free_count++] = (u8)r;

    if (count + 2u > fn->inst_offset_capacity) {
        u32 capacity = count + 2u;
        u32* offsets = (u32*)realloc(fn->inst_offset, (size_t)capacity * sizeof(u32));
        if (!offsets)
            return false;
        fn->inst_offset = offsets;
        u32* bases = (u32*)realloc(fn->pc_base_at, (size_t)capacity * sizeof(u32));
        if (!bases)
            return false;
        fn->pc_base_at = bases;
        fn->inst_offset_capacity = capacity;
    }

    fn->block_start[block_index] = builder->code_count;
    fn->pc_base = block->guest_address;
    fn->current_block = block_index;
    fn->idle_loop = idle_loop_block(block, block_index);

    if (block->cycle_cost &&
        !emit_raw(builder, DOLVM_OP_CHARGE, 0, 0, 0, block->cycle_cost))
        return false;

    for (u32 i = 0; i < count; i++) {
        DolIRInstruction* inst = &block->instructions[i];
        // A block long enough to outrun the byte-sized pc offset carried by
        // memory operations restarts the base; entries past this point pick the
        // new base up from the map.
        if ((inst->guest_pc - fn->pc_base) / 4u > 0xFFu) {
            fn->pc_base = inst->guest_pc;
            if (!emit_raw(builder, DOLVM_OP_PC_BASE, 0, 0, 0, fn->pc_base))
                return false;
        }
        fn->inst_offset[i] = builder->code_count;
        fn->pc_base_at[i] = fn->pc_base;
        if (fn->folded[i])
            continue;
        // A lane operation whose sources all die here does not move anything:
        // the result takes their registers and nothing is emitted.
        if (inst->result && try_coalesce_lanes(fn, inst, i))
            continue;
        if (inst->result) {
            // A value that lives in a home is not allocated out of the block's
            // pool: either it was read out of one, or analysis decided the
            // instruction should compute straight into one.
            u8 home = fn->home_dest[i];
            if (home == DOLVM_NO_REG && fn->home_read[i] == 1u)
                home = home_register(fn, inst->aux);
            if (home != DOLVM_NO_REG) {
                fn->reg[inst->result] = home;
                fn->reg_hi[inst->result] = DOLVM_NO_REG;
            } else if (!assign_result(fn, inst->result, inst->type)) {
                fail(builder, "dolvm: out of registers at 0x%08X\n",
                     inst->guest_pc);
                return false;
            }
        }
        if (!lower_instruction(fn, block, inst, i))
            return false;
        retire_operands(fn, block, inst, i);
    }
    fn->inst_offset[count] = builder->code_count;
    fn->pc_base_at[count] = fn->pc_base;
    if (!emit_terminator(fn, block))
        return false;

    // Entry stubs. An address nothing crosses into enters the body directly;
    // one that does gets its live values rebuilt first, then jumps in.
    DolVMLayout* layout = fn->layout;
    for (u32 e = 0; e < layout->entry_count; e++) {
        DolVMEntry* entry = &layout->entries[e];
        if (entry->block != block_index)
            continue;
        // The optimizer proposes a recipe for every value it let cross this
        // boundary, but its notion of a live value is the IR's. Lowering folds
        // constants into immediates and address adds into displacements, which
        // shortens live ranges and frees registers earlier -- so a value the
        // optimizer still considers live may share a register with something
        // else by now. Replaying its recipe would overwrite that. The emitter's
        // own liveness is the one the registers were allocated against, so it
        // decides.
        u32 wanted = 0;
        for (u32 r = 0; r < entry->recipe_count; r++) {
            const DolVMRecipe* recipe = &layout->recipes[entry->recipe_start + r];
            if (recipe_needs_code(fn, recipe, entry->instruction))
                wanted++;
        }
        u32 offset;
        if (wanted) {
            offset = builder->code_count;
            for (u32 r = 0; r < entry->recipe_count; r++) {
                const DolVMRecipe* recipe =
                    &layout->recipes[entry->recipe_start + r];
                if (!recipe_needs_code(fn, recipe, entry->instruction))
                    continue;
                if (!emit_recipe(fn, recipe))
                    return false;
            }
            if (!emit_raw(builder, DOLVM_OP_JMP, 0, 0, 0,
                          fn->inst_offset[entry->instruction]))
                return false;
        } else if (entry->guest_address == block->guest_address) {
            // The head of a block enters through its prologue, so the cycle
            // charge lands exactly as it does on a fallthrough into the block.
            // Interior addresses skip it, which is what the C backend's own
            // leader-only charge does for the same case.
            offset = fn->block_start[block_index];
        } else {
            offset = fn->inst_offset[entry->instruction];
        }
        DolVMEntryPoint* point = &builder->map[fn->map_base + e];
        point->entry =
            offset | (entry->return_target ? DOLVM_ENTRY_RETURN_TARGET : 0u);
        point->pc_base = fn->pc_base_at[entry->instruction];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Function and module emission
// ---------------------------------------------------------------------------

static bool reserve_map(Builder* builder, u32 count) {
    if (builder->map_count + count > builder->map_capacity) {
        u32 capacity = builder->map_capacity ? builder->map_capacity : 1024u;
        while (capacity < builder->map_count + count)
            capacity *= 2u;
        DolVMEntryPoint* grown = (DolVMEntryPoint*)realloc(
            builder->map, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return false;
        builder->map = grown;
        builder->map_capacity = capacity;
    }
    for (u32 i = 0; i < count; i++) {
        builder->map[builder->map_count + i].entry = DOLVM_NO_ENTRY;
        builder->map[builder->map_count + i].pc_base = 0;
    }
    builder->map_count += count;
    return true;
}

static bool add_region(Builder* builder, u32 start, u32 end, u32 map_index) {
    GROW(builder->regions, builder->region_count, builder->region_capacity,
         DolVMRegion);
    DolVMRegion* region = &builder->regions[builder->region_count++];
    region->guest_start = start;
    region->guest_end = end;
    region->map_index = map_index;
    region->flags = 0;
    return true;
}

static bool emit_function(Builder* builder, DolIRFunction* function,
                          DolVMLayout* layout, u32 region_index) {
    FunctionEmitter fn;
    memset(&fn, 0, sizeof(fn));
    fn.builder = builder;
    fn.function = function;
    fn.layout = layout;
    fn.region_index = region_index;
    fn.map_base = builder->map_count - layout->entry_count;

    u32 values = function->value_count;
    fn.block_start = (u32*)calloc(function->block_count, sizeof(u32));
    fn.reg = (u8*)malloc(values);
    fn.reg_hi = (u8*)malloc(values);
    fn.last_use = (u32*)calloc(values, sizeof(u32));
    fn.def_index = (u32*)calloc(values, sizeof(u32));
    fn.use_count = (u32*)calloc(values, sizeof(u32));
    fn.memory_use_count = (u32*)calloc(values, sizeof(u32));
    fn.retired = (u8*)calloc(values, 1);
    u32 largest = 0;
    for (u32 b = 0; b < function->block_count; b++)
        if (function->blocks[b].instruction_count > largest)
            largest = function->blocks[b].instruction_count;
    fn.folded = (u8*)calloc(largest ? largest : 1u, 1);
    fn.cr_form = (u8*)calloc(largest ? largest : 1u, 1);
    fn.mem_fused = (u8*)calloc(largest ? largest : 1u, 1);
    fn.mem_write = (u32*)calloc(largest ? largest : 1u, sizeof(u32));
    fn.mem_base_off = (u32*)calloc(largest ? largest : 1u, sizeof(u32));
    fn.mem_value_off = (u32*)calloc(largest ? largest : 1u, sizeof(u32));
    fn.home_read = (u8*)calloc(largest ? largest : 1u, 1);
    fn.home_write = (u8*)calloc(largest ? largest : 1u, 1);
    fn.home_dest = (u8*)malloc(largest ? largest : 1u);
    fn.supervisor = (u8*)calloc(largest ? largest : 1u, 1);
    fn.shift_fused = (u8*)calloc(largest ? largest : 1u, 1);
    fn.shift_amount = (u8*)calloc(largest ? largest : 1u, 1);
    fn.homed = builder->homed;
    bool ok = fn.block_start && fn.reg && fn.reg_hi && fn.last_use &&
              fn.def_index && fn.use_count && fn.memory_use_count &&
              fn.retired && fn.folded && fn.cr_form && fn.mem_fused &&
              fn.shift_fused && fn.shift_amount &&
              fn.mem_write && fn.mem_base_off && fn.mem_value_off &&
              fn.home_read && fn.home_write && fn.home_dest &&
              fn.supervisor;
    if (ok) {
        memset(fn.reg, DOLVM_NO_REG, values);
        memset(fn.reg_hi, DOLVM_NO_REG, values);
        for (u32 v = 0; v < values; v++)
            fn.def_index[v] = 0xFFFFFFFFu;
        for (u32 b = 0; b < function->block_count && ok; b++)
            ok = emit_block(&fn, b);
    }

    // Block-local branches and the fallback's inline resume are resolved here,
    // where every block and every entry in this function has an address.
    for (u32 p = 0; p < fn.patch_count && ok; p++) {
        const Patch* patch = &fn.patches[p];
        if (patch->kind == PATCH_BLOCK || patch->kind == PATCH_BLOCK_BODY) {
            builder->code[patch->code_index].imm =
                fn.block_start[patch->operand] +
                (patch->kind == PATCH_BLOCK_BODY ? 1u : 0u);
            continue;
        }
        u32 index = (patch->operand - function->guest_start) / 4u;
        const DolVMEntryPoint* point = &builder->map[fn.map_base + index];
        if (point->entry == DOLVM_NO_ENTRY) {
            fail(builder, "dolvm: no entry for 0x%08X\n", patch->operand);
            ok = false;
            break;
        }
        builder->code[patch->code_index].imm =
            point->entry & DOLVM_ENTRY_OFFSET_MASK;
    }

    free(fn.block_start);
    free(fn.reg);
    free(fn.reg_hi);
    free(fn.last_use);
    free(fn.def_index);
    free(fn.use_count);
    free(fn.memory_use_count);
    free(fn.retired);
    free(fn.folded);
    free(fn.cr_form);
    free(fn.mem_fused);
    free(fn.mem_write);
    free(fn.mem_base_off);
    free(fn.mem_value_off);
    free(fn.home_read);
    free(fn.home_write);
    free(fn.home_dest);
    free(fn.supervisor);
    free(fn.inst_offset);
    free(fn.pc_base_at);
    free(fn.patches);
    return ok;
}

static int compare_functions(const void* left, const void* right) {
    u32 a = (*(DolIRFunction* const*)left)->guest_start;
    u32 b = (*(DolIRFunction* const*)right)->guest_start;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void builder_free(Builder* builder) {
    free(builder->code);
    free(builder->constants);
    free(builder->regions);
    free(builder->map);
    free(builder->patches);
    memset(builder, 0, sizeof(*builder));
}

static bool serialize(Builder* builder, const DolVMEmitOptions* options,
                      void** image, size_t* size) {
    u32 entry_point = options ? options->entry_point : 0u;
    u32 smc_count = options ? options->smc_count : 0u;
    size_t code_bytes = (size_t)builder->code_count * sizeof(DolVMInst);
    size_t constant_bytes = (size_t)builder->constant_count * sizeof(u64);
    size_t region_bytes = (size_t)builder->region_count * sizeof(DolVMRegion);
    size_t map_bytes = (size_t)builder->map_count * sizeof(DolVMEntryPoint);
    size_t hash_bytes = (size_t)builder->region_count * sizeof(u64);
    size_t smc_bytes = (size_t)smc_count * sizeof(DolVMRange);
    size_t total = sizeof(DolVMHeader) + code_bytes + constant_bytes +
                   region_bytes + map_bytes + hash_bytes + smc_bytes;
    u8* buffer = (u8*)malloc(total);
    if (!buffer)
        return false;

    u64* hashes = (u64*)calloc(builder->region_count ? builder->region_count : 1u,
                               sizeof(u64));
    if (!hashes) {
        free(buffer);
        return false;
    }
    if (options && options->hash_guest_range) {
        for (u32 i = 0; i < builder->region_count; i++) {
            if (!options->hash_guest_range(options->hash_user,
                                           builder->regions[i].guest_start,
                                           builder->regions[i].guest_end,
                                           &hashes[i])) {
                if (builder->diagnostics)
                    fprintf(builder->diagnostics,
                            "dolvm: cannot hash guest text 0x%08X-0x%08X\n",
                            builder->regions[i].guest_start,
                            builder->regions[i].guest_end);
                free(hashes);
                free(buffer);
                return false;
            }
        }
    }

    DolVMHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, DOLVM_MAGIC, DOLVM_MAGIC_SIZE);
    header.version = DOLVM_VERSION;
    header.abi_version = DOLVM_ABI_VERSION;
    header.cpu_state_size = (u32)sizeof(CPUState);
    header.flags = (builder->direct_calls ? DOLVM_FLAG_DIRECT_CALLS : 0u) |
                   (builder->homed ? DOLVM_FLAG_HOMED_STATE : 0u);
    header.entry_point = entry_point;
    header.code_count = builder->code_count;
    header.constant_count = builder->constant_count;
    header.region_count = builder->region_count;
    header.map_count = builder->map_count;
    header.smc_count = smc_count;
    header.state_layout_hash = dolvm_state_layout_hash();
    if (options && options->game_id) {
        size_t length = strlen(options->game_id);
        if (length >= sizeof(header.game_id))
            length = sizeof(header.game_id) - 1u;
        memcpy(header.game_id, options->game_id, length);
    }

    size_t offset = 0;
    memcpy(buffer + offset, &header, sizeof(header));
    offset += sizeof(header);
    memcpy(buffer + offset, builder->code, code_bytes);
    offset += code_bytes;
    memcpy(buffer + offset, builder->constants, constant_bytes);
    offset += constant_bytes;
    memcpy(buffer + offset, builder->regions, region_bytes);
    offset += region_bytes;
    memcpy(buffer + offset, builder->map, map_bytes);
    offset += map_bytes;
    memcpy(buffer + offset, hashes, hash_bytes);
    offset += hash_bytes;
    if (smc_bytes)
        memcpy(buffer + offset, options->smc_ranges, smc_bytes);
    free(hashes);

    *image = buffer;
    *size = total;
    return true;
}

// A call is two words. Turning it into a one-word exit leaves the payload
// behind, so it becomes a nop rather than something the stream walker would try
// to decode as an opcode.
static void downgrade_call_to_exit(Builder* builder, u32 index, u32 target) {
    builder->code[index].op = DOLVM_OP_EXIT;
    builder->code[index].a = 0;
    builder->code[index].b = 0;
    builder->code[index].c = 0;
    builder->code[index].imm = target;
    builder->code[index + 1u].op = DOLVM_OP_NOP;
    builder->code[index + 1u].a = 0;
    builder->code[index + 1u].b = 0;
    builder->code[index + 1u].c = 0;
    builder->code[index + 1u].imm = 0;
}

bool dolvm_build_module(DolIRModule* module, const DolVMEmitOptions* options,
                        void** image, size_t* size, DolVMOptStats* stats,
                        FILE* diagnostics) {
    if (!module || !module->function_count || !image || !size)
        return false;

    Builder builder;
    memset(&builder, 0, sizeof(builder));
    builder.direct_calls = options && options->direct_calls;
    builder.homed = options && options->home_state;
    builder.diagnostics = diagnostics;

    // Regions are looked up by binary search at runtime, so they go in address
    // order regardless of how the chunks reached us.
    DolIRFunction** order = (DolIRFunction**)malloc(
        (size_t)module->function_count * sizeof(*order));
    DolVMLayout* layouts = (DolVMLayout*)calloc(module->function_count,
                                                sizeof(*layouts));
    if (!order || !layouts) {
        free(order);
        free(layouts);
        return false;
    }
    for (u32 f = 0; f < module->function_count; f++)
        order[f] = &module->functions[f];
    qsort(order, module->function_count, sizeof(*order), compare_functions);

    bool ok = true;
    for (u32 f = 0; f < module->function_count && ok; f++) {
        DolVMOptStats delta;
        memset(&delta, 0, sizeof(delta));
        ok = dolvm_optimize_function(order[f], &layouts[f], &delta);
        if (ok && stats)
            dolvm_stats_add(stats, &delta);
    }
    for (u32 f = 0; f < module->function_count && ok; f++) {
        u32 map_index = builder.map_count;
        ok = reserve_map(&builder, layouts[f].entry_count) &&
             add_region(&builder, order[f]->guest_start, order[f]->guest_end,
                        map_index) &&
             emit_function(&builder, order[f], &layouts[f], f);
    }

    // Cross-function targets, which only exist when the module resolves its own
    // calls, are patched once every region has an entry map.
    // CALL names the region its target lies in (two bytes, a and b), which is
    // what a gated interpreter checks before following it.
    if (ok && builder.patch_count && builder.region_count > 0xFFFFu) {
        if (diagnostics)
            fprintf(diagnostics, "dolvm: %u regions, at most 65535 with calls\n",
                    builder.region_count);
        ok = false;
    }
    for (u32 p = 0; p < builder.patch_count && ok; p++) {
        const Patch* patch = &builder.patches[p];
        u32 target = patch->operand;
        const DolVMRegion* region = NULL;
        u32 region_index = 0;
        for (u32 r = 0; r < builder.region_count; r++) {
            if (target >= builder.regions[r].guest_start &&
                target < builder.regions[r].guest_end) {
                region = &builder.regions[r];
                region_index = r;
                break;
            }
        }
        if (!region || (target & 3u)) {
            // Nothing in this module covers it, so hand it back to the chassis.
            downgrade_call_to_exit(&builder, patch->code_index, target);
            continue;
        }
        const DolVMEntryPoint* point =
            &builder.map[region->map_index + (target - region->guest_start) / 4u];
        if (point->entry == DOLVM_NO_ENTRY) {
            downgrade_call_to_exit(&builder, patch->code_index, target);
            continue;
        }
        builder.code[patch->code_index].a = (u8)(region_index & 0xFFu);
        builder.code[patch->code_index].b = (u8)(region_index >> 8);
        builder.code[patch->code_index].imm =
            point->entry & DOLVM_ENTRY_OFFSET_MASK;
        u64 payload = ((u64)target << 32) | point->pc_base;
        memcpy(&builder.code[patch->code_index + 1u], &payload, sizeof(payload));
    }

    if (ok && !builder.failed)
        ok = serialize(&builder, options, image, size);
    else
        ok = false;

    for (u32 f = 0; f < module->function_count; f++)
        dolvm_layout_free(&layouts[f]);
    free(layouts);
    free(order);
    builder_free(&builder);
    return ok;
}

bool dolvm_write_module(const void* image, size_t size, const char* path,
                        FILE* diagnostics) {
    FILE* file = fopen(path, "wb");
    if (!file) {
        if (diagnostics)
            fprintf(diagnostics, "dolvm: cannot create %s\n", path);
        return false;
    }
    bool ok = fwrite(image, 1, size, file) == size;
    if (fclose(file) != 0)
        ok = false;
    if (!ok && diagnostics)
        fprintf(diagnostics, "dolvm: short write on %s\n", path);
    return ok;
}
