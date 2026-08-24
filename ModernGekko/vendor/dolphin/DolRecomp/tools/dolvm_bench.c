// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interpreter throughput, measured against the thing it replaces.
//
// Both arms run the same guest words: one through the C backend's generated
// code, compiled by the host compiler at build time, and one through the DolVM
// interpreter. Same CPUState, same helpers, same memory. The ratio is what an
// App Store build gives up by not being allowed to run generated machine code.
//
// The cycle budget is held open for the whole run so the numbers measure
// execution rather than the chassis round trips both arms would pay equally.

#include "backend/vm/dolvm_emit.h"
#include "cpu/cpu.h"
#include "ir/dolir_builder.h"
#include "tools/dolvm_bench_kernels.h"
#include "vm/dolvm_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void func_80100000(CPUState* ctx);
void func_80101000(CPUState* ctx);
void func_80102000(CPUState* ctx);
void func_80103000(CPUState* ctx);
void func_80104000(CPUState* ctx);

typedef void (*NativeKernel)(CPUState*);

#define BENCH_ELEMENTS 4096u
#define BENCH_REPEATS 400u

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static void prepare(CPUState* cpu, const DolVMBenchKernel* kernel) {
    cpu->pc = kernel->address;
    cpu->lr = 0x80200000u;
    cpu->exception = 0;
    cpu->program_exception = 0;
    cpu->cr = 0;
    cpu->xer = 0;
    cpu->msr = 1u << 13;
    // Far enough from the guard that the whole run stays in one dispatch.
    cpu->downcount = 1u << 30;
    cpu->gpr[3] = GC_RAM_BASE + 0x1000u;
    cpu->gpr[4] = GC_RAM_BASE + 0x1000u + BENCH_ELEMENTS * 4u;
    cpu->gpr[5] = BENCH_ELEMENTS;
    cpu->gpr[6] = 0;
    if (strcmp(kernel->name, "sum") == 0) {
        cpu->gpr[4] = BENCH_ELEMENTS;
        cpu->gpr[5] = 0;
    }
    cpu->fpr[2] = 1.0009765625;
    cpu->ps1[2] = 1.0009765625;
}

// Pass a path to also write the kernels out as a module, which dolvm_dis can
// then print instruction by instruction.
int main(int argc, char** argv) {
    const char* dump_path = argc > 1 ? argv[1] : NULL;

    DolVMBenchKernel kernels[DOLVM_BENCH_KERNEL_COUNT];
    dolvm_bench_kernels(kernels);
    const NativeKernel native[DOLVM_BENCH_KERNEL_COUNT] = {
        func_80100000, func_80101000, func_80102000, func_80103000,
        func_80104000};

    DolIRModule ir;
    dolir_module_init(&ir);
    for (u32 k = 0; k < DOLVM_BENCH_KERNEL_COUNT; k++) {
        PPCInst instructions[DOLVM_BENCH_MAX_WORDS];
        for (u32 i = 0; i < kernels[k].count; i++)
            instructions[i] =
                ppc_decode(kernels[k].words[i], kernels[k].address + i * 4u);
        if (!dolir_build_chunk(&ir, instructions, kernels[k].count,
                               kernels[k].address)) {
            fprintf(stderr, "error: cannot build IR for %s\n", kernels[k].name);
            return 1;
        }
    }
    DolVMOptStats stats;
    memset(&stats, 0, sizeof(stats));
    DolVMEmitOptions options;
    memset(&options, 0, sizeof(options));
    options.home_state = true;
    void* image = NULL;
    size_t size = 0;
    if (!dolvm_build_module(&ir, &options, &image, &size, &stats, stderr)) {
        fprintf(stderr, "error: cannot lower benchmark kernels\n");
        return 1;
    }
    dolir_module_free(&ir);
    DolVMModule module;
    char error[256] = "";
    if (!dolvm_module_open(&module, image, size, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    if (dump_path && !dolvm_write_module(image, size, dump_path, stderr))
        return 1;
    dolvm_stats_report(&stats, "kernels", stdout);
    printf("kernels: %u bytecode instructions for %u guest instructions "
           "(%.2f per guest instruction)\n\n",
           module.code_count, module.map_count,
           (double)module.code_count / (double)module.map_count);

    CPUState cpu;
    if (!cpu_init(&cpu)) {
        fprintf(stderr, "error: cannot initialize CPU state\n");
        return 1;
    }

    printf("%-8s %14s %14s %10s  %s\n", "kernel", "native Minst/s",
           "dolvm Minst/s", "ratio", "program");
    double total_native = 0.0;
    double total_vm = 0.0;
    for (u32 k = 0; k < DOLVM_BENCH_KERNEL_COUNT; k++) {
        const DolVMBenchKernel* kernel = &kernels[k];
        double retired =
            (double)BENCH_REPEATS * (double)BENCH_ELEMENTS * kernel->loop_body;

        // A warm-up pass each way, so neither arm pays for cold caches.
        prepare(&cpu, kernel);
        native[k](&cpu);
        prepare(&cpu, kernel);
        dolvm_dispatch(&module, &cpu, kernel->address);

        double started = seconds();
        for (u32 r = 0; r < BENCH_REPEATS; r++) {
            prepare(&cpu, kernel);
            native[k](&cpu);
        }
        double native_time = seconds() - started;

        started = seconds();
        for (u32 r = 0; r < BENCH_REPEATS; r++) {
            prepare(&cpu, kernel);
            dolvm_dispatch(&module, &cpu, kernel->address);
        }
        double vm_time = seconds() - started;

        double native_rate = retired / native_time / 1e6;
        double vm_rate = retired / vm_time / 1e6;
        total_native += native_time;
        total_vm += vm_time;
        printf("%-8s %14.1f %14.1f %9.2fx  %s\n", kernel->name, native_rate,
               vm_rate, vm_time / native_time, kernel->description);
    }
    printf("\noverall: interpreter is %.2fx the wall time of generated code\n",
           total_vm / total_native);

    cpu_free(&cpu);
    dolvm_module_close(&module);
    free(image);
    return 0;
}
