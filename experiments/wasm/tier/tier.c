// A tiering probe for a WebAssembly engine: a kernel with 32 live doubles
// and 16 live integers, run for a fixed count of iterations per call. In a
// baseline tier every local is a stack slot and each op is a load and a
// store; in an optimising tier they are registers. The throughput of
// successive calls therefore steps up at the moment the function is
// promoted -- or never does.
#include <emscripten/emscripten.h>
#include <stdint.h>

EMSCRIPTEN_KEEPALIVE
double tier_kernel(int iters) {
    double a0 = 1.0, a1 = 1.1, a2 = 1.2, a3 = 1.3, a4 = 1.4, a5 = 1.5, a6 = 1.6, a7 = 1.7;
    double a8 = 1.8, a9 = 1.9, a10 = 2.0, a11 = 2.1, a12 = 2.2, a13 = 2.3, a14 = 2.4, a15 = 2.5;
    double b0 = 0.5, b1 = 0.6, b2 = 0.7, b3 = 0.8, b4 = 0.9, b5 = 1.0, b6 = 1.1, b7 = 1.2;
    double b8 = 1.3, b9 = 1.4, b10 = 1.5, b11 = 1.6, b12 = 1.7, b13 = 1.8, b14 = 1.9, b15 = 2.0;
    uint32_t i0 = 1, i1 = 2, i2 = 3, i3 = 4, i4 = 5, i5 = 6, i6 = 7, i7 = 8;
    uint32_t i8 = 9, i9 = 10, i10 = 11, i11 = 12, i12 = 13, i13 = 14, i14 = 15, i15 = 16;
    const double k = 0.999999;
    for (int n = 0; n < iters; ++n) {
        a0 = a0 * k + b15; a1 = a1 * k + b0; a2 = a2 * k + b1; a3 = a3 * k + b2;
        a4 = a4 * k + b3; a5 = a5 * k + b4; a6 = a6 * k + b5; a7 = a7 * k + b6;
        a8 = a8 * k + b7; a9 = a9 * k + b8; a10 = a10 * k + b9; a11 = a11 * k + b10;
        a12 = a12 * k + b11; a13 = a13 * k + b12; a14 = a14 * k + b13; a15 = a15 * k + b14;
        b0 = b0 * k - a1; b1 = b1 * k - a2; b2 = b2 * k - a3; b3 = b3 * k - a4;
        b4 = b4 * k - a5; b5 = b5 * k - a6; b6 = b6 * k - a7; b7 = b7 * k - a8;
        b8 = b8 * k - a9; b9 = b9 * k - a10; b10 = b10 * k - a11; b11 = b11 * k - a12;
        b12 = b12 * k - a13; b13 = b13 * k - a14; b14 = b14 * k - a15; b15 = b15 * k - a0;
        i0 = (i0 * 1664525u + i15) ^ i1; i1 = (i1 * 1664525u + i0) ^ i2; i2 = (i2 * 1664525u + i1) ^ i3;
        i3 = (i3 * 1664525u + i2) ^ i4; i4 = (i4 * 1664525u + i3) ^ i5; i5 = (i5 * 1664525u + i4) ^ i6;
        i6 = (i6 * 1664525u + i5) ^ i7; i7 = (i7 * 1664525u + i6) ^ i8; i8 = (i8 * 1664525u + i7) ^ i9;
        i9 = (i9 * 1664525u + i8) ^ i10; i10 = (i10 * 1664525u + i9) ^ i11; i11 = (i11 * 1664525u + i10) ^ i12;
        i12 = (i12 * 1664525u + i11) ^ i13; i13 = (i13 * 1664525u + i12) ^ i14; i14 = (i14 * 1664525u + i13) ^ i15;
        i15 = (i15 * 1664525u + i14) ^ i0;
    }
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 +
           b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8 + b9 + b10 + b11 + b12 + b13 + b14 + b15 +
           (double)(i0 ^ i1 ^ i2 ^ i3 ^ i4 ^ i5 ^ i6 ^ i7 ^ i8 ^ i9 ^ i10 ^ i11 ^ i12 ^ i13 ^ i14 ^ i15);
}

// The same work through memory: what the kernel costs when its state lives
// in linear memory rather than locals, i.e. what a baseline tier makes of
// the first one. If the two read the same on a device, that device's
// engine is not keeping locals in registers.
static double g_a[32];
static uint32_t g_i[16];

EMSCRIPTEN_KEEPALIVE
double tier_kernel_mem(int iters) {
    volatile double* a = g_a;
    volatile uint32_t* i = g_i;
    for (int n = 0; n < 32; ++n) a[n] = 1.0 + 0.1 * n;
    for (int n = 0; n < 16; ++n) i[n] = (uint32_t)(n + 1);
    const double k = 0.999999;
    for (int n = 0; n < iters; ++n) {
        for (int j = 0; j < 16; ++j) a[j] = a[j] * k + a[16 + ((j + 15) & 15)];
        for (int j = 0; j < 16; ++j) a[16 + j] = a[16 + j] * k - a[(j + 1) & 15];
        for (int j = 0; j < 16; ++j) i[j] = (i[j] * 1664525u + i[(j + 15) & 15]) ^ i[(j + 1) & 15];
    }
    double s = 0;
    for (int n = 0; n < 32; ++n) s += a[n];
    uint32_t x = 0;
    for (int n = 0; n < 16; ++n) x ^= i[n];
    return s + (double)x;
}
