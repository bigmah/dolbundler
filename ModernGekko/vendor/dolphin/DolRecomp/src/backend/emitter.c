// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ExpansionPak

#include "emitter.h"
#include "backend/c_cfg.h"

#include <stdlib.h>
#include <string.h>

static u32 cr_field_shift(u8 crf) {
    return 4u * (7u - (u32)crf);
}

static u32 ppc_mask32(u8 mb, u8 me) {
    u32 mask = 0;
    u8 bit = mb;

    for (;;) {
        mask |= 0x80000000u >> bit;
        if (bit == me)
            break;
        bit = (u8)((bit + 1) & 31);
    }

    return mask;
}

static void emit_set_cr0_from_gpr(FILE* out, u8 reg) {
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        s32 cr_value = (s32)ctx->gpr[%u];\n", reg);
    fprintf(out, "        if (cr_value < 0)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (cr_value > 0)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (cr_value == 0) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (cr_bits << 28);\n");
}

static void emit_set_cr1_from_fpscr(FILE* out) {
    fprintf(out, "        ctx->cr = (ctx->cr & 0xF0FFFFFFu) | ((ctx->fpscr >> 4) & 0x0F000000u);\n");
}

static void emit_compare_s32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    u32 shift = cr_field_shift(crf);

    fprintf(out, "    {\n");
    fprintf(out, "        s32 val_a = (s32)(%s);\n", lhs);
    fprintf(out, "        s32 val_b = (s32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
            shift, shift);
    fprintf(out, "    }\n");
}

static void emit_compare_u32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    u32 shift = cr_field_shift(crf);

    fprintf(out, "    {\n");
    fprintf(out, "        u32 val_a = (u32)(%s);\n", lhs);
    fprintf(out, "        u32 val_b = (u32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
            shift, shift);
    fprintf(out, "    }\n");
}

static void emit_ps_merge(FILE* out, const PPCInst* inst,
                          bool use_a_ps1, bool use_b_ps1) {
    const char* a_bank = use_a_ps1 ? "ps1" : "fpr";
    const char* b_bank = use_b_ps1 ? "ps1" : "fpr";

    fprintf(out, "    {\n");
    fprintf(out, "        f64 ps0 = ctx->%s[%u];\n",
            a_bank, inst->rA);
    fprintf(out, "        f64 ps1 = ctx->%s[%u];\n",
            b_bank, inst->rB);
    fprintf(out, "        ctx->fpr[%u] = ps0;\n", inst->rD);
    fprintf(out, "        ctx->ps1[%u] = ps1;\n", inst->rD);
    fprintf(out, "    }\n");
}

static void emit_fcompare(FILE* out, const PPCInst* inst) {
    fprintf(out, "    ctx->cr = ppc_fcmp_cr(ctx, ctx->cr, %u, ctx->fpr[%u], ctx->fpr[%u], %s);\n",
            inst->crfD, inst->rA, inst->rB,
            inst->op == PPC_OP_FCMPO ? "true" : "false");
}

static void emit_dform_ea(FILE* out, u8 ra, s16 simm, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "(u32)(s32)(%d)", (int)simm);
    } else {
        fprintf(out, "ctx->gpr[%u] + (u32)(s32)(%d)", ra, (int)simm);
    }
}

static void emit_xform_ea(FILE* out, u8 ra, u8 rb, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "ctx->gpr[%u]", rb);
    } else {
        fprintf(out, "ctx->gpr[%u] + ctx->gpr[%u]", ra, rb);
    }
}

static void emit_load(FILE* out, const PPCInst* inst, const char* read_expr,
                      bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        ctx->gpr[%u] = %s;\n", inst->rD, read_expr);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_loadx(FILE* out, const PPCInst* inst, const char* read_expr,
                       bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        ctx->gpr[%u] = %s;\n", inst->rD, read_expr);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_store(FILE* out, const PPCInst* inst, const char* write_func,
                       const char* cast_type, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, ea, (%s)ctx->gpr[%u]);\n",
            write_func, cast_type, inst->rS);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_storex(FILE* out, const PPCInst* inst, const char* write_func,
                        const char* cast_type, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, ea, (%s)ctx->gpr[%u]);\n",
            write_func, cast_type, inst->rS);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fload(FILE* out, const PPCInst* inst, bool single,
                       bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        f64 value = dolrecomp_f32_from_bits(mem_read32(ctx, ea));\n");
        fprintf(out, "        ctx->fpr[%u] = value;\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = value;\n", inst->rD);
    } else {
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_f64_from_bits(mem_read64(ctx, ea));\n",
                inst->rD);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_floadx(FILE* out, const PPCInst* inst, bool single,
                        bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        f64 value = dolrecomp_f32_from_bits(mem_read32(ctx, ea));\n");
        fprintf(out, "        ctx->fpr[%u] = value;\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = value;\n", inst->rD);
    } else {
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_f64_from_bits(mem_read64(ctx, ea));\n",
                inst->rD);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fstore(FILE* out, const PPCInst* inst, bool single,
                        bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        mem_write32(ctx, ea, dolrecomp_f32_to_bits(ctx->fpr[%u]));\n",
                inst->rS);
    } else {
        fprintf(out, "        mem_write64(ctx, ea, dolrecomp_f64_to_bits(ctx->fpr[%u]));\n",
                inst->rS);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fstorex(FILE* out, const PPCInst* inst, bool single,
                         bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        mem_write32(ctx, ea, dolrecomp_f32_to_bits(ctx->fpr[%u]));\n",
                inst->rS);
    } else {
        fprintf(out, "        mem_write64(ctx, ea, dolrecomp_f64_to_bits(ctx->fpr[%u]));\n",
                inst->rS);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

// Defined with the other emission knobs below; the quantised load/store
// templates are the one place above them that needs them.
static bool g_homed;
static bool g_homed_fpr;

static void emit_psq_load(FILE* out, const PPCInst* inst, bool indexed,
                          bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    if (g_homed && g_homed_fpr) {
        // The unquantised fast path by value; anything else is the
        // interpreter's, which writes the register file by index, so the
        // locals are spilled around it and reloaded after.
        fprintf(out, "        f64 q0_, q1_;\n");
        fprintf(out, "        if (hpsq_load_fast(ctx, ram_, ram_size_, cia_, ea, %s, %uu, %s, &q0_, &q1_)) {"
                     " ctx->fpr[%u] = q0_; ctx->ps1[%u] = q1_; }\n",
                inst->w ? "true" : "false", inst->i, indexed ? "true" : "false",
                inst->rD, inst->rD);
        fprintf(out, "        else { DOLRECOMP_SPILL(); ppc_psq_load(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);"
                     " DOLRECOMP_RELOAD(); if (ctx->exception) return; }\n",
                inst->rD, inst->w ? "true" : "false", inst->i,
                indexed ? "true" : "false", inst->address);
    } else {
        fprintf(out, "        ppc_psq_load_inline(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);\n",
                inst->rD, inst->w ? "true" : "false", inst->i,
                indexed ? "true" : "false", inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_psq_store(FILE* out, const PPCInst* inst, bool indexed,
                           bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    if (g_homed && g_homed_fpr) {
        fprintf(out, "        if (!hpsq_store_fast(ctx, ram_, ram_size_, cia_, ea, %s, %uu, %s, ctx->fpr[%u], ctx->ps1[%u]))"
                     " { DOLRECOMP_SPILL(); ppc_psq_store(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);"
                     " if (ctx->exception) return; }\n",
                inst->w ? "true" : "false", inst->i, indexed ? "true" : "false",
                inst->rS, inst->rS,
                inst->rS, inst->w ? "true" : "false", inst->i,
                indexed ? "true" : "false", inst->address);
    } else {
        fprintf(out, "        ppc_psq_store_inline(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);\n",
                inst->rS, inst->w ? "true" : "false", inst->i,
                indexed ? "true" : "false", inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_dcbz(FILE* out, const PPCInst* inst) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, false);
    fprintf(out, ";\n");
    fprintf(out, "        ea &= ~31u;\n");
    fprintf(out, "        for (u32 i = 0; i < 32; i += 4) mem_write32(ctx, ea + i, 0);\n");
    fprintf(out, "    }\n");
}

static void emit_branch_condition(FILE* out, u8 bo, u8 bi) {
    bool ctr_ignored = (bo & 0x04) != 0;
    bool cond_ignored = (bo & 0x10) != 0;

    if (!ctr_ignored) {
        fprintf(out, "        ctx->ctr--;\n");
        fprintf(out, "        bool ctr_ok = (((ctx->ctr != 0) ? 1u : 0u) ^ %uu) != 0;\n",
                (bo >> 1) & 1u);
    } else {
        fprintf(out, "        bool ctr_ok = true;\n");
    }

    if (!cond_ignored) {
        u32 mask = 0x80000000u >> bi;
        fprintf(out, "        bool cr_ok = (((ctx->cr & 0x%08Xu) != 0) == %s);\n",
                mask, ((bo >> 3) & 1u) ? "true" : "false");
    } else {
        fprintf(out, "        bool cr_ok = true;\n");
    }
}

static bool branch_target_is_local(u32 func_start, u32 func_end, u32 target) {
    return target >= func_start && target < func_end && ((target - func_start) & 3u) == 0;
}

// Chunk entry addresses, sorted. Written once before the worker pool starts and
// read-only thereafter, so no locking. NULL means "no table": every cross-chunk
// branch takes the safe return-to-chassis path.
static const u32* g_chunk_starts = NULL;
static const u32* g_chunk_ends = NULL;
static u32 g_chunk_count = 0;

// Whether a direct call asks the chassis's gate first. Off means the old
// opt-in benchmark mode, which skips the SMC guard.
static bool g_gated_calls;

void emit_set_chunk_table(const u32* starts, const u32* ends, u32 count, bool gated) {
    g_chunk_starts = count ? starts : NULL;
    g_chunk_ends = count ? ends : NULL;
    g_chunk_count = count ? count : 0;
    g_gated_calls = gated;
}

// Cross-chunk *tail* calls. emit_cross_chunk_call only fires for `bl`, which has
// a continuation to resume into; a plain `b` to another chunk had nothing to
// resume and so always went back through the chassis. Running the target as a
// host call and then returning still saves the chassis one dispatch, and the
// depth ceiling bounds the recursion a chain of them creates.
static bool g_tail_calls;

void emit_set_tail_calls(bool enabled) {
    g_tail_calls = enabled;
}

static bool g_hle_outcalls;

void emit_set_hle_outcalls(bool enabled) {
    g_hle_outcalls = enabled;
}

// Homed registers. The generated code's register file is ctx->gpr[] in memory,
// and on wasm every read of it is an i32.load from linear memory and every
// write an i32.store -- and because the guest's own stores go through a u8*
// that may alias anything, and every memory access has a cold path that calls
// a hook, the C compiler cannot keep a register in a machine register across
// two guest instructions. Measured on a 64-instruction Disney skate chunk: 679
// loads and 170 stores for 64 guest instructions.
//
// With this on, each generated function keeps the GPRs it touches, plus CR,
// XER, LR and CTR, in C locals (r_3, cr_ ...), loads them from ctx at entry and
// writes them back at every exit; around every call that can read or write
// guest registers -- a chunk call, a tail call, the interpreter fallback, an
// HLE host call, lmw/stmw -- it spills before and reloads after. Guest memory
// goes through hmem_* helpers that take the RAM base and size as locals hoisted
// once per function, so they are never re-read either, and the helpers' cold
// path materialises ctx->pc from the instruction address it is handed, which
// is why the per-instruction `ctx->pc = ...` stores can go: nothing else
// observes the pc between transfers. The rewrite is textual over the emitted
// body (see homed_rewrite), so the instruction templates are untouched and the
// legacy output is byte-identical with the knob off.
static bool g_homed;
// Whether the FPRs are homed too (DOLRECOMP_HOMED_FPR, default on). Off, the
// floating-point templates are the index-based ones and fpr[]/ps1[] stay in
// CPUState while the integer registers are still locals.
static bool g_homed_fpr = true;
// DOLRECOMP_HOMED_VERIFY=1: every per-site spill also checks that each homed
// register the analysis left out really does equal its memory copy, and
// reports the first mismatch per site through dolrecomp_homed_mismatch().
// It turns a hole in the reach analysis from silent corruption into a line
// naming the chunk, the instruction and the register.
static bool g_homed_verify;

void emit_set_homed_verify(bool enabled) {
    g_homed_verify = enabled;
}

void emit_set_homed_registers(bool enabled) {
    g_homed = enabled;
}

void emit_set_homed_fpr(bool enabled) {
    g_homed_fpr = enabled;
}

// Chunked emission puts many guest functions in one C function, so a `bl` to a
// neighbour becomes a plain `goto` and never passes the chassis dispatcher --
// which is where a host installs its SDK intercepts (ppc_host_call). A runtime
// that HLEs SDK functions at their guest addresses therefore misses every call
// that happens to land inside the caller's own chunk. This flag restores the
// intercept point without giving up the goto: check the host first, and fall
// through to the goto when it declines, which is the common case.
static bool g_hle_local_calls;

void emit_set_hle_local_calls(bool enabled) {
    g_hle_local_calls = enabled;
}

// The index of the chunk whose func_<start>() covers `addr`, or -1 if none
// does. Chunks tile the text sections but the first one does not start on the
// common stride, so this binary-searches rather than dividing -- which is also
// why the caller must hand this table in sorted, and why the index it returns
// is the chassis's chunk index: gen_module_tables.py builds chunk_ranges from
// the sorted func_ declarations, and the gate's chunk_open runs parallel to it.
static int chunk_index_for(u32 addr) {
    if (!g_chunk_starts || !g_chunk_count)
        return -1;
    u32 lo = 0, hi = g_chunk_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (g_chunk_starts[mid] <= addr)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (!lo)
        return -1;
    // The chunk found is the last one starting at or below addr; it only
    // covers addr if addr is inside it. An address past the end of a section
    // -- the fallthrough off its last chunk -- belongs to nothing.
    if (g_chunk_ends && addr >= g_chunk_ends[lo - 1u])
        return -1;
    return (int)(lo - 1u);
}


// A cross-chunk `bl` whose target chunk is known: call it directly instead of
// returning to the chassis. The chassis round trip costs two rel-section scans,
// two IsHostCallAddress hash lookups, a ModManager dispatch and a downcount
// flush, none of which a same-module call needs. The SDK's leaf functions make
// this worth having on its own: OSDisableInterrupts is five instructions, and a
// third of GEXE52's dispatches were the round trips into and out of that shape.
//
// What makes it *safe* rather than merely fast is the gate.
// dolrecomp_native_gate_allows() asks the chassis the same three questions it
// would have asked itself before dispatching -- is that chunk still verified
// against guest RAM and unhooked, is there budget left in the timing slice, is
// an exception pending -- and any "no" takes the plain return. Without it a
// direct call skips the chassis's SMC guard, which is what "unsafe" in
// DOLRECOMP_UNSAFE_DIRECT_CALLS refers to.
//
// Resume inline only if the callee came back to the instruction after the call.
// Any other pc means it stopped early -- budget exhausted, an exception, a
// tail-call elsewhere -- and only the chassis knows what to do next.
//
// The prototypes are declared at block scope so this needs no header plumbing;
// the definitions live in other translation units and the linker resolves them.
static bool emit_cross_chunk_call(FILE* out, const PPCInst* inst,
                                  u32 func_start, u32 func_end) {
    u32 continuation = inst->address + 4u;
    int target_index = chunk_index_for(inst->branch_target);
    if (target_index < 0)
        return false;
    u32 target_chunk = g_chunk_starts[target_index];
    // Without a local continuation label there is nothing to resume into, so
    // the call would buy nothing over the plain return.
    if (!branch_target_is_local(func_start, func_end, continuation))
        return false;

    fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
    if (g_gated_calls) {
        fprintf(out, "            bool dolrecomp_native_gate_allows(CPUState* ctx, u32 chunk_index);\n");
        fprintf(out, "            if (dolrecomp_native_gate_allows(ctx, %uu) && dolrecomp_call_enter()) {\n",
                (unsigned)target_index);
    } else {
        fprintf(out, "            if (dolrecomp_call_enter()) {\n");
    }
    fprintf(out, "                void func_%08X(CPUState* ctx);\n", target_chunk);
    fprintf(out, "                func_%08X(ctx);\n", target_chunk);
    fprintf(out, "                dolrecomp_call_leave();\n");
    fprintf(out, "                if (ctx->pc == 0x%08Xu) goto label_%08X;\n",
            continuation, continuation);
    fprintf(out, "            }\n");
    fprintf(out, "            return;\n");
    return true;
}

// A cross-chunk branch with no link and no local continuation. Unlike
// emit_cross_chunk_call there is nothing to `goto` afterwards -- whatever the
// target leaves in ctx->pc is the next thing to run -- so this returns either
// way. The win is one fewer chassis dispatch per tail call; the gate keeps the
// SMC guard exact, and dolrecomp_call_enter keeps a chain of tail calls from
// running the host stack out.
static bool emit_cross_chunk_tail_to(FILE* out, u32 target) {
    if (!g_tail_calls || !g_chunk_starts)
        return false;
    int target_index = chunk_index_for(target);
    if (target_index < 0)
        return false;
    u32 target_chunk = g_chunk_starts[target_index];

    fprintf(out, "            ctx->pc = 0x%08Xu;\n", target);
    if (g_gated_calls) {
        fprintf(out, "            bool dolrecomp_native_gate_allows(CPUState* ctx, u32 chunk_index);\n");
        fprintf(out, "            if (dolrecomp_native_gate_allows(ctx, %uu)) {\n",
                (unsigned)target_index);
    } else {
        fprintf(out, "            {\n");
    }
    fprintf(out, "                void func_%08X(CPUState* ctx);\n", target_chunk);
    fprintf(out, "                DOLRECOMP_TAIL_CALL(func_%08X(ctx));\n", target_chunk);
    fprintf(out, "            }\n");
    fprintf(out, "            return;\n");
    return true;
}

// An indirect transfer -- bctr, bctrl -- resolved in place. The target is only
// known at run time, so the module's own table answers which chunk covers it,
// and the gate is asked about that chunk exactly as for a direct call. A call
// resumes inline if the callee came back to the instruction after it; a jump
// is a tail call. Anything the table does not cover, or the gate refuses, takes
// the plain return and the chassis dispatches as before.
static bool emit_indirect_transfer(FILE* out, const PPCInst* inst,
                                   u32 func_start, u32 func_end) {
    if (!g_chunk_starts)
        return false;
    u32 continuation = inst->address + 4u;
    if (inst->lk && !branch_target_is_local(func_start, func_end, continuation))
        return false;
    if (!inst->lk && !g_tail_calls)
        return false;

    fprintf(out, "            {\n");
    fprintf(out, "                int chunk = dolrecomp_chunk_index_of(target);\n");
    if (g_gated_calls) {
        fprintf(out, "                bool dolrecomp_native_gate_allows(CPUState* ctx, u32 chunk_index);\n");
        fprintf(out, "                if (chunk >= 0 && dolrecomp_native_gate_allows(ctx, (u32)chunk)) {\n");
    } else {
        fprintf(out, "                if (chunk >= 0) {\n");
    }
    if (inst->lk) {
        fprintf(out, "                    if (dolrecomp_call_enter()) {\n");
        fprintf(out, "                        dolrecomp_chunk_table[chunk](ctx);\n");
        fprintf(out, "                        dolrecomp_call_leave();\n");
        fprintf(out, "                        if (ctx->pc == 0x%08Xu) goto label_%08X;\n",
                continuation, continuation);
        fprintf(out, "                    }\n");
    } else {
        fprintf(out, "                    DOLRECOMP_TAIL_CALL(dolrecomp_chunk_table[chunk](ctx));\n");
    }
    fprintf(out, "                }\n");
    fprintf(out, "            }\n");
    return true;
}

// The loop guard. A backward branch charges its block and compares the chunk's
// accumulator against a fixed budget; past it, the loop yields so the chassis
// can deliver events and rescue a spin. With a gate the yield is not needed for
// that -- the guard can flush through the gate and read the live slice instead,
// and only return when the slice really is spent or an exception is pending.
// Without one it returns as it always did.
static void emit_loop_guard(FILE* out, const char* indent, u32 resume_pc,
                            bool resume_is_return_dispatch) {
    fprintf(out, "%sif (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) {\n", indent);
    if (g_gated_calls) {
        if (g_homed && !resume_is_return_dispatch)
            fprintf(out, "%s    ctx->pc = 0x%08Xu;\n", indent, resume_pc);
        fprintf(out, "%s    bool dolrecomp_loop_guard_continue(CPUState* ctx);\n", indent);
        fprintf(out, "%s    if (!dolrecomp_loop_guard_continue(ctx)) {\n", indent);
        if (!resume_is_return_dispatch)
            fprintf(out, "%s        ctx->pc = 0x%08Xu;\n", indent, resume_pc);
        fprintf(out, "%s        return;\n", indent);
        fprintf(out, "%s    }\n", indent);
    } else {
        if (!resume_is_return_dispatch)
            fprintf(out, "%s    ctx->pc = 0x%08Xu;\n", indent, resume_pc);
        fprintf(out, "%s    return;\n", indent);
    }
    fprintf(out, "%s}\n", indent);
}

static void emit_direct_branch(FILE* out, const PPCInst* inst,
                               bool local_target, bool direct_backedge,
                               u32 func_start, u32 func_end) {
    bool local_backward = local_target && inst->branch_target <= inst->address;

    if (inst->lk) {
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
        if (local_target) {
            if (g_hle_local_calls &&
                branch_target_is_local(func_start, func_end, inst->address + 4u)) {
                if (g_homed) {
                    fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->address);
                    fprintf(out, "            DOLRECOMP_SPILL();\n");
                }
                fprintf(out,
                        "            if (ctx->host_call && "
                        "ppc_host_call(ctx, 0x%08Xu)) {\n",
                        inst->branch_target);
                if (g_homed)
                    fprintf(out, "                DOLRECOMP_RELOAD();\n");
                fprintf(out,
                        "                if (ctx->pc == 0x%08Xu && "
                        "!ctx->exception) goto label_%08X;\n",
                        inst->address + 4u, inst->address + 4u);
                fprintf(out, "                return;\n");
                fprintf(out, "            }\n");
                if (g_homed)
                    fprintf(out, "            DOLRECOMP_RELOAD();\n");
            }
            if (local_backward)
                emit_loop_guard(out, "            ", inst->branch_target, false);
            fprintf(out, "            goto label_%08X;\n", inst->branch_target);
        } else if (g_hle_outcalls &&
                   branch_target_is_local(func_start, func_end,
                                          inst->address + 4u)) {
            if (g_homed)
                fprintf(out, "            DOLRECOMP_SPILL();\n");
            fprintf(out,
                    "            DOLRECOMP_OUTCALL(0x%08Xu, 0x%08Xu, "
                    "label_%08X);\n",
                    inst->branch_target, inst->address + 4u,
                    inst->address + 4u);
            if (g_homed)
                fprintf(out, "            DOLRECOMP_RELOAD();\n");
        } else if (!emit_cross_chunk_call(out, inst, func_start, func_end)) {
            fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
            fprintf(out, "            return;\n");
        }
        return;
    }
    if (local_backward) {
        if (direct_backedge) {
            emit_loop_guard(out, "            ", inst->branch_target, false);
            fprintf(out, "            goto label_%08X;\n", inst->branch_target);
        } else {
            fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
            fprintf(out, "            return;\n");
        }
    } else if (local_target) {
        fprintf(out, "            goto label_%08X;\n", inst->branch_target);
    } else if (!emit_cross_chunk_tail_to(out, inst->branch_target)) {
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            return;\n");
    }
}

static void emit_dynamic_branch(FILE* out, const PPCInst* inst,
                                const char* target_expr,
                                bool route_local_returns, bool resolve_in_place,
                                u32 function_address, u32 function_end) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 target = %s;\n", target_expr);
    emit_branch_condition(out, inst->bo, inst->bi);
    fprintf(out, "        if (ctr_ok && cr_ok) {\n");
    if (inst->lk) {
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
    }
    fprintf(out, "            ctx->pc = target;\n");
    if (resolve_in_place)
        emit_indirect_transfer(out, inst, function_address, function_end);
    if (route_local_returns)
        fprintf(out, "            goto return_dispatch_%08X;\n", function_address);
    else
        fprintf(out, "            return;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
}

static void emit_cr_logical(FILE* out, const PPCInst* inst, const char* expr) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 a = (ctx->cr >> (31u - %uu)) & 1u;\n", inst->rA);
    fprintf(out, "        u32 b = (ctx->cr >> (31u - %uu)) & 1u;\n", inst->rB);
    fprintf(out, "        u32 mask = 0x80000000u >> %u;\n", inst->rD);
    fprintf(out, "        u32 value = (%s) & 1u;\n", expr);
    fprintf(out, "        ctx->cr = (ctx->cr & ~mask) | (value ? mask : 0u);\n");
    fprintf(out, "    }\n");
}

static void emit_record_if_needed(FILE* out, const PPCInst* inst, u8 reg) {
    if (inst->rc) {
        emit_set_cr0_from_gpr(out, reg);
    }
}

static const char* emit_cpu_macro(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "BROADWAY";
    case DOLRECOMP_CPU_ESPRESSO:
        return "ESPRESSO";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "GEKKO";
    }
}

static const char* emit_cpu_label(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "broadway";
    case DOLRECOMP_CPU_ESPRESSO:
        return "espresso";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "gekko";
    }
}

void emit_header_for_cpu(FILE* out, DolRecompCPU cpu) {
    fprintf(out,
        "// DolRecomp output\n"
        "// cpu: %s\n"
        "\n"
        "#ifndef RECOMP_GENERATED_H\n"
        "#define RECOMP_GENERATED_H\n"
        "\n"
        "#define DOLRECOMP_CPU_%s 1\n"
        "#define DOLRECOMP_CPU_NAME \"%s\"\n"
        "\n"
        "#include <string.h>\n"
        "#include <math.h>\n"
        "#ifndef DOLRECOMP_CPU_HEADER\n"
        "#define DOLRECOMP_CPU_HEADER \"cpu/cpu.h\"\n"
        "#endif\n"
        "#include DOLRECOMP_CPU_HEADER\n"
        "\n"
        "#ifndef DOLRECOMP_C_LOOP_CYCLE_BUDGET\n"
        "#define DOLRECOMP_C_LOOP_CYCLE_BUDGET 256\n"
        "#endif\n"
        "\n"
        "/* Cross-chunk calls turn guest recursion into host recursion, and a\n"
        "   chunk frame is not small. Without a ceiling a deep guest call chain\n"
        "   overflows the host stack, which is a crash rather than a slow\n"
        "   emulator. Past the limit the call site falls back to returning to\n"
        "   the chassis, which is always correct -- ctx->pc already names the\n"
        "   target, so the chassis simply dispatches it as it did before.\n"
        "   The counter is plain static, not atomic: the chassis runs the module\n"
        "   on one CPU thread. */\n"
        "#ifndef DOLRECOMP_C_MAX_CALL_DEPTH\n"
        "#define DOLRECOMP_C_MAX_CALL_DEPTH 24\n"
        "#endif\n"
        "extern unsigned dolrecomp_call_depth;\n"
        "static inline int dolrecomp_call_enter(void) {\n"
        "    if (dolrecomp_call_depth >= (unsigned)DOLRECOMP_C_MAX_CALL_DEPTH)\n"
        "        return 0;\n"
        "    dolrecomp_call_depth++;\n"
        "    return 1;\n"
        "}\n"
        "static inline void dolrecomp_call_leave(void) {\n"
        "    if (dolrecomp_call_depth)\n"
        "        dolrecomp_call_depth--;\n"
        "}\n"
        "\n"
        "/* A cross-chunk transfer with nothing to resume into -- a `b` into another\n"
        "   chunk, a `bctr`, the fallthrough off a chunk's last instruction. Where the\n"
        "   target has real tail calls the callee replaces this frame, so a guest loop\n"
        "   that straddles a chunk boundary costs no host stack and needs no depth\n"
        "   accounting: the frame that eventually returns is whichever one the guest's\n"
        "   blr lands in, and the caller that made the original host call is still\n"
        "   the one waiting. wasm has them behind -mtail-call (clang then defines\n"
        "   __wasm_tail_call__); elsewhere it is an ordinary host call that returns,\n"
        "   bounded by the depth ceiling, exactly as before. */\n"
        "#if defined(__wasm_tail_call__) || defined(DOLRECOMP_C_MUSTTAIL)\n"
        "#define DOLRECOMP_TAIL_CALL(call) __attribute__((musttail)) return call\n"
        "#else\n"
        "#define DOLRECOMP_TAIL_CALL(call) \\\n"
        "    do { if (dolrecomp_call_enter()) { call; dolrecomp_call_leave(); } return; } while (0)\n"
        "#endif\n"
        "\n"
        "/* DOLRECOMP_HOMED_VERIFY: a spill site found a register it does not\n"
        "   store differing from memory. Defined by the module template. */\n"
        "void dolrecomp_homed_mismatch(u32 func, u32 site, const char* reg);\n"
        "\n"
        "static inline u32 dolrecomp_rotl32(u32 value, u32 sh) {\n"
        "    sh &= 31u;\n"
        "    return sh ? ((value << sh) | (value >> (32u - sh))) : value;\n"
        "}\n"
        "\n"
        // Preserve the PPC bit-level single conversion, including denormals.
        "static inline f64 dolrecomp_f32_from_bits(u32 bits) {\n"
        "    u64 x = bits;\n"
        "    u64 exp = (x >> 23) & 0xFFu;\n"
        "    u64 frac = x & 0x007FFFFFu;\n"
        "    u64 result;\n"
        "    if (exp > 0 && exp < 255) {\n"
        "        u64 y = !(exp >> 7);\n"
        "        u64 z = (y << 61) | (y << 60) | (y << 59);\n"
        "        result = ((x & 0xC0000000u) << 32) | z |\n"
        "                 ((x & 0x3FFFFFFFu) << 29);\n"
        "    } else if (exp == 0 && frac != 0) {\n"
        "        exp = 1023 - 126;\n"
        "        do {\n"
        "            frac <<= 1;\n"
        "            exp -= 1;\n"
        "        } while ((frac & 0x00800000u) == 0);\n"
        "        result = ((x & 0x80000000u) << 32) | (exp << 52) |\n"
        "                 ((frac & 0x007FFFFFu) << 29);\n"
        "    } else {\n"
        "        u64 y = exp >> 7;\n"
        "        u64 z = (y << 61) | (y << 60) | (y << 59);\n"
        "        result = ((x & 0xC0000000u) << 32) | z |\n"
        "                 ((x & 0x3FFFFFFFu) << 29);\n"
        "    }\n"
        "    f64 value;\n"
        "    memcpy(&value, &result, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_f32_to_bits(f64 value) {\n"
        "    u64 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    u32 exp = (u32)((bits >> 52) & 0x7FFu);\n"
        "    if (exp > 896 || (bits & 0x7FFFFFFFFFFFFFFFull) == 0) {\n"
        "        return (u32)(((bits >> 32) & 0xC0000000u) |\n"
        "                     ((bits >> 29) & 0x3FFFFFFFu));\n"
        "    }\n"
        "    if (exp >= 874) {\n"
        "        u32 result =\n"
        "            (u32)(0x80000000u | ((bits & 0x000FFFFFFFFFFFFFull) >> 21));\n"
        "        result >>= 905 - exp;\n"
        "        result |= (u32)((bits >> 32) & 0x80000000u);\n"
        "        return result;\n"
        "    }\n"
        "    return (u32)(((bits >> 32) & 0xC0000000u) |\n"
        "                 ((bits >> 29) & 0x3FFFFFFFu));\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_f64_from_bits(u64 bits) {\n"
        "    f64 value;\n"
        "    memcpy(&value, &bits, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u64 dolrecomp_f64_to_bits(f64 value) {\n"
        "    u64 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    return bits;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_ps_from_bits(u32 bits) {\n"
        "    return dolrecomp_f32_from_bits(bits);\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_ps_to_bits(f64 value) {\n"
        "    return dolrecomp_f32_to_bits(value);\n"
        "}\n"
        "\n"
        ,
        emit_cpu_label(cpu),
        emit_cpu_macro(cpu),
        emit_cpu_label(cpu));
}

void emit_header(FILE* out) {
    emit_header_for_cpu(out, DOLRECOMP_CPU_GEKKO);
}

void emit_footer(FILE* out) {
    fprintf(out, "\n#endif /* RECOMP_GENERATED_H */\n\n// end\n");
}

// The homed floating-point templates. Each op is a value-based helper from
// core/cpu_fp_homed.h: operands in, a struct out, and the destination written
// here from it -- so the register file never has to be in memory for a helper
// to find it. The register references are emitted in the ctx->fpr[N] spelling
// and the rewrite pass turns them into the locals. `both` says the result goes
// to both lanes (single precision, frsp, fres).
// `call` is the helper invocation up to and including its closing paren; the
// result temporaries are appended as out-pointers. They are plain locals whose
// address only an always-inline function sees, so the compiler scalarises
// them away on the fast path.
static void emit_fp1(FILE* out, u8 d, bool both, const char* call) {
    size_t n = strlen(call);
    fprintf(out, "    { f64 t_; if (%.*s, &t_)) { ctx->fpr[%u] = t_;", (int)(n - 1), call, d);
    if (both)
        fprintf(out, " ctx->ps1[%u] = t_;", d);
    fprintf(out, " } }\n");
}

static void emit_fp2(FILE* out, u8 d, const char* call) {
    size_t n = strlen(call);
    fprintf(out, "    { f64 t0_, t1_; %.*s, &t0_, &t1_); ctx->fpr[%u] = t0_; ctx->ps1[%u] = t1_; }\n",
            (int)(n - 1), call, d, d);
}

// The interpreter fallback reads and writes any guest register, and the
// chassis's fallback reads ctx->pc: with homed registers both have to be in
// memory around it.
static void emit_fallback_instruction(FILE* out, const PPCInst* inst) {
    if (g_homed) {
        fprintf(out, "    ctx->pc = 0x%08Xu;\n", inst->address);
        fprintf(out, "    DOLRECOMP_SPILL();\n");
    }
    fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
            inst->raw, inst->address);
    if (g_homed)
        fprintf(out, "    DOLRECOMP_RELOAD();\n");
    fprintf(out, "    return;\n");
}

static void emit_instruction_with_range(FILE* out, const PPCInst* inst,
                                        u32 func_start, u32 func_end,
                                        bool direct_backedge,
                                        bool route_local_returns) {
    char disasm[64];
    ppc_disasm(disasm, sizeof(disasm), inst);
    fprintf(out, "    // %08X: %s\n", inst->address, disasm);

    if (inst->embedded_data) {
        fprintf(out, "    // embedded data\n\n");
        return;
    }

    if (ppc_op_uses_fpu(inst->op)) {
        if (g_homed) {
            // MSR is read through a local the rewrite keeps current (msr_):
            // one test per instruction and no load, and the compiler folds the
            // repeats between calls. The raise always returns false.
            fprintf(out, "    if (!(ctx->msr & PPC_MSR_FP)) { ppc_fp_raise_unavailable(ctx, 0x%08Xu); return; }\n",
                    inst->address);
        } else {
            fprintf(out, "    if (!ppc_fp_available_inline(ctx, 0x%08Xu)) return;\n", inst->address);
        }
    }

    switch (inst->op) {
    case PPC_OP_MULLI:
        fprintf(out, "    ctx->gpr[%u] = (u32)((s64)(s32)ctx->gpr[%u] * (s64)(s32)%d);\n",
                inst->rD, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_SUBFIC:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 res = (u64)(u32)(s32)(%d) + (u64)(~ctx->gpr[%u]) + 1u;\n",
                (int)inst->simm, inst->rA);
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(res >> 32) & 1u) << 29);\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDI:
        if (inst->rA == 0) {
            fprintf(out, "    ctx->gpr[%u] = (u32)(s32)(%d);\n",
                    inst->rD, (int)inst->simm);
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] + (u32)(s32)(%d);\n",
                    inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_ADDIC:
    case PPC_OP_ADDIC_DOT:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u64 b = (u32)(s32)(%d);\n", (int)inst->simm);
        fprintf(out, "        u64 res = a + b;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(res >> 32) & 1u) << 29);\n");
        if (inst->op == PPC_OP_ADDIC_DOT) {
            emit_set_cr0_from_gpr(out, inst->rD);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDIS:
        if (inst->rA == 0) {
            fprintf(out, "    ctx->gpr[%u] = ((u32)(s32)(%d) << 16);\n",
                    inst->rD, (int)inst->simm);
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] + ((u32)(s32)(%d) << 16);\n",
                    inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_CMPI:
        {
            char rhs[32];
            snprintf(rhs, sizeof(rhs), "%d", (int)inst->simm);
            char lhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            emit_compare_s32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMPLI:
        {
            char rhs[32];
            snprintf(rhs, sizeof(rhs), "0x%04Xu", inst->uimm);
            char lhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            emit_compare_u32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMP:
        {
            char lhs[32], rhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            snprintf(rhs, sizeof(rhs), "ctx->gpr[%u]", inst->rB);
            emit_compare_s32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMPL:
        {
            char lhs[32], rhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            snprintf(rhs, sizeof(rhs), "ctx->gpr[%u]", inst->rB);
            emit_compare_u32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_ORI:
        if (inst->rS == 0 && inst->rA == 0 && inst->uimm == 0) {
            fprintf(out, "    // nop\n");
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] | 0x%04Xu;\n",
                    inst->rA, inst->rS, inst->uimm);
        }
        break;

    case PPC_OP_ORIS:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] | (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORI:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] ^ 0x%04Xu;\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORIS:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] ^ (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_ANDI:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ctx->gpr[%u] & 0x%04Xu;\n",
                inst->rA, inst->rS, inst->uimm);
        emit_set_cr0_from_gpr(out, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ANDIS:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ctx->gpr[%u] & (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        emit_set_cr0_from_gpr(out, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADD:
    case PPC_OP_ADDO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 res = a + b;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDC:
    case PPC_OP_ADDCO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)a + (u64)b;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDE:
    case PPC_OP_ADDEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)a + (u64)b + carry;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDME:
    case PPC_OP_ADDMEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 input = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 res = (u64)input + 0xFFFFFFFFull + carry;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0u);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(input, 0xFFFFFFFFu, (u32)res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDZE:
    case PPC_OP_ADDZEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u64 wide = (u64)a + ((ctx->xer >> 29) & 1u);\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, 0u, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBF:
    case PPC_OP_SUBFO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 res = a + b + 1u;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFC:
    case PPC_OP_SUBFCO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)b + (u64)a + 1u;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFE:
    case PPC_OP_SUBFEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 wide = (u64)a + (u64)b + carry;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFME:
    case PPC_OP_SUBFMEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 input = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 res = (u64)input + 0xFFFFFFFFull + carry;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0u);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(input, 0xFFFFFFFFu, (u32)res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFZE:
    case PPC_OP_SUBFZEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u64 wide = (u64)a + ((ctx->xer >> 29) & 1u);\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, 0u, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_NEG:
    case PPC_OP_NEGO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        ctx->gpr[%u] = (~a) + 1u;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, a == 0x80000000u);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
        fprintf(out, "    {\n");
        fprintf(out, "        s64 product = (s64)(s32)ctx->gpr[%u] * (s64)(s32)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)product;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, product < -0x80000000ll || product > 0x7fffffffll);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULHW:
        fprintf(out, "    {\n");
        fprintf(out, "        s64 product = (s64)(s32)ctx->gpr[%u] * (s64)(s32)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)(product >> 32);\n", inst->rD);
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULHWU:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 product = (u64)ctx->gpr[%u] * (u64)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)(product >> 32);\n", inst->rD);
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
        fprintf(out, "    {\n");
        fprintf(out, "        s32 dividend = (s32)ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        s32 divisor = (s32)ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        bool ov = divisor == 0 || ((u32)dividend == 0x80000000u && divisor == -1);\n");
        fprintf(out, "        ctx->gpr[%u] = ov ? ((dividend < 0) ? 0xFFFFFFFFu : 0u) : (u32)(dividend / divisor);\n",
                inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ov);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 divisor = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = divisor == 0 ? 0u : ctx->gpr[%u] / divisor;\n",
                inst->rD, inst->rA);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, divisor == 0);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_AND:
    case PPC_OP_ANDC:
    case PPC_OP_OR:
    case PPC_OP_ORC:
    case PPC_OP_XOR:
    case PPC_OP_NAND:
    case PPC_OP_NOR:
    case PPC_OP_EQV: {
        const char* expr = NULL;
        switch (inst->op) {
        case PPC_OP_AND:  expr = "ctx->gpr[%u] & ctx->gpr[%u]"; break;
        case PPC_OP_ANDC: expr = "ctx->gpr[%u] & ~ctx->gpr[%u]"; break;
        case PPC_OP_OR:   expr = "ctx->gpr[%u] | ctx->gpr[%u]"; break;
        case PPC_OP_ORC:  expr = "ctx->gpr[%u] | ~ctx->gpr[%u]"; break;
        case PPC_OP_XOR:  expr = "ctx->gpr[%u] ^ ctx->gpr[%u]"; break;
        case PPC_OP_NAND: expr = "~(ctx->gpr[%u] & ctx->gpr[%u])"; break;
        case PPC_OP_NOR:  expr = "~(ctx->gpr[%u] | ctx->gpr[%u])"; break;
        default:          expr = "~(ctx->gpr[%u] ^ ctx->gpr[%u])"; break;
        }
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ", inst->rA);
        fprintf(out, expr, inst->rS, inst->rB);
        fprintf(out, ";\n");
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_CNTLZW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 v = ctx->gpr[%u];\n", inst->rS);
        fprintf(out, "        u32 n = 0;\n");
        fprintf(out, "        while (n < 32 && ((v & (0x80000000u >> n)) == 0)) n++;\n");
        fprintf(out, "        ctx->gpr[%u] = n;\n", inst->rA);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_EXTSB:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(s8)ctx->gpr[%u];\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_EXTSH:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(s16)ctx->gpr[%u];\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SLW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = sh > 31 ? 0u : (ctx->gpr[%u] << sh);\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SRW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = sh > 31 ? 0u : (ctx->gpr[%u] >> sh);\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SRAW:
    case PPC_OP_SRAWI:
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_SRAWI) {
            fprintf(out, "        u32 sh = %uu;\n", inst->sh);
        } else {
            fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        }
        fprintf(out, "        u32 value = ctx->gpr[%u];\n", inst->rS);
        fprintf(out, "        bool ca = false;\n");
        fprintf(out, "        if (sh == 0) {\n");
        fprintf(out, "            ctx->gpr[%u] = value;\n", inst->rA);
        fprintf(out, "        } else if (sh > 31) {\n");
        fprintf(out, "            ctx->gpr[%u] = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;\n", inst->rA);
        fprintf(out, "            ca = (value & 0x80000000u) != 0;\n");
        fprintf(out, "        } else {\n");
        fprintf(out, "            ctx->gpr[%u] = (u32)((s32)value >> sh);\n", inst->rA);
        fprintf(out, "            ca = (value & 0x80000000u) && ((value << (32u - sh)) != 0);\n");
        fprintf(out, "        }\n");
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (ca ? 0x20000000u : 0u);\n");
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_RLWINM:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        ctx->gpr[%u] = dolrecomp_rotl32(ctx->gpr[%u], %uu) & 0x%08Xu;\n",
                    inst->rA, inst->rS, inst->sh, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_RLWNM:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        ctx->gpr[%u] = dolrecomp_rotl32(ctx->gpr[%u], ctx->gpr[%u]) & 0x%08Xu;\n",
                    inst->rA, inst->rS, inst->rB, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_RLWIMI:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        u32 rot = dolrecomp_rotl32(ctx->gpr[%u], %uu);\n",
                    inst->rS, inst->sh);
            fprintf(out, "        ctx->gpr[%u] = (ctx->gpr[%u] & ~0x%08Xu) | (rot & 0x%08Xu);\n",
                    inst->rA, inst->rA, mask, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_FADDS:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fadds_h(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rB);
            emit_fp1(out, inst->rD, true, call);
        } else {
            fprintf(out, "    ppc_fadds(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FSUBS:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fsubs_h(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rB);
            emit_fp1(out, inst->rD, true, call);
        } else {
            fprintf(out, "    ppc_fsubs(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMULS:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fmuls_h(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rC);
            emit_fp1(out, inst->rD, true, call);
        } else {
            fprintf(out, "    ppc_fmuls(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rC);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FDIVS:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fdivs_hi(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rB);
            emit_fp1(out, inst->rD, true, call);
        } else {
            fprintf(out, "    ppc_fdivs(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FRES:
        if (g_homed && g_homed_fpr) {
            char call[64];
            snprintf(call, sizeof(call), "ppc_fres_hi(ctx, ctx->fpr[%u])", inst->rB);
            emit_fp1(out, inst->rD, true, call);
        } else {
            fprintf(out, "    { f64 result; if (ppc_fres(ctx, ctx->fpr[%u], &result)) ctx->fpr[%u] = ctx->ps1[%u] = result; }\n",
                    inst->rB, inst->rD, inst->rD);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADDS:
    case PPC_OP_FMSUBS:
    case PPC_OP_FNMADDS:
    case PPC_OP_FNMSUBS: {
        const bool sub = inst->op == PPC_OP_FMSUBS || inst->op == PPC_OP_FNMSUBS;
        const bool neg = inst->op == PPC_OP_FNMADDS || inst->op == PPC_OP_FNMSUBS;
        if (g_homed && g_homed_fpr) {
            char call[128];
            snprintf(call, sizeof(call),
                     "ppc_fmadd_h(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], true, %s, %s)",
                     inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
            emit_fp1(out, inst->rD, true, call);
            if (inst->rc) emit_set_cr1_from_fpscr(out);
            break;
        }
        fprintf(out, "    {\n");
        fprintf(out, "        f64 result;\n");
        fprintf(out, "        if (ppc_fma(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], true, %s, %s, &result))\n",
                inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
        fprintf(out, "            ctx->fpr[%u] = ctx->ps1[%u] = result;\n", inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_FADD:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fadd_h(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rB);
            emit_fp1(out, inst->rD, false, call);
        } else {
            fprintf(out, "    ppc_fadd(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FSUB:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fsub_h(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rB);
            emit_fp1(out, inst->rD, false, call);
        } else {
            fprintf(out, "    ppc_fsub(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMUL:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fmul_h(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rC);
            emit_fp1(out, inst->rD, false, call);
        } else {
            fprintf(out, "    ppc_fmul(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rC);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FDIV:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_fdiv_hi(ctx, ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rB);
            emit_fp1(out, inst->rD, false, call);
        } else {
            fprintf(out, "    ppc_fdiv(ctx, %u, %u, %u);\n", inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FRSQRTE:
        if (g_homed && g_homed_fpr) {
            char call[64];
            snprintf(call, sizeof(call), "ppc_frsqrte_hi(ctx, ctx->fpr[%u])", inst->rB);
            emit_fp1(out, inst->rD, false, call);
        } else {
            fprintf(out, "    { f64 result; if (ppc_frsqrte(ctx, ctx->fpr[%u], &result)) ctx->fpr[%u] = result; }\n",
                    inst->rB, inst->rD);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADD:
    case PPC_OP_FMSUB:
    case PPC_OP_FNMADD:
    case PPC_OP_FNMSUB: {
        const bool sub = inst->op == PPC_OP_FMSUB || inst->op == PPC_OP_FNMSUB;
        const bool neg = inst->op == PPC_OP_FNMADD || inst->op == PPC_OP_FNMSUB;
        if (g_homed && g_homed_fpr) {
            char call[128];
            snprintf(call, sizeof(call),
                     "ppc_fmadd_h(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], false, %s, %s)",
                     inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
            emit_fp1(out, inst->rD, false, call);
            if (inst->rc) emit_set_cr1_from_fpscr(out);
            break;
        }
        fprintf(out, "    {\n");
        fprintf(out, "        f64 result;\n");
        fprintf(out, "        if (ppc_fma(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], false, %s, %s, &result))\n",
                inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
        fprintf(out, "            ctx->fpr[%u] = result;\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_FCTIW:
    case PPC_OP_FCTIWZ:
        if (g_homed && g_homed_fpr) {
            char call[80];
            snprintf(call, sizeof(call), "ppc_fctiw_hi(ctx, ctx->fpr[%u], %s)", inst->rB,
                     inst->op == PPC_OP_FCTIWZ ? "true" : "false");
            emit_fp1(out, inst->rD, false, call);
        } else {
            fprintf(out, "    { u64 result; if (ppc_fctiw(ctx, ctx->fpr[%u], %s, &result)) ctx->fpr[%u] = dolrecomp_f64_from_bits(result); }\n",
                    inst->rB, inst->op == PPC_OP_FCTIWZ ? "true" : "false", inst->rD);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMR:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FNEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) ^ 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFFFFFFFFFull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FNABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) | 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FRSP:
        if (g_homed && g_homed_fpr) {
            char call[64];
            snprintf(call, sizeof(call), "ppc_frsp_hi(ctx, ctx->fpr[%u])", inst->rB);
            emit_fp1(out, inst->rD, true, call);
        } else {
            fprintf(out, "    ppc_frsp(ctx, %u, %u);\n", inst->rD, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FSEL:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = (ctx->fpr[%u] >= 0.0) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 mask = 0x80000000u >> %u;\n", inst->rD);
        if (inst->op == PPC_OP_MTFSB0) {
            fprintf(out, "        if (%u != 1 && %u != 2) ctx->fpscr &= ~mask;\n",
                    inst->rD, inst->rD);
        } else {
            fprintf(out, "        if (%u != 1 && %u != 2) ctx->fpscr |= mask;\n",
                    inst->rD, inst->rD);
        }
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MFFS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(0xFFF8000000000000ull | ctx->fpscr);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_MCRFS: {
        u32 shift = cr_field_shift(inst->crfS);
        u32 dst_shift = cr_field_shift(inst->crfD);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 field = (ctx->fpscr >> %u) & 0xFu;\n", shift);
        fprintf(out, "        ctx->fpscr &= ~((0xFu << %u) & 0x83F80700u);\n", shift);
        fprintf(out, "        ppc_fpscr_updated(ctx);\n");
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (field << %u);\n", dst_shift, dst_shift);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MTFSFI: {
        u32 shift = cr_field_shift(inst->crfD);
        fprintf(out, "    ctx->fpscr = (ctx->fpscr & ~(0xFu << %u)) | (0x%Xu << %u);\n",
                shift, inst->imm, shift);
        fprintf(out, "    ppc_fpscr_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;
    }

    case PPC_OP_MTFSF:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 mask = 0;\n");
        fprintf(out, "        for (u32 i = 0; i < 8; i++) if (0x%02Xu & (1u << i)) mask |= 0xFu << (i * 4);\n", inst->fm);
        fprintf(out, "        u32 source = (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]);\n", inst->rB);
        fprintf(out, "        ctx->fpscr = (ctx->fpscr & ~mask) | (source & mask);\n");
        fprintf(out, "        ppc_fpscr_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_ADD:
        if (g_homed && g_homed_fpr) {
            char call[160];
            snprintf(call, sizeof(call), "ppc_ps_add_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u], ctx->ps1[%u])",
                     inst->rA, inst->rA, inst->rB, inst->rB);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_add_op(ctx, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUB:
        if (g_homed && g_homed_fpr) {
            char call[160];
            snprintf(call, sizeof(call), "ppc_ps_sub_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u], ctx->ps1[%u])",
                     inst->rA, inst->rA, inst->rB, inst->rB);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_sub_op(ctx, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MUL:
        if (g_homed && g_homed_fpr) {
            char call[160];
            snprintf(call, sizeof(call), "ppc_ps_mul_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u], ctx->ps1[%u])",
                     inst->rA, inst->rA, inst->rC, inst->rC);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_mul_op(ctx, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rC);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_DIV:
        if (g_homed && g_homed_fpr) {
            char call[160];
            snprintf(call, sizeof(call),
                     "ppc_ps_div_hi(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u], ctx->ps1[%u])",
                     inst->rA, inst->rA, inst->rB, inst->rB);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_div_op(ctx, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_RES:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_ps_res_hi(ctx, ctx->fpr[%u], ctx->ps1[%u])",
                     inst->rB, inst->rB);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_res_op(ctx, %u, %u);\n", inst->rD, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_RSQRTE:
        if (g_homed && g_homed_fpr) {
            char call[96];
            snprintf(call, sizeof(call), "ppc_ps_rsqrte_hi(ctx, ctx->fpr[%u], ctx->ps1[%u])",
                     inst->rB, inst->rB);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_rsqrte_op(ctx, %u, %u);\n", inst->rD, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADD:
    case PPC_OP_PS_MSUB:
    case PPC_OP_PS_NMADD:
    case PPC_OP_PS_NMSUB:
        if (g_homed && g_homed_fpr) {
            char call[200];
            snprintf(call, sizeof(call),
                     "ppc_ps_madd_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u], ctx->ps1[%u], "
                     "ctx->fpr[%u], ctx->ps1[%u], %s, %s)",
                     inst->rA, inst->rA, inst->rC, inst->rC, inst->rB, inst->rB,
                     (inst->op == PPC_OP_PS_MSUB || inst->op == PPC_OP_PS_NMSUB) ?
                         "true" : "false",
                     (inst->op == PPC_OP_PS_NMADD || inst->op == PPC_OP_PS_NMSUB) ?
                         "true" : "false");
            emit_fp2(out, inst->rD, call);
            if (inst->rc) emit_set_cr1_from_fpscr(out);
            break;
        }
        fprintf(out, "    ppc_ps_madd_op(ctx, %u, %u, %u, %u, %s, %s);\n",
                inst->rD, inst->rA, inst->rC, inst->rB,
                (inst->op == PPC_OP_PS_MSUB || inst->op == PPC_OP_PS_NMSUB) ?
                    "true" : "false",
                (inst->op == PPC_OP_PS_NMADD || inst->op == PPC_OP_PS_NMSUB) ?
                    "true" : "false");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_NEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) ^ 0x80000000u);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) ^ 0x80000000u);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_ABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFu);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) & 0x7FFFFFFFu);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_NABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) | 0x80000000u);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) | 0x80000000u);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MR:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = ctx->ps1[%u];\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUM0:
        if (g_homed && g_homed_fpr) {
            char call[128];
            snprintf(call, sizeof(call),
                     "ppc_ps_sum0_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->ps1[%u])",
                     inst->rA, inst->rB, inst->rC);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_sum0(ctx, %u, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rC, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUM1:
        if (g_homed && g_homed_fpr) {
            char call[128];
            snprintf(call, sizeof(call),
                     "ppc_ps_sum1_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u])",
                     inst->rA, inst->rB, inst->rC);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_sum1(ctx, %u, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rC, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS0:
        if (g_homed && g_homed_fpr) {
            char call[160];
            snprintf(call, sizeof(call),
                     "ppc_ps_mul_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u], ctx->fpr[%u])",
                     inst->rA, inst->rA, inst->rC, inst->rC);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_muls0(ctx, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rC);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS1:
        if (g_homed && g_homed_fpr) {
            char call[160];
            snprintf(call, sizeof(call),
                     "ppc_ps_mul_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->ps1[%u], ctx->ps1[%u])",
                     inst->rA, inst->rA, inst->rC, inst->rC);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_muls1(ctx, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rC);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS0:
        if (g_homed && g_homed_fpr) {
            char call[200];
            snprintf(call, sizeof(call),
                     "ppc_ps_madd_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->fpr[%u], ctx->fpr[%u], "
                     "ctx->fpr[%u], ctx->ps1[%u], false, false)",
                     inst->rA, inst->rA, inst->rC, inst->rC, inst->rB, inst->rB);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_madds0(ctx, %u, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rC, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS1:
        if (g_homed && g_homed_fpr) {
            char call[200];
            snprintf(call, sizeof(call),
                     "ppc_ps_madd_h(ctx, ctx->fpr[%u], ctx->ps1[%u], ctx->ps1[%u], ctx->ps1[%u], "
                     "ctx->fpr[%u], ctx->ps1[%u], false, false)",
                     inst->rA, inst->rA, inst->rC, inst->rC, inst->rB, inst->rB);
            emit_fp2(out, inst->rD, call);
        } else {
            fprintf(out, "    ppc_ps_madds1(ctx, %u, %u, %u, %u);\n",
                    inst->rD, inst->rA, inst->rC, inst->rB);
        }
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE00:
        emit_ps_merge(out, inst, false, false);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE01:
        emit_ps_merge(out, inst, false, true);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE10:
        emit_ps_merge(out, inst, true, false);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE11:
        emit_ps_merge(out, inst, true, true);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_CMPU0:
    case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1:
    case PPC_OP_PS_CMPO1: {
        bool lane1 = inst->op == PPC_OP_PS_CMPU1 || inst->op == PPC_OP_PS_CMPO1;
        bool ordered = inst->op == PPC_OP_PS_CMPO0 || inst->op == PPC_OP_PS_CMPO1;
        const char* bank = lane1 ? "ps1" : "fpr";
        fprintf(out, "    ctx->cr = ppc_fcmp_cr(ctx, ctx->cr, %u, ctx->%s[%u], ctx->%s[%u], %s);\n",
                inst->crfD, bank, inst->rA, bank, inst->rB,
                ordered ? "true" : "false");
        break;
    }

    case PPC_OP_PS_SEL:
        fprintf(out, "    ctx->fpr[%u] = ((f32)ctx->fpr[%u] >= 0.0f) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = ((f32)ctx->ps1[%u] >= 0.0f) ? ctx->ps1[%u] : ctx->ps1[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FCMPU:
    case PPC_OP_FCMPO:
        emit_fcompare(out, inst);
        break;

    case PPC_OP_LWZ:  emit_load(out, inst, "mem_read32(ctx, ea)", false); break;
    case PPC_OP_LWZU: emit_load(out, inst, "mem_read32(ctx, ea)", true); break;
    case PPC_OP_LBZ:  emit_load(out, inst, "mem_read8(ctx, ea)", false); break;
    case PPC_OP_LBZU: emit_load(out, inst, "mem_read8(ctx, ea)", true); break;
    case PPC_OP_LHZ:  emit_load(out, inst, "mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHZU: emit_load(out, inst, "mem_read16(ctx, ea)", true); break;
    case PPC_OP_LHA:  emit_load(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHAU: emit_load(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", true); break;

    case PPC_OP_LWZX:  emit_loadx(out, inst, "mem_read32(ctx, ea)", false); break;
    case PPC_OP_LWZUX: emit_loadx(out, inst, "mem_read32(ctx, ea)", true); break;
    case PPC_OP_LBZX:  emit_loadx(out, inst, "mem_read8(ctx, ea)", false); break;
    case PPC_OP_LBZUX: emit_loadx(out, inst, "mem_read8(ctx, ea)", true); break;
    case PPC_OP_LHZX:  emit_loadx(out, inst, "mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHZUX: emit_loadx(out, inst, "mem_read16(ctx, ea)", true); break;
    case PPC_OP_LHAX:  emit_loadx(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", false); break;
    case PPC_OP_LHAUX: emit_loadx(out, inst, "(u32)(s32)(s16)mem_read16(ctx, ea)", true); break;
    case PPC_OP_LWBRX: emit_loadx(out, inst, "bswap32(mem_read32(ctx, ea))", false); break;
    case PPC_OP_LHBRX: emit_loadx(out, inst, "bswap16(mem_read16(ctx, ea))", false); break;

    case PPC_OP_LFS:   emit_fload(out, inst, true,  false); break;
    case PPC_OP_LFSU:  emit_fload(out, inst, true,  true); break;
    case PPC_OP_LFD:   emit_fload(out, inst, false, false); break;
    case PPC_OP_LFDU:  emit_fload(out, inst, false, true); break;

    case PPC_OP_LFSX:  emit_floadx(out, inst, true,  false); break;
    case PPC_OP_LFSUX: emit_floadx(out, inst, true,  true); break;
    case PPC_OP_LFDX:  emit_floadx(out, inst, false, false); break;
    case PPC_OP_LFDUX: emit_floadx(out, inst, false, true); break;

    case PPC_OP_PSQ_L:   emit_psq_load(out, inst, false, false); break;
    case PPC_OP_PSQ_LU:  emit_psq_load(out, inst, false, true); break;
    case PPC_OP_PSQ_LX:  emit_psq_load(out, inst, true,  false); break;
    case PPC_OP_PSQ_LUX: emit_psq_load(out, inst, true,  true); break;

    case PPC_OP_STW:  emit_store(out, inst, "mem_write32", "u32", false); break;
    case PPC_OP_STWU: emit_store(out, inst, "mem_write32", "u32", true); break;
    case PPC_OP_STB:  emit_store(out, inst, "mem_write8", "u8", false); break;
    case PPC_OP_STBU: emit_store(out, inst, "mem_write8", "u8", true); break;
    case PPC_OP_STH:  emit_store(out, inst, "mem_write16", "u16", false); break;
    case PPC_OP_STHU: emit_store(out, inst, "mem_write16", "u16", true); break;

    case PPC_OP_STWX:  emit_storex(out, inst, "mem_write32", "u32", false); break;
    case PPC_OP_STWUX: emit_storex(out, inst, "mem_write32", "u32", true); break;
    case PPC_OP_STBX:  emit_storex(out, inst, "mem_write8", "u8", false); break;
    case PPC_OP_STBUX: emit_storex(out, inst, "mem_write8", "u8", true); break;
    case PPC_OP_STHX:  emit_storex(out, inst, "mem_write16", "u16", false); break;
    case PPC_OP_STHUX: emit_storex(out, inst, "mem_write16", "u16", true); break;

    case PPC_OP_STFS:   emit_fstore(out, inst, true,  false); break;
    case PPC_OP_STFSU:  emit_fstore(out, inst, true,  true); break;
    case PPC_OP_STFD:   emit_fstore(out, inst, false, false); break;
    case PPC_OP_STFDU:  emit_fstore(out, inst, false, true); break;

    case PPC_OP_STFSX:  emit_fstorex(out, inst, true,  false); break;
    case PPC_OP_STFSUX: emit_fstorex(out, inst, true,  true); break;
    case PPC_OP_STFDX:  emit_fstorex(out, inst, false, false); break;
    case PPC_OP_STFDUX: emit_fstorex(out, inst, false, true); break;

    case PPC_OP_PSQ_ST:   emit_psq_store(out, inst, false, false); break;
    case PPC_OP_PSQ_STU:  emit_psq_store(out, inst, false, true); break;
    case PPC_OP_PSQ_STX:  emit_psq_store(out, inst, true,  false); break;
    case PPC_OP_PSQ_STUX: emit_psq_store(out, inst, true,  true); break;

    case PPC_OP_STWBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        mem_write32(ctx, ea, bswap32(ctx->gpr[%u]));\n", inst->rS);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STHBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        mem_write16(ctx, ea, bswap16((u16)ctx->gpr[%u]));\n", inst->rS);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_LSWI:
    case PPC_OP_LSWX: {
        u32 count = inst->op == PPC_OP_LSWI ? (inst->nb ? inst->nb : 32u) : 0u;
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_LSWX) {
            fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rB);
            if (inst->rA)
                fprintf(out, "        ea += ctx->gpr[%u];\n", inst->rA);
            fprintf(out, "        u32 count = ctx->xer & 0x7Fu;\n");
            fprintf(out, "        u32 reg_count = (count + 3u) / 4u;\n");
            fprintf(out, "        for (u32 r = 0; r < reg_count; r++) {\n");
            fprintf(out, "            u32 reg = (%uu + r) & 31u;\n", inst->rD);
            fprintf(out, "            if (reg == %uu || reg == %uu) {\n", inst->rA, inst->rB);
            fprintf(out, "                ppc_program_exception(ctx, PPC_PROGRAM_ILLEGAL, 0x%08Xu);\n",
                    inst->address);
            fprintf(out, "                return;\n");
            fprintf(out, "            }\n");
            fprintf(out, "        }\n");
        } else {
            if (inst->rA) fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rA);
            else fprintf(out, "        u32 ea = 0u;\n");
            fprintf(out, "        u32 count = %uu;\n", count);
        }
        if (g_homed) fprintf(out, "        DOLRECOMP_SPILL();\n");
        fprintf(out, "        for (u32 n = 0; n < count; n++) {\n");
        fprintf(out, "            u32 reg = (%uu + n / 4u) & 31u;\n", inst->rD);
        fprintf(out, "            if ((n & 3u) == 0) ctx->gpr[reg] = 0;\n");
        fprintf(out, "            ctx->gpr[reg] |= (u32)mem_read8(ctx, ea + n) << (24u - 8u * (n & 3u));\n");
        fprintf(out, "        }\n");
        if (g_homed) fprintf(out, "        DOLRECOMP_RELOAD();\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_STSWI:
    case PPC_OP_STSWX: {
        u32 count = inst->op == PPC_OP_STSWI ? (inst->nb ? inst->nb : 32u) : 0u;
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_STSWX) {
            fprintf(out, "        u32 ea = ctx->gpr[%u]", inst->rB);
            if (inst->rA) fprintf(out, " + ctx->gpr[%u]", inst->rA);
            fprintf(out, ";\n        u32 count = ctx->xer & 0x7Fu;\n");
        } else {
            if (inst->rA) fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rA);
            else fprintf(out, "        u32 ea = 0u;\n");
            fprintf(out, "        u32 count = %uu;\n", count);
        }
        if (g_homed) fprintf(out, "        DOLRECOMP_SPILL();\n");
        fprintf(out, "        for (u32 n = 0; n < count; n++) {\n");
        fprintf(out, "            u32 reg = (%uu + n / 4u) & 31u;\n", inst->rS);
        fprintf(out, "            u8 value = (u8)(ctx->gpr[reg] >> (24u - 8u * (n & 3u)));\n");
        fprintf(out, "            mem_write8(ctx, ea + n, value);\n");
        fprintf(out, "        }\n");
        if (g_homed) fprintf(out, "        DOLRECOMP_RELOAD();\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_LWARX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        ctx->gpr[%u] = mem_read32(ctx, ea);\n", inst->rD);
        fprintf(out, "        ctx->reserve_addr = ea;\n        ctx->reserve_valid = true;\n    }\n");
        break;

    case PPC_OP_STWCX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        bool success = ctx->reserve_valid && ea == ctx->reserve_addr;\n");
        fprintf(out, "        if (success) { mem_write32(ctx, ea, ctx->gpr[%u]); ctx->reserve_valid = false; }\n", inst->rS);
        fprintf(out, "        ctx->cr = (ctx->cr & 0x0FFFFFFFu) | ((success ? 2u : 0u) << 28) | ((ctx->xer >> 3) & 0x10000000u);\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STFIWX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        mem_write32(ctx, ea, (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]));\n    }\n", inst->rS);
        break;

    case PPC_OP_DCBZ:
        emit_dcbz(out, inst);
        break;

    case PPC_OP_DCBZ_L:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_dcbz_l(ctx, ea, 0x%08Xu);\n", inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI: {
        /* The host has a hook for exactly these -- ctx->cache_control, which
         * the LLVM backend has always used -- and it does not need the PC
         * handed back, so the block can carry on. Falling back instead cost a
         * return to the dispatcher per instruction, and a game that flushes
         * caches in 32-byte loops does that a lot: Disney skate issues 1.6
         * million of these a second, and each one was a chassis round trip and
         * a re-dispatch. */
        const char* cache_op =
            inst->op == PPC_OP_DCBST ? "PPC_CACHE_DCBST" :
            inst->op == PPC_OP_DCBF  ? "PPC_CACHE_DCBF"  :
            inst->op == PPC_OP_DCBI  ? "PPC_CACHE_DCBI"  : "PPC_CACHE_ICBI";
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_cache_control(ctx, %s, ea, 0x%08Xu);\n", cache_op,
                inst->address);
        /* dcbi from user mode is a privilege trap, which ppc_cache_control
         * raises; everything else leaves the CPU state alone. */
        fprintf(out, "        if (ctx->exception) return;\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
        fprintf(out, "    (void)ctx;\n");
        break;

    case PPC_OP_LMW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_dform_ea(out, inst->rA, inst->simm, false);
        fprintf(out, ";\n");
        if (g_homed) fprintf(out, "        DOLRECOMP_SPILL();\n");
        fprintf(out, "        for (u32 r = %u; r < 32; r++, ea += 4) ctx->gpr[r] = mem_read32(ctx, ea);\n",
                inst->rD);
        if (g_homed) fprintf(out, "        DOLRECOMP_RELOAD();\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STMW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_dform_ea(out, inst->rA, inst->simm, false);
        fprintf(out, ";\n");
        if (g_homed) fprintf(out, "        DOLRECOMP_SPILL();\n");
        fprintf(out, "        for (u32 r = %u; r < 32; r++, ea += 4) mem_write32(ctx, ea, ctx->gpr[r]);\n",
                inst->rS);
        if (g_homed) fprintf(out, "        DOLRECOMP_RELOAD();\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_B:
        fprintf(out, "    {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target),
                           direct_backedge, func_start, func_end);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_BC:
        fprintf(out, "    {\n");
        emit_branch_condition(out, inst->bo, inst->bi);
        fprintf(out, "        if (ctr_ok && cr_ok) {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target),
                           direct_backedge, func_start, func_end);
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_BCLR:
        // A return is the one transfer that must actually return: the host
        // frame that made the call is waiting for it. Only a bclrl -- a call
        // through LR -- is resolved in place.
        emit_dynamic_branch(out, inst, "ctx->lr & ~3u",
                            route_local_returns, inst->lk != 0, func_start,
                            func_end);
        break;

    case PPC_OP_BCCTR:
        emit_dynamic_branch(out, inst, "ctx->ctr & ~3u", false, true,
                            func_start, func_end);
        break;

    case PPC_OP_TWI:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], (u32)(s32)%d)) {\n",
                inst->to, inst->rA, (int)inst->simm);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        return;\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_TW:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], ctx->gpr[%u])) {\n",
                inst->to, inst->rA, inst->rB);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        return;\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SC:
        fprintf(out, "    ppc_system_call_exception(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    return;\n");
        break;

    case PPC_OP_RFI:
        fprintf(out, "    ppc_rfi(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    return;\n");
        break;

    case PPC_OP_CRAND:  emit_cr_logical(out, inst, "a & b"); break;
    case PPC_OP_CRANDC: emit_cr_logical(out, inst, "a & ~b"); break;
    case PPC_OP_CREQV:  emit_cr_logical(out, inst, "~(a ^ b)"); break;
    case PPC_OP_CRNAND: emit_cr_logical(out, inst, "~(a & b)"); break;
    case PPC_OP_CRNOR:  emit_cr_logical(out, inst, "~(a | b)"); break;
    case PPC_OP_CROR:   emit_cr_logical(out, inst, "a | b"); break;
    case PPC_OP_CRORC:  emit_cr_logical(out, inst, "a | ~b"); break;
    case PPC_OP_CRXOR:  emit_cr_logical(out, inst, "a ^ b"); break;

    case PPC_OP_MCRF: {
        u32 dst_shift = cr_field_shift(inst->crfD);
        u32 src_shift = cr_field_shift(inst->crfS);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 bits = (ctx->cr >> %u) & 0xFu;\n", src_shift);
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (bits << %u);\n",
                dst_shift, dst_shift);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MCRXR: {
        u32 dst_shift = cr_field_shift(inst->crfD);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 bits = (ctx->xer >> 28) & 0xFu;\n");
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (bits << %u);\n",
                dst_shift, dst_shift);
        fprintf(out, "        ctx->xer &= ~0xE0000000u;\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MFCR:
        fprintf(out, "    ctx->gpr[%u] = ctx->cr;\n", inst->rD);
        break;

    case PPC_OP_MTCRF: {
        u32 mask = 0;
        for (u32 crf = 0; crf < 8; crf++) {
            if (inst->crm & (0x80u >> crf))
                mask |= 0xFu << cr_field_shift((u8)crf);
        }
        if (mask) {
            fprintf(out, "    ctx->cr = (ctx->cr & ~0x%08Xu) | (ctx->gpr[%u] & 0x%08Xu);\n",
                    mask, inst->rS, mask);
        } else {
            fprintf(out, "    // mtcrf mask selects no CR fields\n");
        }
        break;
    }

    case PPC_OP_MFMSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->msr;\n", inst->rD);
        break;

    case PPC_OP_MTMSR:
        fprintf(out, "    ctx->msr = ctx->gpr[%u];\n", inst->rS);
        if (g_homed)
            fprintf(out, "    msr_ = ctx->gpr[%u];\n", inst->rS);
        break;

    case PPC_OP_MFSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[%u];\n", inst->rD, inst->sr);
        break;

    case PPC_OP_MFSRIN:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu];\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_MTSR:
        fprintf(out, "    ctx->sr[%u] = ctx->gpr[%u];\n", inst->sr, inst->rS);
        break;

    case PPC_OP_MTSRIN:
        fprintf(out, "    ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu] = ctx->gpr[%u];\n",
                inst->rB, inst->rS);
        break;

    case PPC_OP_MFTB:
        fprintf(out, "    ctx->gpr[%u] = ppc_mftb(ctx, %uu, 0x%08Xu);\n",
                inst->rD, inst->spr, inst->address);
        fprintf(out, "    if (ctx->exception) return;\n");
        break;

    case PPC_OP_MFSPR:
        switch (inst->spr) {
        case 1: fprintf(out, "    ctx->gpr[%u] = ctx->xer;\n", inst->rD); break;
        case 8: fprintf(out, "    ctx->gpr[%u] = ctx->lr;\n", inst->rD); break;
        case 9: fprintf(out, "    ctx->gpr[%u] = ctx->ctr;\n", inst->rD); break;
        case 26: fprintf(out, "    ctx->gpr[%u] = ctx->srr0;\n", inst->rD); break;
        case 27: fprintf(out, "    ctx->gpr[%u] = ctx->srr1;\n", inst->rD); break;
        case 268:
        case 269:
            fprintf(out, "    ctx->gpr[%u] = ppc_mftb(ctx, %uu, 0x%08Xu);\n",
                    inst->rD, inst->spr, inst->address);
            fprintf(out, "    if (ctx->exception) return;\n");
            break;
        case 912: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[0];\n", inst->rD); break;
        case 913: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[1];\n", inst->rD); break;
        case 914: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[2];\n", inst->rD); break;
        case 915: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[3];\n", inst->rD); break;
        case 916: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[4];\n", inst->rD); break;
        case 917: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[5];\n", inst->rD); break;
        case 918: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[6];\n", inst->rD); break;
        case 919: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[7];\n", inst->rD); break;
        case 282: fprintf(out, "    ctx->gpr[%u] = ctx->ear;\n", inst->rD); break;
        case 920: fprintf(out, "    ctx->gpr[%u] = ctx->hid2;\n", inst->rD); break;
        default:
            emit_fallback_instruction(out, inst);
            break;
        }
        break;

    case PPC_OP_MTSPR:
        switch (inst->spr) {
        case 1: fprintf(out, "    ctx->xer = ctx->gpr[%u];\n", inst->rS); break;
        case 8: fprintf(out, "    ctx->lr = ctx->gpr[%u];\n", inst->rS); break;
        case 9: fprintf(out, "    ctx->ctr = ctx->gpr[%u];\n", inst->rS); break;
        case 26: fprintf(out, "    ctx->srr0 = ctx->gpr[%u];\n", inst->rS); break;
        case 27: fprintf(out, "    ctx->srr1 = ctx->gpr[%u];\n", inst->rS); break;
        case 282: fprintf(out, "    ctx->ear = ctx->gpr[%u];\n", inst->rS); break;
        case 912: fprintf(out, "    ctx->gqr[0] = ctx->gpr[%u];\n", inst->rS); break;
        case 913: fprintf(out, "    ctx->gqr[1] = ctx->gpr[%u];\n", inst->rS); break;
        case 914: fprintf(out, "    ctx->gqr[2] = ctx->gpr[%u];\n", inst->rS); break;
        case 915: fprintf(out, "    ctx->gqr[3] = ctx->gpr[%u];\n", inst->rS); break;
        case 916: fprintf(out, "    ctx->gqr[4] = ctx->gpr[%u];\n", inst->rS); break;
        case 917: fprintf(out, "    ctx->gqr[5] = ctx->gpr[%u];\n", inst->rS); break;
        case 918: fprintf(out, "    ctx->gqr[6] = ctx->gpr[%u];\n", inst->rS); break;
        case 919: fprintf(out, "    ctx->gqr[7] = ctx->gpr[%u];\n", inst->rS); break;
        case 920: fprintf(out, "    ctx->hid2 = ctx->gpr[%u];\n", inst->rS); break;
        default:
            emit_fallback_instruction(out, inst);
            break;
        }
        break;

    case PPC_OP_TLBIE:
        fprintf(out, "    ppc_tlbie(ctx, ctx->gpr[%u], 0x%08Xu);\n", inst->rB, inst->address);
        fprintf(out, "    if (ctx->exception) return;\n");
        break;

    case PPC_OP_SYNC:
    case PPC_OP_EIEIO:
    case PPC_OP_ISYNC:
    case PPC_OP_TLBSYNC:
        fprintf(out, "    ppc_memory_fence();\n");
        break;

    case PPC_OP_ECIWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        u32 value = ppc_eciwx(ctx, ea, 0x%08Xu);\n", inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
        fprintf(out, "        ctx->gpr[%u] = value;\n", inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ECOWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_ecowx(ctx, ea, ctx->gpr[%u], 0x%08Xu);\n",
                inst->rS, inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
        fprintf(out, "    }\n");
        break;

    default:
        emit_fallback_instruction(out, inst);
        break;
    }

    fprintf(out, "\n");
}

void emit_instruction(FILE* out, const PPCInst* inst) {
    emit_instruction_with_range(out, inst, 0, (u32)-1, false, false);
}

// --- homed registers: the textual rewrite ----------------------------------
//
// The body of a function is emitted into a memory buffer first; this pass
// then rewrites it and, from the result, learns which registers the function
// touches so the prologue, the spill and the reload can be exact.

typedef struct {
    char* data;
    size_t len, cap;
} StrBuf;

static void sb_put(StrBuf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        b->data = (char*)realloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void sb_puts(StrBuf* b, const char* s) { sb_put(b, s, strlen(s)); }

static bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool starts(const char* p, const char* end, const char* lit) {
    size_t n = strlen(lit);
    return (size_t)(end - p) >= n && memcmp(p, lit, n) == 0;
}

// Registers the rewritten body names, and whether each is assigned.
typedef struct {
    bool used[32], dirty[32];
    bool fused[32], fdirty[32];   /* fpr */
    bool pused[32], pdirty[32];   /* ps1 */
    bool cr_used, cr_dirty, xer_used, xer_dirty;
    bool lr_used, lr_dirty, ctr_used, ctr_dirty;
    bool msr_used;   /* read through a local, written through to ctx */
} HomedSet;

// Whether the text after a register token assigns to it.
static bool assigns_after(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end) return false;
    if (p[0] == '=' && (p + 1 >= end || p[1] != '=')) return true;
    if (p + 1 < end && p[1] == '=' &&
        (p[0] == '+' || p[0] == '-' || p[0] == '&' || p[0] == '|' || p[0] == '^'))
        return true;
    if (p + 2 < end && p[2] == '=' &&
        ((p[0] == '<' && p[1] == '<') || (p[0] == '>' && p[1] == '>')))
        return true;
    if (p + 1 < end && ((p[0] == '+' && p[1] == '+') || (p[0] == '-' && p[1] == '-')))
        return true;
    return false;
}

static void homed_rewrite(const char* in, size_t len, StrBuf* out, HomedSet* set,
                          bool reset, const char* spill) {
    const char* p = in;
    const char* end = in + len;
    if (reset)
        memset(set, 0, sizeof(*set));
    while (p < end) {
        bool boundary = (p == in) || !ident_char(p[-1]);
        if (boundary && (starts(p, end, "ctx->gpr[") ||
                         (g_homed_fpr && (starts(p, end, "ctx->fpr[") || starts(p, end, "ctx->ps1["))))) {
            const char bank = p[5];  /* g, f or p */
            const char* q = p + 9;
            u32 n = 0;
            int digits = 0;
            while (q < end && *q >= '0' && *q <= '9') { n = n * 10 + (u32)(*q - '0'); q++; digits++; }
            if (digits && q < end && *q == ']' && n < 32) {
                char tmp[16];
                bool d = assigns_after(q + 1, end);
                if (bank == 'g') {
                    snprintf(tmp, sizeof(tmp), "r_%u", n);
                    set->used[n] = true;
                    if (d) set->dirty[n] = true;
                } else if (bank == 'f') {
                    snprintf(tmp, sizeof(tmp), "f_%u", n);
                    set->fused[n] = true;
                    if (d) set->fdirty[n] = true;
                } else {
                    snprintf(tmp, sizeof(tmp), "p_%u", n);
                    set->pused[n] = true;
                    if (d) set->pdirty[n] = true;
                }
                sb_puts(out, tmp);
                p = q + 1;
                continue;
            }
            // ctx->gpr[reg] with a run-time index stays an array access; the
            // emitter has spilled and will reload around it.
        }
        if (boundary && starts(p, end, "ctx->msr") && (p + 8 >= end || !ident_char(p[8])) &&
            !assigns_after(p + 8, end)) {
            sb_puts(out, "msr_");
            set->msr_used = true;
            p += 8;
            continue;
        }
        // The emitter's own refresh after mtmsr (`msr_ = ...`) is a use too:
        // a chunk whose only MSR traffic is an mtmsr otherwise gets the
        // assignment and no declaration (found by the 32-instruction layout).
        if (boundary && starts(p, end, "msr_") && (p + 4 >= end || !ident_char(p[4]))) {
            sb_puts(out, "msr_");
            set->msr_used = true;
            p += 4;
            continue;
        }
        if (boundary && starts(p, end, "ctx->")) {
            static const struct { const char* field; const char* local; int which; } fields[] = {
                {"ctr", "ctr_", 3}, {"cr", "cr_", 0}, {"xer", "xer_", 1}, {"lr", "lr_", 2},
            };
            bool done = false;
            for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
                size_t fl = strlen(fields[i].field);
                const char* q = p + 5;
                if ((size_t)(end - q) >= fl && memcmp(q, fields[i].field, fl) == 0 &&
                    (q + fl >= end || !ident_char(q[fl]))) {
                    sb_puts(out, fields[i].local);
                    bool d = assigns_after(q + fl, end);
                    switch (fields[i].which) {
                    case 0: set->cr_used = true; if (d) set->cr_dirty = true; break;
                    case 1: set->xer_used = true; if (d) set->xer_dirty = true; break;
                    case 2: set->lr_used = true; if (d) set->lr_dirty = true; break;
                    default: set->ctr_used = true; if (d) set->ctr_dirty = true; break;
                    }
                    p = q + fl;
                    done = true;
                    break;
                }
            }
            if (done) continue;
        }
        if (boundary && (starts(p, end, "mem_read") || starts(p, end, "mem_write"))) {
            // mem_read32(ctx,  ->  hmem_read32(ctx, ram_, ram_size_, cia_,
            const char* q = p;
            while (q < end && ident_char(*q)) q++;
            if (starts(q, end, "(ctx, ")) {
                sb_puts(out, "h");
                sb_put(out, p, (size_t)(q - p));
                sb_puts(out, "(ctx, ram_, ram_size_, cia_, ");
                p = q + 6;
                continue;
            }
        }
        if (boundary && starts(p, end, "return;")) {
            sb_puts(out, "{ ");
            sb_puts(out, spill);
            sb_puts(out, "(); return; }");
            p += 7;
            continue;
        }
        if (boundary && starts(p, end, "DOLRECOMP_TAIL_CALL(")) {
            sb_puts(out, spill);
            sb_puts(out, "(); DOLRECOMP_TAIL_CALL(");
            p += 20;
            continue;
        }
        // A spill the emitter placed by name (around a helper that reads the
        // register file) takes this site's set too.
        if (boundary && starts(p, end, "DOLRECOMP_SPILL()")) {
            sb_puts(out, spill);
            sb_puts(out, "()");
            p += 17;
            continue;
        }
        // A call into another chunk, a loop helper or the chunk table: the
        // callee reads ctx and leaves its result there.
        if (boundary && (starts(p, end, "func_") || starts(p, end, "loop_") ||
                         starts(p, end, "dolrecomp_chunk_table[chunk]"))) {
            const char* q = p;
            if (starts(p, end, "dolrecomp_chunk_table[chunk]")) {
                q = p + strlen("dolrecomp_chunk_table[chunk]");
            } else {
                q = p + 5;
                int hex = 0;
                while (q < end && ((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'F'))) { q++; hex++; }
                if (hex != 8) q = NULL;
            }
            if (q && starts(q, end, "(ctx);")) {
                sb_puts(out, spill);
                sb_puts(out, "(); ");
                sb_put(out, p, (size_t)(q + 6 - p));
                sb_puts(out, " DOLRECOMP_RELOAD();");
                p = q + 6;
                continue;
            }
        }
        sb_put(out, p, 1);
        p++;
    }
}

// --- per-site spill sets ------------------------------------------------------
//
// A spill site only has to store the registers that may have been assigned
// since the last point the register file was known to be in memory: function
// entry, or a reload after a call. The chunk enters through a switch at any
// instruction, so "since entry" means: assigned by some instruction that can
// reach this one along the chunk's own edges without passing a reload. That
// is a reachability fixpoint over a 64-instruction graph, computed from the
// emitted text of each instruction rather than from the decoder so it sees
// every `goto` the emitter actually wrote. Over-approximate on anything
// unclear; a missing store is silent guest corruption.
//
// (Leaving it to the C compiler was tried first, as a compare against the
// entry value -- `if (f != f_in) store` -- hoping it folds where nothing
// reaches: it does not, the phis defeat it, and the hot FP chunk went from
// 9009 to 28355 wasm ops with every spill site unique and unmerged.)
typedef struct {
    u32 gpr, fpr, ps1;
    u8 misc;  /* 1 cr, 2 xer, 4 lr, 8 ctr */
} RegSet;

static RegSet regset_of(const HomedSet* set) {
    RegSet r = {0, 0, 0, 0};
    for (u32 n = 0; n < 32; ++n) {
        if (set->dirty[n]) r.gpr |= 1u << n;
        if (set->fdirty[n]) r.fpr |= 1u << n;
        if (set->pdirty[n]) r.ps1 |= 1u << n;
    }
    if (set->cr_dirty) r.misc |= 1;
    if (set->xer_dirty) r.misc |= 2;
    if (set->lr_dirty) r.misc |= 4;
    if (set->ctr_dirty) r.misc |= 8;
    return r;
}

static bool regset_union_into(RegSet* dst, const RegSet* src) {
    RegSet before = *dst;
    dst->gpr |= src->gpr;
    dst->fpr |= src->fpr;
    dst->ps1 |= src->ps1;
    dst->misc |= src->misc;
    return dst->gpr != before.gpr || dst->fpr != before.fpr || dst->ps1 != before.ps1 ||
           dst->misc != before.misc;
}

static void emit_spill_macro(FILE* out, const char* name, const RegSet* r,
                             const HomedSet* all, u32 func_addr, u32 site) {
    fprintf(out, "#define %s() do { ", name);
    for (u32 n = 0; n < 32; ++n)
        if (r->gpr & (1u << n)) fprintf(out, "ctx->gpr[%u] = r_%u; ", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (r->fpr & (1u << n)) fprintf(out, "ctx->fpr[%u] = f_%u; ", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (r->ps1 & (1u << n)) fprintf(out, "ctx->ps1[%u] = p_%u; ", n, n);
    if (r->misc & 1) fprintf(out, "ctx->cr = cr_; ");
    if (r->misc & 2) fprintf(out, "ctx->xer = xer_; ");
    if (r->misc & 4) fprintf(out, "ctx->lr = lr_; ");
    if (r->misc & 8) fprintf(out, "ctx->ctr = ctr_; ");
    if (g_homed_verify && all) {
        // The complement: what this site does not store must already match.
        for (u32 n = 0; n < 32; ++n)
            if (all->dirty[n] && !(r->gpr & (1u << n)))
                fprintf(out, "if (ctx->gpr[%u] != r_%u) dolrecomp_homed_mismatch(0x%08Xu, %uu, \"r%u\"); ",
                        n, n, func_addr, site, n);
        for (u32 n = 0; n < 32; ++n)
            if (all->fdirty[n] && !(r->fpr & (1u << n)))
                fprintf(out, "if (dolrecomp_f64_to_bits(ctx->fpr[%u]) != dolrecomp_f64_to_bits(f_%u)) dolrecomp_homed_mismatch(0x%08Xu, %uu, \"f%u\"); ",
                        n, n, func_addr, site, n);
        for (u32 n = 0; n < 32; ++n)
            if (all->pdirty[n] && !(r->ps1 & (1u << n)))
                fprintf(out, "if (dolrecomp_f64_to_bits(ctx->ps1[%u]) != dolrecomp_f64_to_bits(p_%u)) dolrecomp_homed_mismatch(0x%08Xu, %uu, \"p%u\"); ",
                        n, n, func_addr, site, n);
        if (all->cr_dirty && !(r->misc & 1))
            fprintf(out, "if (ctx->cr != cr_) dolrecomp_homed_mismatch(0x%08Xu, %uu, \"cr\"); ", func_addr, site);
        if (all->xer_dirty && !(r->misc & 2))
            fprintf(out, "if (ctx->xer != xer_) dolrecomp_homed_mismatch(0x%08Xu, %uu, \"xer\"); ", func_addr, site);
        if (all->lr_dirty && !(r->misc & 4))
            fprintf(out, "if (ctx->lr != lr_) dolrecomp_homed_mismatch(0x%08Xu, %uu, \"lr\"); ", func_addr, site);
        if (all->ctr_dirty && !(r->misc & 8))
            fprintf(out, "if (ctx->ctr != ctr_) dolrecomp_homed_mismatch(0x%08Xu, %uu, \"ctr\"); ", func_addr, site);
    }
    fprintf(out, "} while (0)\n");
}

// Whether the emitted text of an instruction can fall through into the next.
static bool inst_falls_through(const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_B:
        return inst->lk;
    case PPC_OP_BCLR:
    case PPC_OP_BCCTR:
        return inst->lk || (inst->bo & 0x14u) != 0x14u;
    case PPC_OP_RFI:
    case PPC_OP_SC:
        return false;
    default:
        return true;
    }
}

// The per-function macros and the prologue that go with a rewritten body.
// DOLRECOMP_SPILL() stores every register the function assigns anywhere; the
// per-site DOLRECOMP_SPILL_<i>() macros below store the reach set instead.
static void emit_homed_macros(FILE* out, const HomedSet* set) {
    fprintf(out, "#define DOLRECOMP_SPILL() do { ");
    for (u32 n = 0; n < 32; ++n)
        if (set->dirty[n]) fprintf(out, "ctx->gpr[%u] = r_%u; ", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (set->fdirty[n]) fprintf(out, "ctx->fpr[%u] = f_%u; ", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (set->pdirty[n]) fprintf(out, "ctx->ps1[%u] = p_%u; ", n, n);
    if (set->cr_dirty) fprintf(out, "ctx->cr = cr_; ");
    if (set->xer_dirty) fprintf(out, "ctx->xer = xer_; ");
    if (set->lr_dirty) fprintf(out, "ctx->lr = lr_; ");
    if (set->ctr_dirty) fprintf(out, "ctx->ctr = ctr_; ");
    fprintf(out, "} while (0)\n");
    fprintf(out, "#define DOLRECOMP_RELOAD() do { ");
    for (u32 n = 0; n < 32; ++n)
        if (set->used[n]) fprintf(out, "r_%u = ctx->gpr[%u]; ", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (set->fused[n]) fprintf(out, "f_%u = ctx->fpr[%u]; ", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (set->pused[n]) fprintf(out, "p_%u = ctx->ps1[%u]; ", n, n);
    if (set->cr_used) fprintf(out, "cr_ = ctx->cr; ");
    if (set->xer_used) fprintf(out, "xer_ = ctx->xer; ");
    if (set->lr_used) fprintf(out, "lr_ = ctx->lr; ");
    if (set->ctr_used) fprintf(out, "ctr_ = ctx->ctr; ");
    if (set->msr_used) fprintf(out, "msr_ = ctx->msr; ");
    fprintf(out, "} while (0)\n");
}

static void emit_homed_prologue(FILE* out, const HomedSet* set, u32 address) {
    fprintf(out, "    u8* const ram_ = ctx->ram;\n");
    fprintf(out, "    const u32 ram_size_ = ctx->ram_size;\n");
    fprintf(out, "    u32 cia_ = 0x%08Xu;\n", address);
    fprintf(out, "    (void)ram_; (void)ram_size_; (void)cia_;\n");
    for (u32 n = 0; n < 32; ++n)
        if (set->used[n]) fprintf(out, "    u32 r_%u = ctx->gpr[%u];\n", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (set->fused[n]) fprintf(out, "    f64 f_%u = ctx->fpr[%u];\n", n, n);
    for (u32 n = 0; n < 32; ++n)
        if (set->pused[n]) fprintf(out, "    f64 p_%u = ctx->ps1[%u];\n", n, n);
    if (set->cr_used) fprintf(out, "    u32 cr_ = ctx->cr;\n");
    if (set->xer_used) fprintf(out, "    u32 xer_ = ctx->xer;\n");
    if (set->lr_used) fprintf(out, "    u32 lr_ = ctx->lr;\n");
    if (set->ctr_used) fprintf(out, "    u32 ctr_ = ctx->ctr;\n");
    if (set->msr_used) fprintf(out, "    u32 msr_ = ctx->msr;\n");
}

static void emit_homed_epilogue(FILE* out) {
    fprintf(out, "#undef DOLRECOMP_SPILL\n#undef DOLRECOMP_RELOAD\n\n");
}

// Emit a body through the rewrite: `body` is the text between the function's
// opening brace and its closing one. With `offsets` (one per instruction plus
// the start of the epilogue) the spill sites inside instruction i store the
// per-site set; without, every site stores the function's whole dirty set.
static void emit_homed_function(FILE* out, const char* signature, u32 address,
                                const char* body, size_t body_len,
                                const u32* offsets, u32 count,
                                const PPCInst* insts, const CFunctionCFG* cfg) {
    HomedSet set;
    if (!offsets) {
        StrBuf rewritten = {0};
        homed_rewrite(body, body_len, &rewritten, &set, true, "DOLRECOMP_SPILL");
        emit_homed_macros(out, &set);
        fprintf(out, "%s {\n", signature);
        emit_homed_prologue(out, &set, address);
        fwrite(rewritten.data, 1, rewritten.len, out);
        fprintf(out, "}\n");
        emit_homed_epilogue(out);
        free(rewritten.data);
        return;
    }

    RegSet* assigned = (RegSet*)calloc(count, sizeof(RegSet));
    RegSet* reach = (RegSet*)calloc(count, sizeof(RegSet));   // dirty on exit, for successors
    RegSet* site = (RegSet*)calloc(count, sizeof(RegSet));    // dirty at the sites inside i
    bool* kills = (bool*)calloc(count, sizeof(bool));
    StrBuf* slices = (StrBuf*)calloc(count + 1, sizeof(StrBuf));
    u8* has_site = (u8*)calloc(count, 1);
    memset(&set, 0, sizeof(set));

    // Rewrite each instruction on its own, naming its spill sites, and learn
    // what it assigns and whether it reloads.
    for (u32 i = 0; i < count; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "DOLRECOMP_SPILL_%u", i);
        HomedSet local;
        StrBuf scratch = {0};
        homed_rewrite(body + offsets[i], offsets[i + 1] - offsets[i], &scratch, &local, true, name);
        assigned[i] = regset_of(&local);
        // A reload makes the register file clean for whatever follows -- but
        // only if every path to the successors passes it. A cross-chunk call
        // reaches its continuation through the reload alone, and the lmw
        // family and the fallback reload unconditionally; the quantised
        // load/store reloads only on its slow path, so it is not a kill.
        const bool conditional_reload =
            insts[i].op == PPC_OP_PSQ_L || insts[i].op == PPC_OP_PSQ_LU ||
            insts[i].op == PPC_OP_PSQ_LX || insts[i].op == PPC_OP_PSQ_LUX ||
            insts[i].op == PPC_OP_PSQ_ST || insts[i].op == PPC_OP_PSQ_STU ||
            insts[i].op == PPC_OP_PSQ_STX || insts[i].op == PPC_OP_PSQ_STUX;
        kills[i] = !conditional_reload && scratch.data &&
                   strstr(scratch.data, "DOLRECOMP_RELOAD()") != NULL;
        has_site[i] = scratch.data && strstr(scratch.data, name) != NULL;
        free(scratch.data);
        // The function-wide set, accumulated: the prologue and the reload
        // want everything the function touches.
        homed_rewrite(body + offsets[i], offsets[i + 1] - offsets[i], &slices[i], &set, false, name);
    }
    // The epilogue -- the fallthrough into the next chunk, the return
    // dispatch -- is reachable from every blr, so it spills the whole set.
    homed_rewrite(body + offsets[count], body_len - offsets[count], &slices[count], &set, false,
                  "DOLRECOMP_SPILL");

    // Successors: the gotos the emitter wrote, the fallthrough, and for a
    // return routed through the dispatch switch, every return target.
    // reach[i] is what may be dirty on exit from i. A 64-instruction chunk
    // makes the quadratic predecessor walk cheap.
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 0; i < count; ++i) {
            RegSet in = {0, 0, 0, 0};
            for (u32 p = 0; p < count; ++p) {
                bool edge = false;
                if (p + 1 == i && inst_falls_through(&insts[p]))
                    edge = true;
                const char* t = body + offsets[p];
                const char* tend = body + offsets[p + 1];
                for (const char* q = t; !edge && q < tend;) {
                    const char* g = strstr(q, "goto label_");
                    if (!g || g >= tend)
                        break;
                    u32 target = (u32)strtoul(g + 11, NULL, 16);
                    if (target >= address && (target - address) / 4u == i)
                        edge = true;
                    q = g + 11;
                }
                if (!edge && cfg->return_targets[i]) {
                    const char* g = strstr(t, "goto return_dispatch_");
                    if (g && g < tend)
                        edge = true;
                }
                if (edge)
                    regset_union_into(&in, &reach[p]);
            }
            // What a spill site inside i must store: everything dirty on
            // entry plus i's own assignments. An instruction that reloads
            // spills before the call and reloads after it, so its sites need
            // the full set even though what it passes on to its successors
            // is only what it assigned itself.
            RegSet at_site = in;
            regset_union_into(&at_site, &assigned[i]);
            if (regset_union_into(&site[i], &at_site))
                changed = true;
            RegSet outset = kills[i] ? assigned[i] : at_site;
            if (regset_union_into(&reach[i], &outset))
                changed = true;
        }
    }

    emit_homed_macros(out, &set);
    for (u32 i = 0; i < count; ++i) {
        if (!has_site[i])
            continue;
        char name[32];
        snprintf(name, sizeof(name), "DOLRECOMP_SPILL_%u", i);
        emit_spill_macro(out, name, &site[i], &set, address, i);
    }
    // The entry switch sits before the first instruction's slice. Nothing in
    // it touches a register, but it goes through the rewrite like the rest so
    // its `default: return` spills nothing rather than everything.
    StrBuf prefix = {0};
    {
        HomedSet unused;
        homed_rewrite(body, offsets[0], &prefix, &unused, true, "DOLRECOMP_SPILL");
    }
    fprintf(out, "%s {\n", signature);
    emit_homed_prologue(out, &set, address);
    if (prefix.data)
        fwrite(prefix.data, 1, prefix.len, out);
    free(prefix.data);
    for (u32 i = 0; i <= count; ++i) {
        if (slices[i].data)
            fwrite(slices[i].data, 1, slices[i].len, out);
        free(slices[i].data);
    }
    fprintf(out, "}\n");
    for (u32 i = 0; i < count; ++i)
        if (has_site[i])
            fprintf(out, "#undef DOLRECOMP_SPILL_%u\n", i);
    emit_homed_epilogue(out);
    free(assigned);
    free(reach);
    free(site);
    free(kills);
    free(slices);
    free(has_site);
}

static void emit_counted_loop(FILE* out, const PPCInst* insts,
                              const CFunctionCFG* cfg, u32 function_address,
                              u32 function_end, u32 first, u32 last) {
    u32 loop_address = insts[first].address;
    u32 continuation = insts[last].address + 4u;
    FILE* body = out;
    char* buf = NULL;
    size_t buf_len = 0;
    if (g_homed) {
        body = open_memstream(&buf, &buf_len);
    } else {
        fprintf(out, "static void loop_%08X(CPUState* ctx) {\n", loop_address);
    }
    fprintf(body, "label_%08X:\n", loop_address);
    fprintf(body, "    ctx->downcount -= %u;\n", cfg->block_cycles[first]);
    for (u32 i = first; i <= last; ++i) {
        if (g_homed)
            fprintf(body, "    cia_ = 0x%08Xu;\n", insts[i].address);
        else if (cfg->materialize_pc[i])
            fprintf(body, "    ctx->pc = 0x%08Xu;\n", insts[i].address);
        emit_instruction_with_range(body, &insts[i], function_address,
                                    function_end, i == last, false);
    }
    fprintf(body, "    ctx->pc = 0x%08Xu;\n", continuation);
    if (g_homed) {
        fprintf(body, "    DOLRECOMP_SPILL();\n");
        fclose(body);
        char signature[64];
        snprintf(signature, sizeof(signature), "static void loop_%08X(CPUState* ctx)",
                 loop_address);
        emit_homed_function(out, signature, loop_address, buf, buf_len, NULL, 0, NULL, NULL);
        free(buf);
    } else {
        fprintf(out, "}\n\n");
    }
}

bool emit_function(FILE* out, const PPCInst* insts, u32 count, u32 func_addr) {
    u32 func_end = func_addr + count * 4u;
    CFunctionCFG cfg;
    if (!c_function_cfg_build(&cfg, insts, count, func_addr)) {
        fprintf(stderr, "error: out of memory while analyzing function %08X\n",
                func_addr);
        return false;
    }
    // DOLRECOMP_C_LOCAL_RETURNS=0 sends every blr back through the dispatcher
    // instead of routing it inside the function.
    bool has_local_returns = false;
    for (u32 i = 0; i < count; ++i)
        has_local_returns |= cfg.return_targets[i] != 0;

    for (u32 i = 0; i < count; ++i) {
        if (cfg.loop_ends[i] != UINT32_MAX)
            emit_counted_loop(out, insts, &cfg, func_addr, func_end, i,
                              cfg.loop_ends[i]);
    }

    FILE* body = out;
    char* buf = NULL;
    size_t buf_len = 0;
    u32* offsets = NULL;
    if (g_homed) {
        body = open_memstream(&buf, &buf_len);
        offsets = (u32*)calloc(count + 1, sizeof(u32));
    } else {
        fprintf(out, "void func_%08X(CPUState* ctx) {\n", func_addr);
    }
    fprintf(body, "    switch (ctx->pc) {\n");
    for (u32 i = 0; i < count; i++) {
        fprintf(body, "    case 0x%08Xu: goto label_%08X;\n",
                insts[i].address, insts[i].address);
    }
    // Nothing has been changed yet, so this exit needs no spill; the rewrite
    // would add one to a plain `return;`.
    fprintf(body, g_homed ? "    default: { return; }\n" : "    default: return;\n");
    fprintf(body, "    }\n");

    for (u32 i = 0; i < count; i++) {
        if (offsets) {
            fflush(body);
            offsets[i] = (u32)buf_len;
        }
        fprintf(body, "label_%08X:\n", insts[i].address);
        if (cfg.loop_ends[i] != UINT32_MAX) {
            u32 continuation = insts[cfg.loop_ends[i]].address + 4u;
            fprintf(body, "    loop_%08X(ctx);\n", insts[i].address);
            if (continuation < func_end) {
                fprintf(body, "    if (ctx->pc == 0x%08Xu) goto label_%08X;\n",
                        continuation, continuation);
            }
            fprintf(body, "    return;\n");
            continue;
        }
        if (g_homed)
            fprintf(body, "    cia_ = 0x%08Xu;\n", insts[i].address);
        else if (cfg.materialize_pc[i])
            fprintf(body, "    ctx->pc = 0x%08Xu;\n", insts[i].address);
        if (cfg.leaders[i] && cfg.block_cycles[i] != 0)
            fprintf(body, "    ctx->downcount -= %u;\n", cfg.block_cycles[i]);
        emit_instruction_with_range(
            body, &insts[i], func_addr, func_end,
            c_function_cfg_can_loop_directly(&cfg, insts, func_addr, i),
            has_local_returns);
    }

    if (offsets) {
        fflush(body);
        offsets[count] = (u32)buf_len;
    }
    // Falling off the end of a chunk is a transfer into the next one, and on a
    // title chunked at a fixed stride it is a hot one: a loop that straddles
    // the boundary crosses it every iteration. Tail into the neighbour rather
    // than hand every crossing to the chassis.
    fprintf(body, "    {\n");
    if (!emit_cross_chunk_tail_to(body, func_end)) {
        fprintf(body, "            ctx->pc = 0x%08Xu;\n", func_end);
        fprintf(body, "            return;\n");
    }
    fprintf(body, "    }\n");
    if (has_local_returns) {
        fprintf(body, "return_dispatch_%08X:\n", func_addr);
        emit_loop_guard(body, "    ", 0, true);
        fprintf(body, "    switch (ctx->pc) {\n");
        for (u32 i = 0; i < count; ++i) {
            if (cfg.return_targets[i]) {
                fprintf(body, "    case 0x%08Xu: goto label_%08X;\n",
                        insts[i].address, insts[i].address);
            }
        }
        fprintf(body, "    default: return;\n");
        fprintf(body, "    }\n");
    }
    if (g_homed) {
        fclose(body);
        char signature[64];
        snprintf(signature, sizeof(signature), "void func_%08X(CPUState* ctx)", func_addr);
        emit_homed_function(out, signature, func_addr, buf, buf_len, offsets, count, insts, &cfg);
        free(buf);
        free(offsets);
    } else {
        fprintf(out, "}\n\n");
    }
    c_function_cfg_destroy(&cfg);
    return true;
}
