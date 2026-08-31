// SPDX-License-Identifier: GPL-3.0-or-later
//
// gekko_fma is only allowed to exist if it is bit-identical to the fma it
// replaces. It is a fast path, not an approximation: every input it accepts
// must produce exactly the bits fma would have produced, and every input it
// cannot prove exact must fall through.
//
// So this is a differential test. It forces the fast path on -- the native
// build would otherwise compile it out, since native targets have a real FMA
// instruction and do not need it -- and compares against fma over the shapes
// that actually reach it plus the edges each guard exists for.

#define DOLRECOMP_FMA_FASTPATH 1
#include "../src/cpu/gekko_fma.h"

#include <inttypes.h>
#include <stdio.h>

static uint64_t rng_state = 0x853c49e6748fea9bull;
static uint64_t next_u64(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static double bits_to_f64(uint64_t bits) {
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

// The same 25-bit rounding the Gekko applies to the multiplier of a
// single-precision multiply-add, so `c` has the shape it really has in flight.
static double force_25_bit(double value) {
    uint64_t bits = dolrecomp_fma_bits(value);
    uint64_t fraction = bits & 0x000FFFFFFFFFFFFFull;
    uint64_t keep_mask = 0xFFFFFFFFF8000000ull;
    uint64_t round = 0x0000000008000000ull;
    if ((bits & 0x7FF0000000000000ull) == 0 && fraction != 0) {
        unsigned shift = 0;
        uint64_t probe = fraction;
        while (!(probe & 0x0010000000000000ull) && shift < 60) { probe <<= 1; shift++; }
        if (shift < 28) {
            keep_mask = ~((1ull << (27 - shift)) - 1);
            round >>= shift;
        } else {
            keep_mask = ~0ull;
            round = 0;
        }
    }
    bits = (bits & keep_mask) + (bits & round);
    return bits_to_f64(bits);
}

static double rand_single(void) {
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)next_u64();
    return (double)v.f;
}

// Exponents crowded against the guards: the subnormal floor and the overflow
// ceiling, where an exact significand is still not an exact product.
static double rand_edge(void) {
    static const int exps[] = {0, 1, 2, 3, 0x7FC, 0x7FD, 0x7FE, 0x7FF, 511, 512, 1023};
    uint64_t r = next_u64();
    int e = exps[r % (sizeof exps / sizeof exps[0])];
    uint64_t mant = next_u64() & 0x000FFFFFFFFFFFFFull;
    if (r & 0x100) mant &= ~0x0000000003FFFFFFull;  // often already exact-shaped
    return bits_to_f64(((r >> 9) & 1ull) << 63 | ((uint64_t)e << 52) | mant);
}

int main(void) {
    const long long iterations = 50000000;
    long long mismatches = 0, fast_taken = 0, nan_pairs = 0;

    for (long long i = 0; i < iterations; i++) {
        double a, c, b;
        switch (i & 3) {
        case 0:  // the common case: singles, c rounded to 25 bits
            a = rand_single(); c = force_25_bit(rand_single()); b = rand_single();
            break;
        case 1:  // arbitrary doubles, which must mostly fall through
            a = bits_to_f64(next_u64()); c = bits_to_f64(next_u64()); b = bits_to_f64(next_u64());
            break;
        case 2:  // exponent edges against both guards
            a = rand_edge(); c = force_25_bit(rand_edge()); b = rand_edge();
            break;
        default: // mixed: a full double against a rounded single
            a = bits_to_f64(next_u64()); c = force_25_bit(rand_single()); b = rand_single();
            break;
        }

        const double got = gekko_fma(a, c, b);
        const double want = fma(a, c, b);

        // Did the fast path actually run? Recompute its predicate so the test
        // can prove it is exercising something and not just measuring fma.
        const uint64_t ab = dolrecomp_fma_bits(a), cb = dolrecomp_fma_bits(c);
        const int ae = (int)((ab >> 52) & 0x7FF), ce = (int)((cb >> 52) & 0x7FF);
        if (ae > 0 && ae < 0x7FF && ce > 0 && ce < 0x7FF &&
            (ab & 0x3FFFFFFull) == 0 && (cb & 0x7FFFFFFull) == 0) {
            const int pexp = ae + ce - 1023;
            if (pexp > 1 && pexp < 0x7FE) fast_taken++;
        }

        if (isnan(got) && isnan(want)) { nan_pairs++; continue; }
        if (dolrecomp_fma_bits(got) != dolrecomp_fma_bits(want)) {
            if (mismatches < 10) {
                fprintf(stderr,
                        "mismatch: a=%.17g c=%.17g b=%.17g  got %016" PRIx64
                        " want %016" PRIx64 "\n",
                        a, c, b, dolrecomp_fma_bits(got), dolrecomp_fma_bits(want));
            }
            mismatches++;
        }
    }

    printf("gekko_fma: %lld comparisons, %lld took the fast path (%.1f%%), "
           "%lld NaN pairs, %lld mismatches\n",
           iterations, fast_taken, 100.0 * (double)fast_taken / (double)iterations,
           nan_pairs, mismatches);

    if (fast_taken == 0) {
        fprintf(stderr, "the fast path never ran; this test proves nothing\n");
        return 1;
    }
    return mismatches != 0;
}
