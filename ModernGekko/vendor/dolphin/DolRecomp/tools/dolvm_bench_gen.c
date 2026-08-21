// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emit the benchmark kernels through DolRecomp's C backend, so the host
// compiler builds the native arm the interpreter is measured against.

#include "backend/emitter.h"
#include "frontend/decoder.h"
#include "tools/dolvm_bench_kernels.h"

#include <stdio.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: dolvm_bench_gen <output.c>\n");
        return 2;
    }
    FILE* out = fopen(argv[1], "w");
    if (!out) {
        fprintf(stderr, "error: cannot create %s\n", argv[1]);
        return 1;
    }
    // dolrecomp_call_depth is defined by dr_cpu, which this links against.
    emit_header_for_cpu(out, DOLRECOMP_CPU_GEKKO);

    DolVMBenchKernel kernels[DOLVM_BENCH_KERNEL_COUNT];
    dolvm_bench_kernels(kernels);
    for (u32 k = 0; k < DOLVM_BENCH_KERNEL_COUNT; k++) {
        const DolVMBenchKernel* kernel = &kernels[k];
        PPCInst instructions[DOLVM_BENCH_MAX_WORDS];
        for (u32 i = 0; i < kernel->count; i++)
            instructions[i] =
                ppc_decode(kernel->words[i], kernel->address + i * 4u);
        if (!emit_function(out, instructions, kernel->count, kernel->address)) {
            fclose(out);
            return 1;
        }
    }
    fprintf(out, "#endif\n");
    fclose(out);
    return 0;
}
