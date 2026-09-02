/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GXRUNTIME_CPU_FP_HOMED_H
#define GXRUNTIME_CPU_FP_HOMED_H

/* Floating point by value, for generated code that keeps the FPRs in locals
 * (DOLRECOMP_HOMED_REGS).
 *
 * The interpreter's helpers take register *indices* and read and write
 * cpu->fpr[] / cpu->ps1[] themselves, which is exactly what generated code
 * with homed registers cannot allow: the register file is in C locals, and a
 * helper that reaches into CPUState would read stale memory and have its
 * writes overwritten by the next spill. So every operation here takes its
 * operands as values and hands its result back as a value; only FPSCR still
 * lives in CPUState, because every helper updates it in place and nothing
 * generated reads it between two FP instructions.
 *
 * The fast paths -- the same tests the interpreter's fast paths make, see
 * cpu_interpreter_float.c for why they are exactly these -- are inline so that
 * a fmuls whose operands are already in wasm locals is a multiply, three bit
 * tests and a convert, with no call. The exact paths (`_x`) are out of line
 * and return a struct: a pointer to a homed local would force that local into
 * memory for the whole function, and a struct comes back through a temporary
 * the caller never otherwise touches.
 *
 * The interpreter's index-based helpers are wrappers over these (cpu_interpreter_float.c),
 * so there is one fast path and one exact path per operation, and the
 * paired-single differential test covers both users. */

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Always inline, not merely `inline`: at -O3 clang left these as local
   functions (dozens of call sites each), and every FP op in the generated
   code became a call returning a struct through the stack -- three times the
   f64 traffic the homing had removed. Inlined, the struct is scalarised away. */
#if defined(_MSC_VER)
#define PPC_H_INLINE static __forceinline
#else
#define PPC_H_INLINE static inline __attribute__((always_inline))
#endif

typedef struct {
    f64 v;
    u32 ok;   /* the destination is written only when set */
} PPCFpr1;

typedef struct {
    f64 v0, v1;
} PPCFpr2;

#define PPC_FPSCR_H_NI 0x00000004u
#define PPC_FPSCR_H_FI 0x00020000u
#define PPC_FPSCR_H_FR 0x00040000u
#define PPC_FPSCR_H_EXP_MASK  0x7FF0000000000000ull
#define PPC_FPSCR_H_FRAC_MASK 0x000FFFFFFFFFFFFFull

PPC_H_INLINE u64 ppc_h_round_c_bits(u64 bits) {
    return (bits & 0xFFFFFFFFF8000000ull) + (bits & 0x0000000008000000ull);
}

/* The one C operand force_25bit_c does not handle with the mask-and-add above. */
PPC_H_INLINE u64 ppc_h_c_unusual(u64 bits) {
    return (u64)(((bits & PPC_FPSCR_H_EXP_MASK) == 0) & ((bits & PPC_FPSCR_H_FRAC_MASK) != 0));
}

PPC_H_INLINE u64 ppc_h_non_finite(u64 bits) {
    return (u64)((bits & PPC_FPSCR_H_EXP_MASK) == PPC_FPSCR_H_EXP_MASK);
}

/* Ties only arise out of the fused multiply-add forms. */
PPC_H_INLINE u64 ppc_h_r_unusual(u64 bits) {
    return (u64)(((bits & 0x1FFFFFFFull) == 0x10000000ull) |
                 ((bits & PPC_FPSCR_H_EXP_MASK) == PPC_FPSCR_H_EXP_MASK));
}

/* force_single's rule under FPSCR[NI]: anything below the smallest normal
   single becomes a signed zero. Gekko titles run with NI set, so the fast
   path implements the flush rather than declining it. */
PPC_H_INLINE f32 ppc_h_single(u32 ni, f64 value) {
    u64 bits = f64_bits(value);
    if (ni && (bits & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull)
        return f32_value((u32)((bits & 0x8000000000000000ull) >> 32));
    return (f32)value;
}

PPC_H_INLINE u32 ppc_h_classify_f32(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    u32 sign = bits >> 31;
    u32 exponent = bits & 0x7F800000u;
    u32 fraction = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

PPC_H_INLINE u32 ppc_h_classify_f64(f64 value) {
    u64 bits = f64_bits(value);
    u32 sign = (u32)(bits >> 63);
    u64 exponent = bits & PPC_FPSCR_H_EXP_MASK;
    u64 fraction = bits & PPC_FPSCR_H_FRAC_MASK;
    if (exponent == PPC_FPSCR_H_EXP_MASK)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

/* FPRF -- the result-class field of FPSCR -- is what Dolphin's JITs skip
   unless the "Enable FPRF" setting is on, which it is not by default: nothing
   a game does reads it back except through mffs/mcrfs, and no title is known
   to depend on it. GXRUNTIME_NO_FPRF makes the fast paths skip it too, which
   is a classify, a load, a mask and a store fewer per FP instruction; the
   exact paths still compute it. */
#if defined(GXRUNTIME_NO_FPRF)
#define ppc_h_set_fprf(cpu, value) ((void)(cpu), (void)(value))
#else
PPC_H_INLINE void ppc_h_set_fprf(CPUState* cpu, u32 value) {
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((value & 0x1Fu) << 12);
}
#endif

/* --- exact paths, out of line ------------------------------------------- */
PPCFpr1 ppc_fadds_x(CPUState* cpu, f64 a, f64 b);
PPCFpr1 ppc_fsubs_x(CPUState* cpu, f64 a, f64 b);
PPCFpr1 ppc_fmuls_x(CPUState* cpu, f64 a, f64 c);
PPCFpr1 ppc_fadd_x(CPUState* cpu, f64 a, f64 b);
PPCFpr1 ppc_fsub_x(CPUState* cpu, f64 a, f64 b);
PPCFpr1 ppc_fmul_x(CPUState* cpu, f64 a, f64 c);
PPCFpr1 ppc_fmadd_x(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
                    bool subtract, bool negative);
PPCFpr2 ppc_ps_add_x(CPUState* cpu, f64 a0, f64 a1, f64 b0, f64 b1);
PPCFpr2 ppc_ps_sub_x(CPUState* cpu, f64 a0, f64 a1, f64 b0, f64 b1);
PPCFpr2 ppc_ps_mul_x(CPUState* cpu, f64 a0, f64 a1, f64 c0, f64 c1);
PPCFpr2 ppc_ps_madd_x(CPUState* cpu, f64 a0, f64 a1, f64 c0, f64 c1, f64 b0, f64 b1,
                      bool subtract, bool negative);
PPCFpr2 ppc_ps_sum0_x(CPUState* cpu, f64 a0, f64 b1, f64 c1);
PPCFpr2 ppc_ps_sum1_x(CPUState* cpu, f64 a0, f64 b1, f64 c0);

/* --- operations with no fast path: out of line, by value ------------------- */
PPCFpr1 ppc_fdivs_h(CPUState* cpu, f64 a, f64 b);
PPCFpr1 ppc_fdiv_h(CPUState* cpu, f64 a, f64 b);
PPCFpr1 ppc_frsp_h(CPUState* cpu, f64 b);
PPCFpr1 ppc_fres_h(CPUState* cpu, f64 b);
PPCFpr1 ppc_frsqrte_h(CPUState* cpu, f64 b);
PPCFpr1 ppc_fctiw_h(CPUState* cpu, f64 b, bool toward_zero);
PPCFpr2 ppc_ps_div_h(CPUState* cpu, f64 a0, f64 a1, f64 b0, f64 b1);
PPCFpr2 ppc_ps_res_h(CPUState* cpu, f64 b0, f64 b1);
PPCFpr2 ppc_ps_rsqrte_h(CPUState* cpu, f64 b0, f64 b1);

/* --- the fast paths ------------------------------------------------------ */

/* The result goes out through a pointer to a temporary the generated code
   declares next to the call; the cold path's struct is a *different*
   temporary, touched only there. With the struct returned from the inline
   function itself, the temporary the exact path's sret pointed at was the one
   the fast path also had to write, so every FP op stored its result to the
   stack and loaded it back -- the traffic this file exists to remove. */
PPC_H_INLINE bool ppc_h_cold1(PPCFpr1 x, f64* out) {
    if (x.ok)
        *out = x.v;
    return x.ok != 0;
}

/* Scalar single precision: the result is written to both lanes. */
PPC_H_INLINE bool ppc_fadds_h(CPUState* cpu, f64 a, f64 b, f64* out) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    f64 r = a + b;
    if (!ppc_h_non_finite(f64_bits(r))) {
        f32 s = ppc_h_single(ni, r);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s));
        *out = (f64)s;
        return true;
    }
    return ppc_h_cold1(ppc_fadds_x(cpu, a, b), out);
}

PPC_H_INLINE bool ppc_fsubs_h(CPUState* cpu, f64 a, f64 b, f64* out) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    f64 r = a - b;
    if (!ppc_h_non_finite(f64_bits(r))) {
        f32 s = ppc_h_single(ni, r);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s));
        *out = (f64)s;
        return true;
    }
    return ppc_h_cold1(ppc_fsubs_x(cpu, a, b), out);
}

PPC_H_INLINE bool ppc_fmuls_h(CPUState* cpu, f64 a, f64 c, f64* out) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    u64 cb = f64_bits(c);
    f64 r = a * f64_value(ppc_h_round_c_bits(cb));
    if (!(ppc_h_c_unusual(cb) | ppc_h_non_finite(f64_bits(r)))) {
        f32 s = ppc_h_single(ni, r);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s));
        cpu->fpscr &= ~(PPC_FPSCR_H_FI | PPC_FPSCR_H_FR);
        *out = (f64)s;
        return true;
    }
    return ppc_h_cold1(ppc_fmuls_x(cpu, a, c), out);
}

/* Scalar double precision: lane 0 only. */
PPC_H_INLINE bool ppc_fadd_h(CPUState* cpu, f64 a, f64 b, f64* out) {
    f64 r = a + b;
    if (!ppc_h_non_finite(f64_bits(r))) {
        ppc_h_set_fprf(cpu, ppc_h_classify_f64(r));
        *out = r;
        return true;
    }
    return ppc_h_cold1(ppc_fadd_x(cpu, a, b), out);
}

PPC_H_INLINE bool ppc_fsub_h(CPUState* cpu, f64 a, f64 b, f64* out) {
    f64 r = a - b;
    if (!ppc_h_non_finite(f64_bits(r))) {
        ppc_h_set_fprf(cpu, ppc_h_classify_f64(r));
        *out = r;
        return true;
    }
    return ppc_h_cold1(ppc_fsub_x(cpu, a, b), out);
}

PPC_H_INLINE bool ppc_fmul_h(CPUState* cpu, f64 a, f64 c, f64* out) {
    f64 r = a * c;
    if (!ppc_h_non_finite(f64_bits(r))) {
        ppc_h_set_fprf(cpu, ppc_h_classify_f64(r));
        cpu->fpscr &= ~(PPC_FPSCR_H_FI | PPC_FPSCR_H_FR);
        *out = r;
        return true;
    }
    return ppc_h_cold1(ppc_fmul_x(cpu, a, c), out);
}

/* The operations with no fast path, in the same shape. */
PPC_H_INLINE bool ppc_fdivs_hi(CPUState* cpu, f64 a, f64 b, f64* out) {
    return ppc_h_cold1(ppc_fdivs_h(cpu, a, b), out);
}
PPC_H_INLINE bool ppc_fdiv_hi(CPUState* cpu, f64 a, f64 b, f64* out) {
    return ppc_h_cold1(ppc_fdiv_h(cpu, a, b), out);
}
PPC_H_INLINE bool ppc_frsp_hi(CPUState* cpu, f64 b, f64* out) {
    return ppc_h_cold1(ppc_frsp_h(cpu, b), out);
}
PPC_H_INLINE bool ppc_fres_hi(CPUState* cpu, f64 b, f64* out) {
    return ppc_h_cold1(ppc_fres_h(cpu, b), out);
}
PPC_H_INLINE bool ppc_frsqrte_hi(CPUState* cpu, f64 b, f64* out) {
    return ppc_h_cold1(ppc_frsqrte_h(cpu, b), out);
}
PPC_H_INLINE bool ppc_fctiw_hi(CPUState* cpu, f64 b, bool toward_zero, f64* out) {
    return ppc_h_cold1(ppc_fctiw_h(cpu, b, toward_zero), out);
}

/* The fused forms. `single` writes both lanes, double lane 0 only; the
   caller decides from the same flag. gekko_fma is the exact fma the
   interpreter uses (an fma is bit-identical to a multiply-add whenever the
   product is exact, and wasm has no fma instruction). */
#include "../../../DolRecomp/src/cpu/gekko_fma.h"

PPC_H_INLINE bool ppc_fmadd_h(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
                              bool subtract, bool negative, f64* out) {
    const f64 sb = subtract ? -b : b;
    if (single) {
        const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
        u64 cb = f64_bits(c);
        f64 r = gekko_fma(a, f64_value(ppc_h_round_c_bits(cb)), sb);
        if (!(ppc_h_c_unusual(cb) | ppc_h_r_unusual(f64_bits(r)))) {
            f32 tmp = ppc_h_single(ni, r);
            f32 result = negative ? -tmp : tmp;
            if (!subtract && !negative) {
                cpu->fpscr = (cpu->fpscr & ~(PPC_FPSCR_H_FI | PPC_FPSCR_H_FR)) |
                             ((r != (f64)tmp) ? PPC_FPSCR_H_FI : 0u);
            }
            ppc_h_set_fprf(cpu, ppc_h_classify_f32(result));
            *out = (f64)result;
            return true;
        }
    } else {
        f64 r = gekko_fma(a, c, sb);
        if (!ppc_h_non_finite(f64_bits(r))) {
            f64 result = negative ? -r : r;
            ppc_h_set_fprf(cpu, ppc_h_classify_f64(result));
            *out = result;
            return true;
        }
    }
    return ppc_h_cold1(ppc_fmadd_x(cpu, a, c, b, single, subtract, negative), out);
}

/* Paired single. Every one of these writes both lanes, through two out
   pointers for the same reason as above. */
PPC_H_INLINE void ppc_h_cold2(PPCFpr2 x, f64* o0, f64* o1) {
    *o0 = x.v0;
    *o1 = x.v1;
}

PPC_H_INLINE void ppc_ps_add_h(CPUState* cpu, f64 a0, f64 a1, f64 b0, f64 b1, f64* o0, f64* o1) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    f64 r0 = a0 + b0;
    f64 r1 = a1 + b1;
    if (!(ppc_h_non_finite(f64_bits(r0)) | ppc_h_non_finite(f64_bits(r1)))) {
        f32 s0 = ppc_h_single(ni, r0);
        f32 s1 = ppc_h_single(ni, r1);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s0));
        *o0 = (f64)s0;
        *o1 = (f64)s1;
        return;
    }
    ppc_h_cold2(ppc_ps_add_x(cpu, a0, a1, b0, b1), o0, o1);
}

PPC_H_INLINE void ppc_ps_sub_h(CPUState* cpu, f64 a0, f64 a1, f64 b0, f64 b1, f64* o0, f64* o1) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    f64 r0 = a0 - b0;
    f64 r1 = a1 - b1;
    if (!(ppc_h_non_finite(f64_bits(r0)) | ppc_h_non_finite(f64_bits(r1)))) {
        f32 s0 = ppc_h_single(ni, r0);
        f32 s1 = ppc_h_single(ni, r1);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s0));
        *o0 = (f64)s0;
        *o1 = (f64)s1;
        return;
    }
    ppc_h_cold2(ppc_ps_sub_x(cpu, a0, a1, b0, b1), o0, o1);
}

/* ps_mul; ps_muls0 is this with (c0, c0) and ps_muls1 with (c1, c1). */
PPC_H_INLINE void ppc_ps_mul_h(CPUState* cpu, f64 a0, f64 a1, f64 c0, f64 c1, f64* o0, f64* o1) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    u64 cb0 = f64_bits(c0);
    u64 cb1 = f64_bits(c1);
    f64 r0 = a0 * f64_value(ppc_h_round_c_bits(cb0));
    f64 r1 = a1 * f64_value(ppc_h_round_c_bits(cb1));
    if (!(ppc_h_c_unusual(cb0) | ppc_h_c_unusual(cb1) |
          ppc_h_non_finite(f64_bits(r0)) | ppc_h_non_finite(f64_bits(r1)))) {
        f32 s0 = ppc_h_single(ni, r0);
        f32 s1 = ppc_h_single(ni, r1);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s0));
        *o0 = (f64)s0;
        *o1 = (f64)s1;
        return;
    }
    ppc_h_cold2(ppc_ps_mul_x(cpu, a0, a1, c0, c1), o0, o1);
}

/* ps_madd/msub/nmadd/nmsub; ps_madds0 is (c0, c0) and ps_madds1 (c1, c1),
   with subtract and negative false. */
PPC_H_INLINE void ppc_ps_madd_h(CPUState* cpu, f64 a0, f64 a1, f64 c0, f64 c1,
                                f64 b0, f64 b1, bool subtract, bool negative,
                                f64* o0, f64* o1) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    u64 cb0 = f64_bits(c0);
    u64 cb1 = f64_bits(c1);
    f64 r0 = gekko_fma(a0, f64_value(ppc_h_round_c_bits(cb0)), subtract ? -b0 : b0);
    f64 r1 = gekko_fma(a1, f64_value(ppc_h_round_c_bits(cb1)), subtract ? -b1 : b1);
    u64 rb0 = f64_bits(r0);
    u64 rb1 = f64_bits(r1);
    if (!(ppc_h_c_unusual(cb0) | ppc_h_c_unusual(cb1) | ppc_h_r_unusual(rb0) |
          ppc_h_r_unusual(rb1))) {
        f32 t0 = ppc_h_single(ni, r0);
        f32 t1 = ppc_h_single(ni, r1);
        f32 s0 = negative ? -t0 : t0;
        f32 s1 = negative ? -t1 : t1;
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s0));
        *o0 = (f64)s0;
        *o1 = (f64)s1;
        return;
    }
    ppc_h_cold2(ppc_ps_madd_x(cpu, a0, a1, c0, c1, b0, b1, subtract, negative), o0, o1);
}

/* ps_sum0: lane 0 is a0 + b1, lane 1 is c1. */
PPC_H_INLINE void ppc_ps_sum0_h(CPUState* cpu, f64 a0, f64 b1, f64 c1, f64* o0, f64* o1) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    f64 r0 = a0 + b1;
    if (!ppc_h_non_finite(f64_bits(r0))) {
        f32 s0 = ppc_h_single(ni, r0);
        f32 s1 = ppc_h_single(ni, c1);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s0));
        *o0 = (f64)s0;
        *o1 = (f64)s1;
        return;
    }
    ppc_h_cold2(ppc_ps_sum0_x(cpu, a0, b1, c1), o0, o1);
}

/* ps_sum1: lane 0 is c0, lane 1 is a0 + b1, and FPRF reports lane 1. */
PPC_H_INLINE void ppc_ps_sum1_h(CPUState* cpu, f64 a0, f64 b1, f64 c0, f64* o0, f64* o1) {
    const u32 ni = cpu->fpscr & PPC_FPSCR_H_NI;
    f64 r1 = a0 + b1;
    if (!ppc_h_non_finite(f64_bits(r1))) {
        f32 s0 = ppc_h_single(ni, c0);
        f32 s1 = ppc_h_single(ni, r1);
        ppc_h_set_fprf(cpu, ppc_h_classify_f32(s1));
        *o0 = (f64)s0;
        *o1 = (f64)s1;
        return;
    }
    ppc_h_cold2(ppc_ps_sum1_x(cpu, a0, b1, c0), o0, o1);
}

PPC_H_INLINE void ppc_ps_div_hi(CPUState* cpu, f64 a0, f64 a1, f64 b0, f64 b1, f64* o0, f64* o1) {
    ppc_h_cold2(ppc_ps_div_h(cpu, a0, a1, b0, b1), o0, o1);
}
PPC_H_INLINE void ppc_ps_res_hi(CPUState* cpu, f64 b0, f64 b1, f64* o0, f64* o1) {
    ppc_h_cold2(ppc_ps_res_h(cpu, b0, b1), o0, o1);
}
PPC_H_INLINE void ppc_ps_rsqrte_hi(CPUState* cpu, f64 b0, f64 b1, f64* o0, f64* o1) {
    ppc_h_cold2(ppc_ps_rsqrte_h(cpu, b0, b1), o0, o1);
}

/* --- quantised loads and stores, by value ---------------------------------
 *
 * The fast path is the unquantised one (GQR type 0), through the homed memory
 * helpers; anything else goes to the interpreter's ppc_psq_load/store, which
 * read and write cpu->fpr[]/ps1[] by index, so the generated code spills
 * around that call. */
PPC_H_INLINE bool hpsq_load_fast(CPUState* cpu, u8* ram, u32 ram_size, u32 cia,
                                  u32 ea, bool w, u8 gqr_index, bool indexed,
                                  f64* o0, f64* o1) {
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if (((gqr >> 16) & 7u) == 0u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u)) {
        *o0 = f64_value(convert_to_double(hmem_read32(cpu, ram, ram_size, cia, ea)));
        *o1 = w ? 1.0 : f64_value(convert_to_double(
                            hmem_read32(cpu, ram, ram_size, cia, ea + 4u)));
        return true;
    }
    return false;
}

PPC_H_INLINE bool hpsq_store_fast(CPUState* cpu, u8* ram, u32 ram_size, u32 cia,
                                   u32 ea, bool w, u8 gqr_index, bool indexed,
                                   f64 v0, f64 v1) {
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if ((gqr & 7u) == 0u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u)) {
        hmem_write32(cpu, ram, ram_size, cia, ea, convert_to_single_ftz(f64_bits(v0)));
        if (!w)
            hmem_write32(cpu, ram, ram_size, cia, ea + 4u,
                         convert_to_single_ftz(f64_bits(v1)));
        return true;
    }
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* GXRUNTIME_CPU_FP_HOMED_H */
