/* What wasm SIMD is worth on a bulk byteswap: the ceiling for the "SIMD the
   bulk paths" idea. Scalar bswap loop vs i8x16.shuffle over the same buffer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

#define N (4u << 20) /* 4 Mi words = 16 MB, bigger than any cache */
static uint32_t src[N];
static uint32_t dst[N];

static double sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

__attribute__((noinline)) static void swap_scalar(uint32_t *d, const uint32_t *s,
                                                  uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = __builtin_bswap32(s[i]);
}

#ifdef __wasm_simd128__
__attribute__((noinline)) static void swap_simd(uint32_t *d, const uint32_t *s,
                                                uint32_t n) {
    /* one i8x16.shuffle reverses the bytes of four 32-bit words at once --
       the same thing arm64's rev32.16b does in one instruction. */
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        v128_t v = wasm_v128_load(s + i);
        v = wasm_i8x16_shuffle(v, v, 3, 2, 1, 0, 7, 6, 5, 4,
                                     11, 10, 9, 8, 15, 14, 13, 12);
        wasm_v128_store(d + i, v);
    }
}
#endif

static double best_of(void (*fn)(uint32_t *, const uint32_t *, uint32_t),
                      int reps) {
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        double t0 = sec();
        fn(dst, src, N);
        double dt = sec() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

int main(void) {
    for (uint32_t i = 0; i < N; i++) src[i] = i * 2654435761u;

    double s = best_of(swap_scalar, 7);
    printf("scalar  %.3f ms  %.2f GB/s\n", s * 1e3, (double)N * 4 / s / 1e9);
    uint32_t check_scalar = dst[N / 2];

#ifdef __wasm_simd128__
    memset(dst, 0, sizeof dst);
    double v = best_of(swap_simd, 7);
    printf("simd    %.3f ms  %.2f GB/s   speedup %.2fx\n",
           v * 1e3, (double)N * 4 / v / 1e9, s / v);
    printf("match   %s\n", dst[N / 2] == check_scalar ? "yes" : "NO");
#else
    printf("simd    (not built with -msimd128)\n");
#endif
    return 0;
}
