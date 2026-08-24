// SPDX-License-Identifier: GPL-3.0-or-later
//
// DolVM: the bytecode a DolRecomp module is lowered to when the host may not
// create executable pages at runtime.
//
// A native backend (C or LLVM) turns guest PowerPC into host machine code, and
// the host jumps into it. That is exactly the thing an App Store binary may not
// do: the store's review rules forbid a shipped app from generating or loading
// executable code. So the same DolIR is lowered here to *data* -- a register
// machine's instruction stream -- and the app interprets it. Nothing the
// recompiler produces is ever mapped executable.
//
// The register machine is not the guest, but it does hold part of the guest.
// Most of the register file is scratch -- the SSA temporaries of one DolIR
// block, never read across a dispatch boundary. The top of it is the guest's
// general purpose registers, LR, CTR and XER, which live there for the length
// of a dispatch and are read out of and written back to `CPUState` once at each
// end. Everything else architectural stays in `CPUState` and is read and
// written per access, which is what the state opcodes below do.
//
// Instructions are a fixed 8 bytes. Ops that need more than (a, b, c, imm)
// consume the following slot as a raw payload word; `dolvm_op_words` reports
// how many slots an op occupies so the verifier and disassembler can walk the
// stream without decoding it.

#ifndef DOLRECOMP_VM_DOLVM_H
#define DOLRECOMP_VM_DOLVM_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOLVM_MAGIC "DOLVM\0\0"
#define DOLVM_MAGIC_SIZE 8u
// Bumped when the *emitter* changes in a way that makes an older module worse
// rather than wrong -- a new fused form, a better layout. The ABI version says
// an interpreter cannot run a module; this says it can, but should not have to.
// The iOS library rebuilds a module whose version does not match, which is the
// only thing that gets an emitter improvement onto a device: the disc is
// recompiled there, and nothing else in the header would have changed.
#define DOLVM_VERSION 4u

// Bumped whenever the meaning of an existing opcode changes. The loader
// refuses a module it was not built to run rather than misinterpreting it.
// 4: CALL names the region its target lies in, and INDIRECT always names its
//    own; both are what a gated chassis checks before the interpreter resolves
//    the edge in place.
// 6: HLE stands in for a whole recognised SDK routine at its entry point.
#define DOLVM_ABI_VERSION 6u

// Register file size. Values are block-local, and the emitter recycles a
// register as soon as its last use retires, so real blocks land far below this;
// a block that would exceed it is split instead.
#define DOLVM_MAX_REGISTERS 256u

// Homed guest state.
//
// Most of the register file is scratch: block-local temporaries that are never
// read across a dispatch boundary. The top of it is the opposite. A home is one
// fixed register standing in for one guest state slot for the whole of a
// dispatch -- filled from CPUState on the way in, written back on the way out,
// and read and written directly by everything in between. The load and the
// store that used to bracket every use of a guest register stop being emitted
// at all, which on a real title is a quarter to a third of everything the
// interpreter executes.
//
// The layout is fixed by the ABI rather than declared per module because the
// interpreter names these registers in code compiled long before any module
// exists. A module says only whether it was lowered against them, since one
// that was not still expects CPUState to be authoritative mid-dispatch.
// Every general purpose register, not a chosen few. Narrowing the set to the
// ones the EABI makes hot -- r0-r7 and r24-r31, which carry three quarters of
// all register traffic on every title measured -- halves the fill and the flush
// and is 4-10% *slower* on all three. The copy is a vector run over contiguous
// memory with nothing depending on it, so it costs far less than its
// instruction count suggests, while every slot left out goes back to a load and
// a store per access in exactly the loops that mattered.
#define DOLVM_HOME_BASE 220u
#define DOLVM_HOME_GPR(n) (DOLVM_HOME_BASE + (u32)(n))
#define DOLVM_HOME_LR (DOLVM_HOME_BASE + 32u)
#define DOLVM_HOME_CTR (DOLVM_HOME_BASE + 33u)
// XER earns a home for the carry bit: `addic`, `addc` and the rest of the
// carrying arithmetic read it, insert into it and write it back, which was a
// load and a store of CPUState per instruction plus the store-to-load round
// trip between them. The condition-register opcodes read its summary-overflow
// bit here rather than out of CPUState.
#define DOLVM_HOME_XER (DOLVM_HOME_BASE + 34u)
#define DOLVM_HOME_COUNT 35u

// Registers the emitter may hand out for block-local values. The homes sit
// above this and the "no register" sentinel above them.
#define DOLVM_LOCAL_REGISTERS DOLVM_HOME_BASE

// Matches DOLRECOMP_C_LOOP_CYCLE_BUDGET: how far downcount may go negative
// inside one dispatch before a back edge returns to the chassis. A gate (below)
// may raise it, never lower it.
#define DOLVM_LOOP_CYCLE_BUDGET 256

// Backstop for loops whose blocks all cost zero cycles.
#define DOLVM_LOOP_STEP_BUDGET 2048

// Depth of the interpreter's own return-address stack, used only when a module
// opts into resolving intra-module calls without a chassis round trip.
#define DOLVM_CALL_STACK_DEPTH 64

#define DOLVM_NO_ENTRY 0xFFFFFFFFu
#define DOLVM_ENTRY_RETURN_TARGET 0x80000000u
#define DOLVM_ENTRY_OFFSET_MASK 0x7FFFFFFFu

typedef enum {
    // -- data movement ----------------------------------------------------
    DOLVM_OP_NOP,
    DOLVM_OP_MOV,          // r[a] = r[b]
    DOLVM_OP_CONST32,      // r[a] = imm
    DOLVM_OP_CONST64,      // r[a] = constants[imm]

    // -- guest state (imm is a byte offset into CPUState) ------------------
    DOLVM_OP_LOAD_STATE8,  // r[a] = *(u8*)(ctx + imm)
    DOLVM_OP_LOAD_STATE32,
    DOLVM_OP_LOAD_STATE64,
    DOLVM_OP_LOAD_STATEF,  // r[a].d = *(f64*)(ctx + imm)
    DOLVM_OP_STORE_STATE8, // *(u8*)(ctx + imm) = r[b]
    DOLVM_OP_STORE_STATE32,
    DOLVM_OP_STORE_STATE64,
    DOLVM_OP_STORE_STATEF,

    // -- integer arithmetic -----------------------------------------------
    // Values are held zero-extended to the register width, so the 32-bit forms
    // mask their result and the logical ops need no width at all.
    DOLVM_OP_ADD32,
    DOLVM_OP_ADD32I,       // r[a] = (r[b] + imm) & 0xFFFFFFFF
    DOLVM_OP_SUB32,
    DOLVM_OP_MUL32,
    DOLVM_OP_MUL32I,
    DOLVM_OP_UDIV32,
    DOLVM_OP_SDIV32,
    DOLVM_OP_ADD64,
    DOLVM_OP_SUB64,
    DOLVM_OP_MUL64,
    DOLVM_OP_UDIV64,
    DOLVM_OP_SDIV64,
    DOLVM_OP_AND,
    DOLVM_OP_ANDI,
    DOLVM_OP_OR,
    DOLVM_OP_ORI,
    DOLVM_OP_XOR,
    DOLVM_OP_XORI,
    DOLVM_OP_NOT,          // imm = width mask selector, see dolvm_width_mask
    DOLVM_OP_SHL32,
    DOLVM_OP_SHL32I,
    DOLVM_OP_LSHR32,
    DOLVM_OP_LSHR32I,
    DOLVM_OP_ASHR32,
    DOLVM_OP_ASHR32I,
    DOLVM_OP_SHL64,
    DOLVM_OP_SHL64I,
    DOLVM_OP_LSHR64,
    DOLVM_OP_LSHR64I,
    DOLVM_OP_ASHR64,
    DOLVM_OP_ASHR64I,
    DOLVM_OP_ROTL32,
    DOLVM_OP_ROTL32I,
    DOLVM_OP_CLZ32,
    DOLVM_OP_CLZ64,
    DOLVM_OP_BSWAP16,
    DOLVM_OP_BSWAP32,
    DOLVM_OP_BSWAP64,

    // -- width conversion -------------------------------------------------
    DOLVM_OP_TRUNC,        // r[a] = r[b] & dolvm_width_mask(imm)
    DOLVM_OP_SEXT,         // imm = source width selector | dest selector << 8

    // -- comparison (result is 0 or 1) ------------------------------------
    DOLVM_OP_ICMP_EQ,
    DOLVM_OP_ICMP_EQI,
    DOLVM_OP_ICMP_NE,
    DOLVM_OP_ICMP_NEI,
    DOLVM_OP_ICMP_ULT,
    DOLVM_OP_ICMP_ULTI,
    DOLVM_OP_ICMP_ULE,
    DOLVM_OP_ICMP_ULEI,
    DOLVM_OP_ICMP_SLT32,
    DOLVM_OP_ICMP_SLT32I,
    DOLVM_OP_ICMP_SLE32,
    DOLVM_OP_ICMP_SLE32I,
    DOLVM_OP_ICMP_SLT64,
    DOLVM_OP_ICMP_SLE64,
    DOLVM_OP_FCMP_OEQ,
    DOLVM_OP_FCMP_OLT,
    DOLVM_OP_FCMP_OGE,
    DOLVM_OP_SELECT,       // r[a] = r[b] ? r[c] : r[imm & 0xFF]

    // -- floating point (f64 in .d, f32 as a bit pattern in .u) ------------
    DOLVM_OP_FADD,
    DOLVM_OP_FSUB,
    DOLVM_OP_FMUL,
    DOLVM_OP_FDIV,
    DOLVM_OP_FNEG,
    DOLVM_OP_FABS,
    DOLVM_OP_FPTRUNC,      // r[a] = bits of (f32)r[b].d
    DOLVM_OP_FPEXT,        // r[a].d = (f64) float-from-bits r[b]

    // -- guest memory (imm is a signed displacement added to r[b]) ---------
    DOLVM_OP_LOAD8,
    DOLVM_OP_LOAD16,
    DOLVM_OP_LOAD32,
    DOLVM_OP_LOAD64,
    DOLVM_OP_LOAD8S,
    DOLVM_OP_LOAD16S,
    DOLVM_OP_LOAD32S,
    DOLVM_OP_STORE8,       // *(guest)(r[b] + imm) = r[c]
    DOLVM_OP_STORE16,
    DOLVM_OP_STORE32,
    DOLVM_OP_STORE64,

    // -- bookkeeping ------------------------------------------------------
    DOLVM_OP_CHARGE,       // ctx->downcount -= imm
    DOLVM_OP_PC_BASE,      // guest pc base for the block that follows
    DOLVM_OP_LOOP_GUARD,   // over budget: ctx->pc = imm, leave

    // -- control flow (imm is an instruction index into the code stream) ---
    DOLVM_OP_JMP,
    DOLVM_OP_JMP_IF,       // if (r[b]) goto imm
    DOLVM_OP_JMP_IFNOT,
    DOLVM_OP_EXIT,         // ctx->pc = imm, leave
    DOLVM_OP_EXIT_REG,     // ctx->pc = r[b], leave
    // a = 1 clears the low two bits of the target, which is what `blr` and
    // `bctr` do and what the builder otherwise writes out as its own mask.
    // imm = the region the branch was emitted in. Ungated, the interpreter
    // resolves only inside that region and only onto a return target; gated,
    // anywhere in the module the gate says the chassis would go itself.
    DOLVM_OP_INDIRECT,     // resolve r[b] locally when possible, else leave
    // Linked branch (or tail call) to an in-module address. imm = instruction
    // index, a | b << 8 = the target's region, payload = target pc << 32 |
    // the target's pc base. Ungated it leaves exactly as EXIT would.
    DOLVM_OP_CALL,
    DOLVM_OP_FALLBACK,     // payload word: raw instruction; imm = guest pc
    DOLVM_OP_SYSCALL,      // imm = guest pc
    DOLVM_OP_RFI,          // imm = guest pc

    // -- runtime helpers --------------------------------------------------
    DOLVM_OP_FP_AVAILABLE,   // imm = guest pc; leaves when FP is unavailable
    DOLVM_OP_EXACT_FLOAT,    // payload word: DolIRExactFloat descriptor
    DOLVM_OP_EXACT_PAIRED,   // payload word: DolIRExactPaired descriptor
    DOLVM_OP_PSQ_LOAD,       // payload word: descriptor; b = address register
    DOLVM_OP_PSQ_STORE,
    DOLVM_OP_STWCX,          // a = source gpr, b = address register, imm = pc
    DOLVM_OP_FPSCR_UPDATED,
    DOLVM_OP_FPSCR_BIT,      // imm = bit | set << 8
    DOLVM_OP_PROGRAM_EXC,    // payload word: cause; b = condition, imm = pc
    DOLVM_OP_SPR_READ,       // a = destination, imm = spr | pc in payload
    DOLVM_OP_SPR_WRITE,
    DOLVM_OP_LSWX,           // payload word: rD | rA << 8 | rB << 16
    DOLVM_OP_DCBZ_L,         // b = address register, imm = guest pc
    DOLVM_OP_ECIWX,          // a = destination, b = address, imm = pc
    DOLVM_OP_ECOWX,          // b = address, c = value, imm = pc
    DOLVM_OP_TLBIE,
    DOLVM_OP_CACHE_CONTROL,  // payload word: operation; b = address, imm = pc
    DOLVM_OP_FENCE,
    // Whole condition-register field update: compares r[b] against r[c] and
    // writes the field named by imm, summary-overflow bit included.
    DOLVM_OP_SET_CR_FIELD,   // imm = field | signed << 8

    // -- fused forms ------------------------------------------------------
    // Every one of these replaces a sequence the interpreter would otherwise
    // pay a dispatch apiece for. They exist because dispatch, not the work, is
    // what an interpreter spends its time on: the comparison in CMP_STATE is
    // the same comparison SET_CR_FIELD does, but it arrives without the two
    // state loads that used to feed it.
    //
    // Guarded jump: LOOP_GUARD followed by JMP, which is how every back edge
    // was emitted. Payload word holds the guest pc to leave at. a = 1 marks an
    // idle loop -- see JMP_IF_CR_GUARD.
    DOLVM_OP_JMP_GUARD,      // imm = instruction index
    // Branch on one condition-register bit, replacing the state load, the mask
    // and the compare the builder writes out for `bc`.
    DOLVM_OP_JMP_IF_CR,      // a = cr bit (0 = msb), b = sense, imm = index
    // Condition-register field update reading its operands out of CPUState
    // directly. Payload word: state offset in the high half, and either the
    // comparand (CMP_STATE_I) or the second state offset (CMP_STATE) in the low.
    DOLVM_OP_CMP_STATE_I,    // imm = field | signed << 8
    DOLVM_OP_CMP_STATE,      // imm = field | signed << 8
    // A guest load or store whose address base, and whose destination or
    // source, are CPUState slots rather than VM registers -- which is what
    // `lwz rD,off(rA)` and `stw rS,off(rA)` are, once the address add has been
    // folded into the displacement. Three dispatches become one.
    //
    // a = pc word, b = the plain load/store opcode this stands in for,
    // imm = displacement. Payload word: the destination or source slot offset
    // in the high half, the address base slot offset in the low. A load also
    // names its destination register in c, for the uses that follow it.
    DOLVM_OP_LOAD_MEM_STATE,
    DOLVM_OP_STORE_MEM_STATE,
    // The same, for the far more common case where the address base is already
    // in a register -- state forwarding hands one block's worth of stack and
    // base-pointer reads to every access that follows. Only the destination or
    // source slot folds in, so the payload word carries just that offset.
    //
    // a = pc word, b = the plain opcode, c = address register, imm = displacement.
    // A fused load writes the register as well as the slot, because the value
    // usually has later uses in the block -- state forwarding sees to that --
    // and requiring it not to would be requiring the fusion never to fire.
    DOLVM_OP_LOAD_MEM_TO_STATE,   // payload: slot offset << 32 | destination register
    DOLVM_OP_STORE_MEM_FROM_STATE,
    // Condition-register field update against a constant, which is what every
    // `cmpwi` and `cmplwi` is. Payload word: the comparand.
    DOLVM_OP_SET_CR_FIELDI,  // b = left register, imm = field | signed << 8
    // A branch that also pays the cycle charge its target block would have
    // begun with, and lands one instruction past it. Every block is entered
    // either by one of these or by a chassis dispatch through the entry map,
    // and the map still points at the charge, so the accounting is unchanged --
    // it is just no longer a dispatch of its own. Payload word: the charge, and
    // for JMP_GUARD the guest pc to leave at in its low half.
    DOLVM_OP_JMP_CHARGE,       // imm = instruction index
    DOLVM_OP_JMP_IF_CR_CHARGE, // a = cr bit, b = sense, imm = index
    // A loop's whole back edge: the condition-register test, the budget guard
    // and the target's cycle charge. `bne` back to the top of a loop was two
    // dispatches -- the test, then the guarded jump the taken edge fell into --
    // and a loop pays that on every iteration. Payload word: the charge in the
    // high half, the guest pc to leave at in the low.
    //
    // Bit 7 of b marks an idle loop: a block that polls one word of memory and
    // branches back to itself, which is a guest waiting for an interrupt or the
    // other processor to change that word. Taken, the edge charges the rest of
    // the slice and leaves at the loop head rather than polling through it,
    // which is what Dolphin's JITs do with the same three instructions.
    DOLVM_OP_JMP_IF_CR_GUARD,  // a = cr bit, b = sense | idle << 7, imm = index
    // The supervisor check every privileged instruction opens with: the MSR
    // read, the mask, the compare against zero and the conditional raise. The
    // operating system a game links against runs privileged instructions
    // constantly -- every interrupt mask is an `mfmsr` and an `mtmsr` -- and
    // written out longhand the four of them came to a tenth of everything the
    // interpreter executed on a real title.
    DOLVM_OP_SUPERVISOR,       // imm = guest pc; raises when MSR[PR] is set
    // A compare against a constant and the branch that reads the field it just
    // wrote. `cmpwi rX,n; bne` is the shape of every search loop and every
    // counted loop, and as two opcodes the branch had to read the condition
    // register back out of CPUState one instruction after the compare stored it
    // -- a store-to-load round trip on the loop's carried dependency. Fused,
    // the branch tests the bits it already has, and the field is still written
    // because the guest can read it later.
    //
    // a = left register, imm = instruction index,
    // b = field | signed << 3 | which bit of the field << 4 | sense << 6,
    // and for _GUARD the idle-loop mark in bit 7.
    // Payload word: the comparand, and the target's cycle charge in the high
    // half for the two forms that pay it. _GUARD takes a second payload word,
    // the guest pc to leave at.
    DOLVM_OP_CMP_JMP_IF_CR,
    DOLVM_OP_CMP_JMP_IF_CR_CHARGE,
    DOLVM_OP_CMP_JMP_IF_CR_GUARD,

    // A shift or rotate by a constant, and the mask that always follows it.
    // `rlwinm` is how PowerPC spells every field extraction there is, and the
    // builder writes it out as a rotate and an and -- two dispatches, with the
    // second reading back through the register file what the first just wrote.
    // One opcode does both, and the dependency between them stops existing.
    //
    // a = destination, b = source, c = shift amount, imm = mask. The source is
    // read as 32 bits and the result is zero-extended before the mask, exactly
    // as the pair was.
    DOLVM_OP_ROTL32I_AND,
    DOLVM_OP_SHL32I_AND,
    DOLVM_OP_LSHR32I_AND,
    DOLVM_OP_ASHR32I_AND,

    // A whole recognised SDK routine, executed natively. Every GameCube title
    // statically links the same SDK, and its hottest leaves -- the 32-byte
    // cache-line loops, the paired-single matrix kernels -- are exactly the
    // dense code an interpreter loses the most on. The recompiler proves a
    // function is the known routine by comparing every instruction word
    // against a stored pattern (a checksum only narrows the search), then
    // plants this at the routine's entry, after the block's charge. A helper
    // that cannot take the call in front of it -- unexpected mode bits, the
    // runtime switch off -- falls through to the interpreted body that still
    // follows, so the op is an optimisation, never a dependency. Mid-routine
    // entries (an exception resuming inside it) use the ordinary entry map
    // and never see this op at all.
    //
    // a = DolVMHleId, b | c << 8 = the region this op was emitted in (for
    // resolving the return the way INDIRECT would), imm = the routine's guest
    // entry pc. Payload word: the cycle charge the entry block already paid,
    // so the helper can charge its model minus what the CHARGE in front of it
    // took.
    DOLVM_OP_HLE,

    DOLVM_OP_COUNT
} DolVMOp;

// Native stand-ins for SDK routines the recompiler can prove are present.
// The pattern that proves each one lives with the matcher
// (backend/vm/dolvm_hle_match.c); the execution lives with the interpreter
// (vm/dolvm_interp.c). Both are keyed by this id, which is baked into
// modules -- append only.
// The SDK's cache-range family: (start r3, length r4), one cache-block op per
// 32 bytes, clobbering r3, r4, r5, CTR and CR0 exactly as the loop does. Two
// SDK generations compile the alignment differently -- the newer computes
// blocks as (length + (start & 31) + 31) >> 5, the older tests the alignment
// with a record-form clrlwi (which is what CR0 ends up holding) and rounds a
// misaligned start up by a whole extra block -- and three endings exist: the
// SDK's `sc` fast-sync, a plain blr, and icbi's sync; isync; blr.
typedef enum {
    DOLVM_HLE_NONE = 0,
    DOLVM_HLE_DC_FLUSH_RANGE = 1,           // dcbf,  sc
    DOLVM_HLE_DC_INVALIDATE_RANGE = 2,      // dcbi,  blr
    DOLVM_HLE_DC_STORE_RANGE = 3,           // dcbst, sc
    DOLVM_HLE_DC_FLUSH_RANGE_NO_SYNC = 4,   // dcbf,  blr
    DOLVM_HLE_DC_STORE_RANGE_NO_SYNC = 5,   // dcbst, blr
    DOLVM_HLE_IC_INVALIDATE_RANGE = 6,      // icbi,  sync; isync; blr
    DOLVM_HLE_DC_FLUSH_RANGE_OLD = 7,       // the older alignment math
    DOLVM_HLE_DC_INVALIDATE_RANGE_OLD = 8,
    DOLVM_HLE_DC_STORE_RANGE_OLD = 9,
    DOLVM_HLE_DC_FLUSH_RANGE_NO_SYNC_OLD = 10,
    DOLVM_HLE_IC_INVALIDATE_RANGE_OLD = 11,
    // Ids from here up are not interpreter-implemented helpers but whole SDK
    // routines the C backend compiled at build time (dolvm_hle_native.c,
    // regenerated by tools/dolvm_hle_gen.c). Their patterns pin the routine's
    // link address, because the generated code bakes it into its exception
    // paths; the same bytes linked elsewhere stay interpreted until that
    // variant is added.
    DOLVM_HLE_NATIVE_FIRST = 12,
    // Retired: the Y-component Huffman decoder alone, superseded by the whole
    // cluster below. The number stays reserved because modules bake ids in.
    DOLVM_HLE_THP_HUFF_Y_GALE = 12,
    // Melee's whole THP video decoder -- the frame/IDCT driver and the three
    // component Huffman decoders it calls, lowered as one chunk so the calls
    // between them stay native. With the reverb kernel below, 96% of every
    // opcode the heaviest scene interprets.
    DOLVM_HLE_THP_DECODE_GALE = 13,
    DOLVM_HLE_AX_REVERB_GALE = 14,
    // 15 and 16 were Disney skate's paired-single matrix kernels, measured
    // at nothing and removed: a native routine that spends itself in the
    // exact-float helpers runs at the interpreter's speed, because the
    // interpreter's cost there was never the interpreting. The numbers stay
    // reserved.
    // Metroid Prime 2's Metrowerks soft 64-bit divide (__div2i + __div2u as
    // one cluster). Retro's boot converts time through it three million
    // times a second -- 43% of every interpreted opcode.
    DOLVM_HLE_DIV64_G2ME = 17,
    // Metroid Prime 2's THP decoder, Retro's own compile of it -- the intro
    // and menu movies play through this at 0.58x interpreted on an M4 Pro.
    DOLVM_HLE_THP_DECODE_G2ME = 18,
    DOLVM_HLE_COUNT
} DolVMHleId;

// The native stand-ins, indexed by id; NULL where the id is an interpreter
// helper. Defined in the generated dolvm_hle_native.c.
struct CPUState;
extern void (*const g_dolvm_hle_native[DOLVM_HLE_COUNT])(struct CPUState*);

// Width selectors for TRUNC / SEXT / NOT.
typedef enum {
    DOLVM_WIDTH_1 = 0,
    DOLVM_WIDTH_8 = 1,
    DOLVM_WIDTH_16 = 2,
    DOLVM_WIDTH_32 = 3,
    DOLVM_WIDTH_64 = 4,
} DolVMWidth;

static inline u64 dolvm_width_mask(u32 width) {
    static const u64 masks[5] = {
        0x1ull, 0xFFull, 0xFFFFull, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull
    };
    return masks[width & 7u];
}

static inline u32 dolvm_width_bits(u32 width) {
    static const u32 bits[5] = {1u, 8u, 16u, 32u, 64u};
    return bits[width & 7u];
}

typedef struct {
    u8 op;
    u8 a;
    u8 b;
    u8 c;
    u32 imm;
} DolVMInst;

// Guest address ranges covered by the module, in ascending order. `map_index`
// is where this region's per-instruction entry offsets start in the entry map.
// One per guest instruction in a region. `entry` is the bytecode index to start
// at, with DOLVM_ENTRY_RETURN_TARGET set when an indirect branch may land here;
// `pc_base` is the guest address the enclosing block's memory ops count their
// own pc from, so entering mid-block still names the right faulting address.
typedef struct {
    u32 entry;
    u32 pc_base;
} DolVMEntryPoint;

typedef struct {
    u32 guest_start;
    u32 guest_end;
    u32 map_index;
    u32 flags;
} DolVMRegion;

typedef enum {
    // Linked branches to in-module addresses were lowered to CALL rather than
    // to EXIT. Whether the interpreter follows them is the gate's decision: a
    // chassis that installs none gets a leave at every one, exactly as if they
    // were EXITs, because resolving in place bypasses whatever that chassis
    // checks on dispatch (host-call interception, self-modifying-code
    // retirement) and only the gate can say those checks would pass.
    DOLVM_FLAG_DIRECT_CALLS = 1u << 0,
    // Lowered against the homed registers above: the general purpose registers,
    // LR and CTR live in the register file for the length of a dispatch, and
    // CPUState's copies of them are stale until it ends. An interpreter must
    // fill and flush them; a module without this flag expects neither.
    DOLVM_FLAG_HOMED_STATE = 1u << 1,
} DolVMModuleFlags;

// A guest address range, end-exclusive. Same shape as the chassis module ABI's
// range type, so the loader can hand these straight over.
typedef struct {
    u32 start;
    u32 end;
} DolVMRange;

typedef struct {
    char magic[DOLVM_MAGIC_SIZE];
    u32 version;
    u32 abi_version;
    u32 cpu_state_size;
    u32 flags;
    u32 entry_point;
    u32 code_count;
    u32 constant_count;
    u32 region_count;
    u32 map_count;
    // The rest is what a chassis needs to accept this as a recompiled module
    // and to police it the way it polices a native one. A host that only wants
    // to interpret can ignore all of it.
    char game_id[8];
    u32 smc_count;
    // dolvm_state_layout_hash() as the emitter saw CPUState. A host with a
    // different value cannot run this module's baked state offsets.
    u32 state_layout_hash;
    u32 reserved[1];
} DolVMHeader;

// What a chassis knows that lets the interpreter follow a control transfer in
// place instead of returning: which regions it would dispatch into itself, how
// long it is prepared to wait, and which exceptions it is holding.
//
// Every field is a pointer into chassis-owned memory, read on the interpreter's
// thread; the chassis writes them between dispatches (the per-region bytes and
// the budget) or from hooks the interpreter itself calls (the pending word).
// Installed on the module with `dolvm_module_set_gate`; with none installed the
// interpreter resolves nothing beyond its own region and no calls at all.
typedef struct {
    // One byte per region, parallel to DolVMModule::regions: nonzero while the
    // chassis would itself dispatch into that region -- verified against guest
    // RAM, not retired to a fallback interpreter, no host call inside it.
    const u8* region_open;
    // Cycles a dispatch may still charge before a loop back edge or a
    // resolved edge has to return: the chassis's live slice counter. Read at
    // every guard rather than once on entry, because the chassis shortens it
    // from inside the hooks the interpreter calls when one of them schedules
    // an event due before the slice was going to end.
    const s32* budget;
    // A word the chassis raises exceptions in, and the bits that mean "come
    // back now": the synchronous set unconditionally, the asynchronous set
    // only while MSR[EE] is set. Tested at every resolved edge, so an
    // interrupt a guest store just raised is delivered within the function
    // that raised it, not at the end of the slice.
    const u32* pending;
    u32 pending_sync;
    u32 pending_async;
} DolVMGate;

// A loaded module. `dolvm_module_open` points these at the file image without
// copying, so the image must outlive the module.
typedef struct {
    const DolVMHeader* header;
    const DolVMInst* code;
    const u64* constants;
    const DolVMRegion* regions;
    const DolVMEntryPoint* map;
    // FNV-1a 64 of each region's original guest text, in region order. The
    // chassis checks guest RAM against these before trusting a region, which is
    // how self-modifying code stops being silently wrong.
    const u64* region_hashes;
    const DolVMRange* smc_ranges;
    u32 code_count;
    u32 constant_count;
    u32 region_count;
    u32 map_count;
    u32 smc_count;
    u32 flags;
    void* image;        // owned when loaded from a file, else NULL
    size_t image_size;
    // Region lookup by address in O(1): `lookup[(address - lookup_base) >>
    // DOLVM_LOOKUP_SHIFT]` is the first region ending past that bucket, and a
    // bucket holds at most a couple of regions, so the walk from there is a
    // compare or two. Built at open; NULL when the span is too large to table,
    // in which case the binary search over `regions` stands in.
    u32* lookup;
    u32 lookup_base;
    u32 lookup_span;
    const DolVMGate* gate;
} DolVMModule;

#define DOLVM_LOOKUP_SHIFT 14u
// Buckets the lookup table is allowed to reach: 1M entries is 4 MB, which
// covers 16 GB of guest span -- more than any module describes.
#define DOLVM_LOOKUP_MAX_BUCKETS (1u << 20)

// How many 8-byte slots an op occupies, payload words included.
u32 dolvm_op_words(u32 op);
const char* dolvm_op_name(u32 op);

// Container I/O. `dolvm_module_open` validates the header and every branch
// target, entry offset and register index before returning true, so the
// interpreter can run without re-checking anything on the hot path.
bool dolvm_module_open(DolVMModule* module, const void* image, size_t size,
                       char* error, size_t error_size);
bool dolvm_module_load_file(DolVMModule* module, const char* path,
                            char* error, size_t error_size);
void dolvm_module_close(DolVMModule* module);

// Install (or with NULL, remove) the chassis's gate. The gate and everything it
// points at must outlive every dispatch that runs while it is installed.
void dolvm_module_set_gate(DolVMModule* module, const DolVMGate* gate);

// Resolve a guest address to its entry, or NULL when the module misses it.
//
// Inline because the interpreter does this once per dispatch and a dispatch is
// only about thirty guest instructions long; the out-of-line forms stay for
// everything that is not on that path.
static inline const DolVMRegion* dolvm_module_region_inline(
    const DolVMModule* module, u32 address) {
    if (module->lookup) {
        u32 offset = address - module->lookup_base;
        if (offset >= module->lookup_span)
            return 0;
        u32 index = module->lookup[offset >> DOLVM_LOOKUP_SHIFT];
        const DolVMRegion* region = &module->regions[index];
        const DolVMRegion* end = &module->regions[module->region_count];
        // Regions are sorted and disjoint, so the walk stops at the first one
        // starting past the address.
        for (; region < end && address >= region->guest_start; region++) {
            if (address < region->guest_end)
                return region;
        }
        return 0;
    }
    u32 low = 0;
    u32 high = module->region_count;
    while (low < high) {
        u32 middle = low + (high - low) / 2u;
        const DolVMRegion* region = &module->regions[middle];
        if (address < region->guest_start)
            high = middle;
        else if (address >= region->guest_end)
            low = middle + 1u;
        else
            return region;
    }
    return 0;
}

static inline const DolVMEntryPoint* dolvm_module_entry_inline(
    const DolVMModule* module, u32 address) {
    if (address & 3u)
        return 0;
    const DolVMRegion* region = dolvm_module_region_inline(module, address);
    if (!region)
        return 0;
    u32 index = region->map_index + (address - region->guest_start) / 4u;
    const DolVMEntryPoint* entry = &module->map[index];
    return entry->entry == DOLVM_NO_ENTRY ? 0 : entry;
}

const DolVMEntryPoint* dolvm_module_entry(const DolVMModule* module,
                                          u32 address);
const DolVMRegion* dolvm_module_region(const DolVMModule* module, u32 address);

#ifdef __cplusplus
}
#endif

#endif
