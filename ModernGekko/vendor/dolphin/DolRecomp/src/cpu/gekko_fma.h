// SPDX-License-Identifier: GPL-3.0-or-later
//
// A cheap fma for targets that have no FMA instruction.
//
// wasm is one, so fma() there is musl's software implementation -- and it was
// the hottest single symbol in the emulator thread, 8.4% of it.
//
// Most calls do not need it. fma rounds once, at the end; a plain a*c+b rounds
// twice, once after the multiply. When the product is *exact* the first
// rounding does nothing and the two agree bit for bit.
//
// The product is exact when the two significands fit together in the 53 bits a
// double stores. Trailing zeroes in the stored mantissa bound each one: `c` has
// usually been through force_25_bit and carries at most 26 significant bits, so
// `a` may carry 27. The exponent needs a separate guard -- an exact significand
// still rounds away if the product lands in the subnormal range, and still
// overflows to infinity -- so the product's exponent has to stay normal, and it
// can be either pexp or pexp + 1 depending on where the significands land.
//
// Anything failing a test falls through to the real fma, so this cannot change
// a result: it only picks the cheaper of two identical answers. Held to that by
// moderngekko_gekko_fma_test, which forces the path on natively and diffs it
// against fma over ~50M inputs including the edges each guard exists for.
#ifndef DOLRECOMP_GEKKO_FMA_H
#define DOLRECOMP_GEKKO_FMA_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#if !defined(DOLRECOMP_FMA_FASTPATH)
#if defined(__wasm__)
#define DOLRECOMP_FMA_FASTPATH 1
#else
#define DOLRECOMP_FMA_FASTPATH 0
#endif
#endif

static inline uint64_t dolrecomp_fma_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static inline double gekko_fma(double a, double c, double b) {
#if DOLRECOMP_FMA_FASTPATH
    const uint64_t abits = dolrecomp_fma_bits(a);
    const uint64_t cbits = dolrecomp_fma_bits(c);
    const int aexp = (int)((abits >> 52) & 0x7FFull);
    const int cexp = (int)((cbits >> 52) & 0x7FFull);

    // Both normal, and at most 27 + 26 significant bits between them.
    if (aexp > 0 && aexp < 0x7FF && cexp > 0 && cexp < 0x7FF &&
        (abits & 0x0000000003FFFFFFull) == 0 &&
        (cbits & 0x0000000007FFFFFFull) == 0) {
        const int pexp = aexp + cexp - 1023;
        if (pexp > 1 && pexp < 0x7FE)
            return a * c + b;
    }
#endif
    return fma(a, c, b);
}

#endif  // DOLRECOMP_GEKKO_FMA_H
