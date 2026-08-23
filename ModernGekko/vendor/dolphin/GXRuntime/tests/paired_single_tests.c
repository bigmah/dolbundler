// The paired-single fast path against the exact code it replaces.
//
// Each ppc_ps_* op has two implementations: one that computes both lanes
// straight through and checks a handful of bit patterns afterwards, and the
// scalar code it falls back to when one of those patterns turns up. The fast
// path is only worth having if it is bit-identical, and "bit-identical" here
// means the destination register pair *and* every FPSCR bit the op can touch,
// under every rounding mode and with flush-to-zero both on and off.
//
// So this runs the pair side by side over operands drawn to land on the cases
// that separate them: denormal multiplicands, results that sit exactly on a
// round-to-even tie, values that overflow to infinity, signalling NaNs, and
// ordinary numbers in between.

#include <stdio.h>
#include <string.h>

#include "../src/core/cpu_interpreter_private.h"

static int failures = 0;
static unsigned long long checked = 0;

static u64 rng_state = 0x243F6A8885A308D3ull;

static u64 rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static f64 bits_to_f64(u64 bits) {
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static u64 f64_to_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Operand classes, in the proportions that matter: mostly ordinary values,
// with every awkward one represented often enough to be hit thousands of
// times over a run.
static f64 sample_operand(void) {
    u64 r = rng_next();
    switch (r % 16u) {
    case 0:
        return 0.0;
    case 1:
        return -0.0;
    case 2:
        return bits_to_f64(0x7FF0000000000000ull);  // +inf
    case 3:
        return bits_to_f64(0xFFF0000000000000ull);  // -inf
    case 4:
        return bits_to_f64(0x7FF8000000000000ull);  // qNaN
    case 5:
        return bits_to_f64(0x7FF4000000000000ull);  // sNaN
    case 6:
        return bits_to_f64((rng_next() & 0x800FFFFFFFFFFFFFull) | 1u);  // denormal double
    case 7: {
        // A denormal single widened to double: below the smallest normal
        // single, which is where force_single's flush-to-zero rule lives.
        u64 bits = (rng_next() & 0x8000000000000000ull) |
                   ((u64)((rng_next() % 0x30u) + 0x350u) << 52) |
                   (rng_next() & 0x000FFFFFFFFFFFFFull);
        return bits_to_f64(bits);
    }
    case 8:
    case 9: {
        // Huge, so products overflow to infinity.
        u64 bits = (rng_next() & 0x8000000000000000ull) |
                   ((u64)((rng_next() % 0x20u) + 0x7D0u) << 52) |
                   (rng_next() & 0x000FFFFFFFFFFFFFull);
        return bits_to_f64(bits);
    }
    case 10:
    case 11: {
        // Values whose low mantissa bits sit on the round-to-even boundary
        // the fused multiply-add forms correct by hand.
        u64 bits = (rng_next() & 0x8000000000000000ull) |
                   ((u64)((rng_next() % 0x40u) + 0x3E0u) << 52) |
                   (rng_next() & 0x000FFFFFE0000000ull) | 0x10000000ull;
        return bits_to_f64(bits);
    }
    default: {
        // An ordinary single-precision magnitude, which is what a game's
        // geometry and physics actually contain.
        u32 bits = (u32)((rng_next() & 0x807FFFFFu) |
                         (((rng_next() % 40u) + 105u) << 23));
        f32 value;
        memcpy(&value, &bits, sizeof(value));
        return (f64)value;
    }
    }
}

// Registers 1..3 hold the operands; 0 is the destination, and is seeded with
// a pattern so a lane the op fails to write shows up as a difference too.
static void seed(CPUState* cpu, u32 fpscr) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->fpscr = fpscr;
    cpu->fpr[0] = bits_to_f64(0xDEADBEEFCAFEF00Dull);
    cpu->ps1[0] = bits_to_f64(0x0123456789ABCDEFull);
    for (u32 i = 1; i <= 3; i++) {
        cpu->fpr[i] = sample_operand();
        cpu->ps1[i] = sample_operand();
    }
    ppc_fpscr_control_updated(cpu);
}

static void compare(const char* op, const CPUState* fast,
                    const CPUState* exact) {
    checked++;
    if (f64_to_bits(fast->fpr[0]) == f64_to_bits(exact->fpr[0]) &&
        f64_to_bits(fast->ps1[0]) == f64_to_bits(exact->ps1[0]) &&
        fast->fpscr == exact->fpscr)
        return;
    if (failures < 20) {
        fprintf(stderr,
                "FAIL %s\n  ps0 %016llx vs %016llx\n  ps1 %016llx vs %016llx\n"
                "  fpscr %08x vs %08x\n  a %016llx/%016llx c %016llx/%016llx "
                "b %016llx/%016llx\n",
                op, (unsigned long long)f64_to_bits(fast->fpr[0]),
                (unsigned long long)f64_to_bits(exact->fpr[0]),
                (unsigned long long)f64_to_bits(fast->ps1[0]),
                (unsigned long long)f64_to_bits(exact->ps1[0]), fast->fpscr,
                exact->fpscr, (unsigned long long)f64_to_bits(exact->fpr[1]),
                (unsigned long long)f64_to_bits(exact->ps1[1]),
                (unsigned long long)f64_to_bits(exact->fpr[2]),
                (unsigned long long)f64_to_bits(exact->ps1[2]),
                (unsigned long long)f64_to_bits(exact->fpr[3]),
                (unsigned long long)f64_to_bits(exact->ps1[3]));
    }
    failures++;
}

#define RUN(name, call_fast, call_exact)                                       \
    do {                                                                       \
        CPUState fast, exact;                                                  \
        u64 saved = rng_state;                                                 \
        seed(&fast, fpscr);                                                    \
        rng_state = saved;                                                     \
        seed(&exact, fpscr);                                                   \
        call_fast;                                                             \
        call_exact;                                                            \
        compare(name, &fast, &exact);                                          \
    } while (0)

int main(void) {
    // Every rounding mode, and flush-to-zero both ways: Gekko titles run with
    // NI set, which is exactly the arm a version of this gated on NI never
    // exercised.
    static const u32 modes[] = {0u, 1u, 2u, 3u, FPSCR_NI_BIT, FPSCR_NI_BIT | 1u,
                                FPSCR_NI_BIT | 2u, FPSCR_NI_BIT | 3u,
                                FPSCR_VE_BIT, FPSCR_VE_BIT | FPSCR_NI_BIT};
    const unsigned iterations = 200000u;

    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        const u32 fpscr = modes[m];
        for (unsigned i = 0; i < iterations; i++) {
            RUN("ps_add", ppc_ps_add_op(&fast, 0, 1, 2),
                ppc_ps_add_op_exact(&exact, 0, 1, 2));
            RUN("ps_sub", ppc_ps_sub_op(&fast, 0, 1, 2),
                ppc_ps_sub_op_exact(&exact, 0, 1, 2));
            RUN("ps_mul", ppc_ps_mul_op(&fast, 0, 1, 2),
                ppc_ps_mul_op_exact(&exact, 0, 1, 2));
            RUN("ps_muls0", ppc_ps_muls0(&fast, 0, 1, 2),
                ppc_ps_muls0_exact(&exact, 0, 1, 2));
            RUN("ps_muls1", ppc_ps_muls1(&fast, 0, 1, 2),
                ppc_ps_muls1_exact(&exact, 0, 1, 2));
            RUN("ps_madd", ppc_ps_madd_op(&fast, 0, 1, 2, 3, false, false),
                ppc_ps_madd_op_exact(&exact, 0, 1, 2, 3, false, false));
            RUN("ps_msub", ppc_ps_madd_op(&fast, 0, 1, 2, 3, true, false),
                ppc_ps_madd_op_exact(&exact, 0, 1, 2, 3, true, false));
            RUN("ps_nmadd", ppc_ps_madd_op(&fast, 0, 1, 2, 3, false, true),
                ppc_ps_madd_op_exact(&exact, 0, 1, 2, 3, false, true));
            RUN("ps_nmsub", ppc_ps_madd_op(&fast, 0, 1, 2, 3, true, true),
                ppc_ps_madd_op_exact(&exact, 0, 1, 2, 3, true, true));
            RUN("ps_madds0", ppc_ps_madds0(&fast, 0, 1, 2, 3),
                ppc_ps_madds0_exact(&exact, 0, 1, 2, 3));
            RUN("ps_madds1", ppc_ps_madds1(&fast, 0, 1, 2, 3),
                ppc_ps_madds1_exact(&exact, 0, 1, 2, 3));
            RUN("ps_sum0", ppc_ps_sum0(&fast, 0, 1, 2, 3),
                ppc_ps_sum0_exact(&exact, 0, 1, 2, 3));
            RUN("ps_sum1", ppc_ps_sum1(&fast, 0, 1, 2, 3),
                ppc_ps_sum1_exact(&exact, 0, 1, 2, 3));

            RUN("fadds", ppc_fadds(&fast, 0, 1, 2),
                ppc_fadds_exact(&exact, 0, 1, 2));
            RUN("fsubs", ppc_fsubs(&fast, 0, 1, 2),
                ppc_fsubs_exact(&exact, 0, 1, 2));
            RUN("fmuls", ppc_fmuls(&fast, 0, 1, 2),
                ppc_fmuls_exact(&exact, 0, 1, 2));
            RUN("fadd", ppc_fadd(&fast, 0, 1, 2),
                ppc_fadd_exact(&exact, 0, 1, 2));
            RUN("fsub", ppc_fsub(&fast, 0, 1, 2),
                ppc_fsub_exact(&exact, 0, 1, 2));
            RUN("fmul", ppc_fmul(&fast, 0, 1, 2),
                ppc_fmul_exact(&exact, 0, 1, 2));
            RUN("fmadds", ppc_fmadd_op(&fast, 0, 1, 2, 3, true, false, false),
                ppc_fmadd_op_exact(&exact, 0, 1, 2, 3, true, false, false));
            RUN("fmsubs", ppc_fmadd_op(&fast, 0, 1, 2, 3, true, true, false),
                ppc_fmadd_op_exact(&exact, 0, 1, 2, 3, true, true, false));
            RUN("fnmadds", ppc_fmadd_op(&fast, 0, 1, 2, 3, true, false, true),
                ppc_fmadd_op_exact(&exact, 0, 1, 2, 3, true, false, true));
            RUN("fmadd", ppc_fmadd_op(&fast, 0, 1, 2, 3, false, false, false),
                ppc_fmadd_op_exact(&exact, 0, 1, 2, 3, false, false, false));
            RUN("fnmsub", ppc_fmadd_op(&fast, 0, 1, 2, 3, false, true, true),
                ppc_fmadd_op_exact(&exact, 0, 1, 2, 3, false, true, true));

        }
    }

    // A destination that is also an operand has to read before it writes.
    for (unsigned i = 0; i < 20000u; i++) {
        CPUState fast, exact;
        u64 saved = rng_state;
        seed(&fast, FPSCR_NI_BIT);
        rng_state = saved;
        seed(&exact, FPSCR_NI_BIT);
        fast.fpr[0] = fast.fpr[1];
        fast.ps1[0] = fast.ps1[1];
        exact.fpr[0] = exact.fpr[1];
        exact.ps1[0] = exact.ps1[1];
        ppc_ps_madd_op(&fast, 0, 0, 2, 3, false, false);
        ppc_ps_madd_op_exact(&exact, 0, 0, 2, 3, false, false);
        compare("ps_madd aliased", &fast, &exact);
    }

    printf("%llu comparisons, %d failures\n", checked, failures);
    return failures ? 1 : 0;
}
