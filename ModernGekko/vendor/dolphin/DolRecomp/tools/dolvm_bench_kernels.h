// SPDX-License-Identifier: GPL-3.0-or-later
//
// The guest programs the benchmark runs. Shared between the generator that
// emits the C backend's version and the harness that lowers the same words to
// bytecode, so both arms are provably the same program.

#ifndef DOLRECOMP_TOOLS_DOLVM_BENCH_KERNELS_H
#define DOLRECOMP_TOOLS_DOLVM_BENCH_KERNELS_H

#include "common/types.h"

#define DOLVM_BENCH_KERNEL_COUNT 3u
#define DOLVM_BENCH_MAX_WORDS 16u

typedef struct {
    const char* name;
    const char* description;
    u32 address;
    u32 words[DOLVM_BENCH_MAX_WORDS];
    u32 count;
    u32 loop_body;   // guest instructions retired per iteration
} DolVMBenchKernel;

static u32 dolvm_bench_bc(u32 bo, u32 bi, s32 offset) {
    return 0x40000000u | (bo << 21) | (bi << 16) | ((u32)offset & 0xFFFCu);
}

// bne to the top of a loop `back` instructions above the branch.
static u32 dolvm_bench_bne_back(u32 back) {
    return dolvm_bench_bc(4u, 2u, -(s32)(back * 4u));
}

static void dolvm_bench_kernels(DolVMBenchKernel* out) {
    // Sum an array: the plainest integer/load loop there is.
    DolVMBenchKernel* sum = &out[0];
    sum->name = "sum";
    sum->description = "lwz/add/addi/addic./bne over a word array";
    sum->address = 0x80100000u;
    sum->count = 0;
    sum->words[sum->count++] = 0x80C30000u;  // lwz    r6, 0(r3)
    sum->words[sum->count++] = 0x7CA53214u;  // add    r5, r5, r6
    sum->words[sum->count++] = 0x38630004u;  // addi   r3, r3, 4
    sum->words[sum->count++] = 0x3484FFFFu;  // addic. r4, r4, -1
    sum->words[sum->count++] = dolvm_bench_bne_back(4u);
    sum->words[sum->count++] = 0x4E800020u;  // blr
    sum->loop_body = 5u;

    // Copy an array: two memory operations per iteration instead of one.
    DolVMBenchKernel* copy = &out[1];
    copy->name = "copy";
    copy->description = "lwz/stw/addi/addi/addic./bne over a word array";
    copy->address = 0x80101000u;
    copy->count = 0;
    copy->words[copy->count++] = 0x80C30000u;  // lwz    r6, 0(r3)
    copy->words[copy->count++] = 0x90C40000u;  // stw    r6, 0(r4)
    copy->words[copy->count++] = 0x38630004u;  // addi   r3, r3, 4
    copy->words[copy->count++] = 0x38840004u;  // addi   r4, r4, 4
    copy->words[copy->count++] = 0x34A5FFFFu;  // addic. r5, r5, -1
    copy->words[copy->count++] = dolvm_bench_bne_back(5u);
    copy->words[copy->count++] = 0x4E800020u;  // blr
    copy->loop_body = 6u;

    // Scale an array of singles: dominated by the exact-float helpers, which
    // both backends call identically, so it shows what dispatch overhead looks
    // like once real work sits in the loop.
    DolVMBenchKernel* scale = &out[2];
    scale->name = "scale";
    scale->description = "lfs/fmuls/stfs/addi/addic./bne over a float array";
    scale->address = 0x80102000u;
    scale->count = 0;
    scale->words[scale->count++] = 0xC0230000u;  // lfs    f1, 0(r3)
    scale->words[scale->count++] = 0xEC2100B2u;  // fmuls  f1, f1, f2
    scale->words[scale->count++] = 0xD0230000u;  // stfs   f1, 0(r3)
    scale->words[scale->count++] = 0x38630004u;  // addi   r3, r3, 4
    scale->words[scale->count++] = 0x34A5FFFFu;  // addic. r5, r5, -1
    scale->words[scale->count++] = dolvm_bench_bne_back(5u);
    scale->words[scale->count++] = 0x4E800020u;  // blr
    scale->loop_body = 6u;
}

#endif
