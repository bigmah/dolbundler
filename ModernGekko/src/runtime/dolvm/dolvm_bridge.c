// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs a recompiled game that is data rather than code. The C and LLVM backends
// end at host machine code and the chassis jumps into it; this one ends at a
// bytecode module the chassis interprets, so nothing the recompiler produced is
// ever mapped executable. Same DolIR, same analysis, same module ABI -- the
// only difference the chassis can see is that dispatch() takes longer.
//
// This file is the only part of the chassis compiled against GXRuntime's
// CPUState header, which is why the interface it exposes mentions none of it.

#include "dolvm_bridge.h"

#include "core/cpu.h"
#include "vm/dolvm.h"
#include "vm/dolvm_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef DOLVM_SAMPLE
#include <mach/mach.h>
#include <pthread.h>
#endif

#ifdef DOLVM_PGO_GENERATE
extern int __llvm_profile_write_file(void);
#endif

static DolVMModule s_module;
static bool s_open;
static DolVMBridgeRange* s_regions;
static char s_game_id[8];

int dolvm_bridge_open(const char* path, DolVMBridgeInfo* info, char* error,
                      size_t error_size)
{
    dolvm_bridge_close();
    if (!dolvm_module_load_file(&s_module, path, error, error_size))
        return 0;

    s_regions = (DolVMBridgeRange*)malloc(
        (size_t)(s_module.region_count ? s_module.region_count : 1u) *
        sizeof(*s_regions));
    if (!s_regions)
    {
        dolvm_module_close(&s_module);
        if (error && error_size)
            snprintf(error, error_size, "dolvm: out of memory listing regions");
        return 0;
    }
    for (u32 i = 0; i < s_module.region_count; i++)
    {
        s_regions[i].start = s_module.regions[i].guest_start;
        s_regions[i].end = s_module.regions[i].guest_end;
    }
    memcpy(s_game_id, s_module.header->game_id, sizeof(s_game_id));
    s_game_id[sizeof(s_game_id) - 1u] = '\0';
    s_open = true;

    memset(info, 0, sizeof(*info));
    info->game_id = s_game_id;
    info->entry_point = s_module.header->entry_point;
    info->state_layout_hash = s_module.header->state_layout_hash;
    info->regions = s_regions;
    info->region_hashes = s_module.region_hashes;
    info->region_count = s_module.region_count;
    // DolVMRange and DolVMBridgeRange are both two u32s in the same order; the
    // cast keeps the container's table in place instead of copying it.
    info->smc_ranges = (const DolVMBridgeRange*)s_module.smc_ranges;
    info->smc_count = s_module.smc_count;
    info->direct_calls = (s_module.flags & DOLVM_FLAG_DIRECT_CALLS) ? 1 : 0;
    return 1;
}

void dolvm_bridge_close(void)
{
#ifdef DOLVM_PROFILE
    if (s_open)
        dolvm_profile_report(stderr);
#endif
    if (s_open)
        dolvm_module_close(&s_module);
    free(s_regions);
    s_regions = NULL;
    s_open = false;
}

#ifdef DOLVM_SAMPLE
// Where the interpreter's time actually goes, per opcode.
//
// Counting executed opcodes says how many there are, not what they cost, and on
// this machine those turn out to be very different questions -- removing a
// tenth of the dispatches changed nothing measurable, while removing far fewer
// branches did. A sampling profiler answers the second question, but every
// handler lives inside one enormous function, so an ordinary one reports a
// single symbol. g_dolvm_op_handlers holds each handler's entry address, which
// is what turns a sampled pc back into an opcode.
extern const void* g_dolvm_op_handlers[];
extern u64 g_dolvm_op_counts[];

static mach_port_t s_sampled_thread;
static u64 s_samples[512];
static u64 s_samples_outside;
static volatile int s_sampling;

static void* dolvm_sampler(void* unused) {
    (void)unused;
    // Handler entry addresses, sorted, so a pc maps to the last one at or below
    // it. Everything below the first is somewhere in the prologue.
    uintptr_t bounds[512];
    u32 slots[512];
    u32 count = 0;
    for (u32 op = 0; op < DOLVM_OP_COUNT && count < 512u; op++) {
        if (!g_dolvm_op_handlers[op])
            continue;
        bounds[count] = (uintptr_t)g_dolvm_op_handlers[op];
        slots[count] = op;
        count++;
    }
    for (u32 i = 1; i < count; i++) {
        uintptr_t key = bounds[i];
        u32 slot = slots[i];
        u32 j = i;
        while (j && bounds[j - 1u] > key) {
            bounds[j] = bounds[j - 1u];
            slots[j] = slots[j - 1u];
            j--;
        }
        bounds[j] = key;
        slots[j] = slot;
    }

    while (s_sampling) {
        struct timespec pause = {0, 200000};
        nanosleep(&pause, NULL);
        if (thread_suspend(s_sampled_thread) != KERN_SUCCESS)
            continue;
        arm_thread_state64_t state;
        mach_msg_type_number_t size = ARM_THREAD_STATE64_COUNT;
        kern_return_t rc = thread_get_state(
            s_sampled_thread, ARM_THREAD_STATE64, (thread_state_t)&state, &size);
        thread_resume(s_sampled_thread);
        if (rc != KERN_SUCCESS)
            continue;
        uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
        if (!count || pc < bounds[0]) {
            s_samples_outside++;
            continue;
        }
        u32 low = 0, high = count;
        while (low + 1u < high) {
            u32 middle = low + (high - low) / 2u;
            if (bounds[middle] <= pc)
                low = middle;
            else
                high = middle;
        }
        // Anything more than a few kilobytes past the last handler is not in the
        // dispatch loop at all.
        if (pc - bounds[low] > 8192u)
            s_samples_outside++;
        else
            s_samples[slots[low]]++;
    }
    return NULL;
}

static void dolvm_sample_start(void) {
    s_sampled_thread = mach_thread_self();
    s_sampling = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, dolvm_sampler, NULL);
    pthread_detach(thread);
}

static void dolvm_sample_report(void) {
    s_sampling = 0;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
    u64 total = s_samples_outside;
    for (u32 op = 0; op < DOLVM_OP_COUNT; op++)
        total += s_samples[op];
    if (!total)
        return;
    fprintf(stderr, "[dolvm-sample] %llu samples, %.1f%% outside the dispatch loop\n",
            (unsigned long long)total, 100.0 * (double)s_samples_outside / (double)total);
    fprintf(stderr, "   time%%   ops%%   ns/op  opcode\n");
    u64 op_total = 0;
    for (u32 op = 0; op < DOLVM_OP_COUNT; op++)
        op_total += g_dolvm_op_counts[op];
    u32 order[DOLVM_OP_COUNT];
    for (u32 op = 0; op < DOLVM_OP_COUNT; op++)
        order[op] = op;
    for (u32 i = 1; i < DOLVM_OP_COUNT; i++) {
        u32 key = order[i];
        u32 j = i;
        while (j && s_samples[order[j - 1u]] < s_samples[key]) {
            order[j] = order[j - 1u];
            j--;
        }
        order[j] = key;
    }
    for (u32 i = 0; i < 24u && i < DOLVM_OP_COUNT; i++) {
        u32 op = order[i];
        if (!s_samples[op])
            break;
        double share = 100.0 * (double)s_samples[op] / (double)total;
        double ops = op_total ? 100.0 * (double)g_dolvm_op_counts[op] / (double)op_total : 0.0;
        // 200us per sample, so a sample is 0.2ms of this opcode's total time.
        double ns = g_dolvm_op_counts[op]
                        ? (double)s_samples[op] * 200000.0 / (double)g_dolvm_op_counts[op]
                        : 0.0;
        fprintf(stderr, "  %6.2f %6.2f %7.2f  %s\n", share, ops, ns,
                dolvm_op_name(op));
    }
}
#endif

int dolvm_bridge_dispatch(struct CPUState* ctx, uint32_t address)
{
    if (!s_open)
        return 0;
#ifdef DOLVM_BENCH
    // Wall time to retire a fixed number of guest cycles. Running for a fixed
    // number of seconds instead measures whatever scene the game happened to
    // reach, and a faster build reaches a different one -- which is how a
    // change that does nothing can look like several percent either way. The
    // chassis only touches downcount between dispatches, so the difference
    // across one is exactly what the module charged.
    {
        static u64 budget;
        static u64 charged;
        static double started;
        static double started_cpu;
        if (!budget) {
            const char* configured = getenv("DOLVM_BENCH_CYCLES");
            budget = configured ? strtoull(configured, NULL, 0) : 0;
            if (!budget)
                budget = ~0ull;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            started = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
            started_cpu = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
#ifdef DOLVM_SAMPLE
            dolvm_sample_start();
#endif
        }
        s64 before = ctx->downcount;
        int rc = dolvm_dispatch(&s_module, ctx, address);
        charged += (u64)(before - ctx->downcount);
        if (charged >= budget) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed =
                (double)now.tv_sec + (double)now.tv_nsec / 1e9 - started;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
            double cpu =
                (double)now.tv_sec + (double)now.tv_nsec / 1e9 - started_cpu;
            // Wall time is what a player feels, and cpu time is what survives a
            // busy machine: a background daemon eating a core moves the first by
            // tens of percent and the second by very little, which is the
            // difference between a measurement and a mood.
            fprintf(stderr,
                    "[dolvm-bench] %llu guest cycles in %.3fs = %.4fx "
                    "(cpu %.3fs = %.4fx)\n",
                    (unsigned long long)charged, elapsed,
                    (double)charged / 486000000.0 / elapsed, cpu,
                    (double)charged / 486000000.0 / cpu);
            fflush(stderr);
#ifdef DOLVM_PGO_GENERATE
            // An instrumented build writes its profile from an atexit hook,
            // which _exit skips. Ask for it explicitly.
            __llvm_profile_write_file();
#endif
#ifdef DOLVM_PROFILE
            dolvm_profile_report(stderr);
#endif
#ifdef DOLVM_SAMPLE
            dolvm_sample_report();
#endif
            fflush(stderr);
            _exit(0);
        }
        return rc;
    }
#else
    return dolvm_dispatch(&s_module, ctx, address);
#endif
}

void dolvm_bridge_on_state_loaded(struct CPUState* ctx)
{
    // Re-arm host FP rounding and flush-to-zero from the guest FPSCR the
    // chassis just loaded, exactly as a native module's glue does.
    ppc_fpscr_updated(ctx);
}
