/* native-vs-wasm throughput for DolRecomp-generated guest code */
#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void func_80003100(CPUState* ctx);

#define RET_SENTINEL 0x81234564u
#define SCRATCH (GC_RAM_BASE + 0x00100000u)

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static unsigned long long chassis_entries;

static void run_once(CPUState* c, u32 entry) {
    c->pc = entry;
    c->lr = 0x81234567u;
    c->exception = 0;
    c->program_exception = 0;
    do {
        c->downcount = (s64)1 << 40;
        func_80003100(c);
        chassis_entries++;
        if (c->exception) {
            fprintf(stderr, "guest exception %u at %08x\n", c->exception, c->pc);
            exit(3);
        }
    } while (c->pc != RET_SENTINEL);
}

typedef struct { const char* name; u32 entry; unsigned per_iter; } Kernel;

static const Kernel kernels[] = {
    { "int",  0x80003100u, 9 },
    { "mem",  0x80003200u, 8 },
    { "fp",   0x80003300u, 8 },
    { "call", 0x80003400u, 6 },
};

static void seed(CPUState* c) {
    mem_write32(c, SCRATCH + 0, 0x11223344u);
    mem_write32(c, SCRATCH + 4, 0x00005678u);
    mem_write32(c, SCRATCH + 12, 0xABCD0000u);
    /* IEEE-754 single 1.5 and 0.75, big-endian in guest memory */
    mem_write32(c, SCRATCH + 0x1000, 0x3FC00000u);
    mem_write32(c, SCRATCH + 0x1004, 0x3F400000u);
}

static char csv_buf[4096];

const char* bench_run(unsigned iters, int reps) {
    CPUState c;
    size_t off = 0;
    csv_buf[0] = 0;
    if (!cpu_init(&c)) return "init failed";
    c.msr = 1u << 13;
    c.gpr[1] = GC_RAM_BASE + 0x00080000u;
    seed(&c);
    off += (size_t)snprintf(csv_buf + off, sizeof(csv_buf) - off,
                            "kernel,iters,best_seconds,guest_instr,guest_MIPS\n");
    for (unsigned k = 0; k < sizeof(kernels)/sizeof(kernels[0]); k++) {
        double best = 1e30;
        for (int r = 0; r < reps; r++) {
            switch (k) {
            case 0: c.gpr[3] = iters; break;
            case 1: c.gpr[3] = SCRATCH; c.gpr[4] = iters; break;
            case 2: c.gpr[3] = SCRATCH + 0x1000; c.gpr[4] = iters; break;
            case 3: c.gpr[3] = iters; break;
            }
            double t0 = seconds();
            run_once(&c, kernels[k].entry);
            double dt = seconds() - t0;
            if (dt < best) best = dt;
        }
        double gi = (double)iters * kernels[k].per_iter;
        off += (size_t)snprintf(csv_buf + off, sizeof(csv_buf) - off,
                                "%s,%u,%.6f,%.0f,%.1f\n",
                                kernels[k].name, iters, best, gi, gi / best / 1e6);
    }
    cpu_free(&c);
    return csv_buf;
}

int main(int argc, char** argv) {
    u32 iters = argc > 1 ? (u32)strtoul(argv[1], NULL, 0) : 2000000u;
    int reps = argc > 2 ? atoi(argv[2]) : 3;
    CPUState c;
    if (!cpu_init(&c)) return 1;
    c.msr = 1u << 13; /* MSR[FP] */
    c.gpr[1] = GC_RAM_BASE + 0x00080000u;
    seed(&c);

    printf("kernel,iters,best_seconds,guest_instr,guest_MIPS,chassis_entries,checksum\n");
    for (unsigned k = 0; k < sizeof(kernels)/sizeof(kernels[0]); k++) {
        double best = 1e30;
        u32 check = 0;
        unsigned long long entries = 0;
        for (int r = 0; r < reps; r++) {
            chassis_entries = 0;
            switch (k) {
            case 0: c.gpr[3] = iters; break;
            case 1: c.gpr[3] = SCRATCH; c.gpr[4] = iters; break;
            case 2: c.gpr[3] = SCRATCH + 0x1000; c.gpr[4] = iters; break;
            case 3: c.gpr[3] = iters; break;
            }
            double t0 = seconds();
            run_once(&c, kernels[k].entry);
            double dt = seconds() - t0;
            if (dt < best) best = dt;
            check = c.gpr[3];
            entries = chassis_entries;
        }
        double gi = (double)iters * kernels[k].per_iter;
        printf("%s,%u,%.6f,%.0f,%.1f,%llu,0x%08X\n",
               kernels[k].name, iters, best, gi, gi / best / 1e6, entries, check);
    }
    cpu_free(&c);
    return 0;
}
