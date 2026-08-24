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
#include "core/dispatch_gate.h"
#include "vm/dolvm.h"
#include "vm/dolvm_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef DOLVM_SAMPLE
#include <dlfcn.h>
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
// The interpreter's view of the chassis's gate: the same pointers, restated in
// the interpreter's own type so DolRecomp never has to see a chassis header.
static DolVMGate s_gate;
static const u32 s_no_pending;
static const s32 s_no_budget;

extern int g_dolvm_poll_skip_enabled;
extern int g_dolvm_hle_enabled;

int dolvm_bridge_open(const char* path, DolVMBridgeInfo* info, char* error,
                      size_t error_size)
{
    if (getenv("DOLVM_NO_POLL_SKIP"))
        g_dolvm_poll_skip_enabled = 0;
    {
        // The A/B for what a native stand-in is worth: the same module with
        // every HLE op declining, so the interpreted bodies under them run.
        const char* hle = getenv("DOLVM_HLE");
        if (hle && hle[0] == '0')
            g_dolvm_hle_enabled = 0;
    }
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

void dolvm_bridge_set_gate(const StaticRecompDispatchGate* gate)
{
    if (!s_open || !gate || gate->chunk_count != s_module.region_count ||
        !gate->chunk_open)
    {
        if (s_open)
            dolvm_module_set_gate(&s_module, NULL);
        if (gate && s_open)
            fprintf(stderr,
                    "dolvm: gate describes %u chunks, module has %u regions; "
                    "resolving nothing across them\n",
                    gate->chunk_count, s_module.region_count);
        return;
    }
    s_gate.region_open = gate->chunk_open;
    s_gate.budget = gate->budget ? gate->budget : &s_no_budget;
    s_gate.pending = gate->pending ? gate->pending : &s_no_pending;
    s_gate.pending_sync = gate->pending_sync;
    s_gate.pending_async = gate->pending_async;
    dolvm_module_set_gate(&s_module, &s_gate);
}

#ifdef DOLVM_SAMPLE
// Defined below, with the sampler; declared here so shutdown can report.
static void dolvm_sample_report(void);
extern volatile int dolvm_sample_running(void);
#endif

void dolvm_bridge_close(void)
{
#ifdef DOLVM_PROFILE
    if (s_open)
        dolvm_profile_report(stderr);
#endif
#ifdef DOLVM_SAMPLE
    // Without the bench there is no _exit to report from, so a clean shutdown
    // is the moment. This is how the profile comes off a phone: the app stops
    // the game on a timer, the chassis closes the module, and the report lands
    // in the run log with everything else.
    if (s_open && dolvm_sample_running())
        dolvm_sample_report();
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
#ifdef DOLVM_PROFILE
extern u64 g_dolvm_op_counts[];
#endif

static mach_port_t s_sampled_thread;
static u64 s_samples[512];
static u64 s_samples_outside;
static volatile int s_sampling;

// Raw program-counter histogram, so a hot spot can be named to the instruction
// rather than to whichever handler label happens to precede it in memory --
// which, once PGO has split every handler into a hot and a cold half, is the
// wrong one about half the time.
#define DOLVM_PC_BUCKETS 65536u
static uintptr_t s_pc_keys[DOLVM_PC_BUCKETS];
static u64 s_pc_counts[DOLVM_PC_BUCKETS];

static void dolvm_pc_record(uintptr_t pc) {
    u32 slot = (u32)((pc >> 2) * 2654435761u) & (DOLVM_PC_BUCKETS - 1u);
    for (u32 probe = 0; probe < DOLVM_PC_BUCKETS; probe++) {
        u32 i = (slot + probe) & (DOLVM_PC_BUCKETS - 1u);
        if (s_pc_keys[i] == pc || s_pc_keys[i] == 0) {
            s_pc_keys[i] = pc;
            s_pc_counts[i]++;
            return;
        }
    }
}

static void dolvm_pc_report(u64 total) {
    u32 order[256];
    u32 live = 0;
    for (u32 i = 0; i < DOLVM_PC_BUCKETS; i++) {
        if (!s_pc_counts[i])
            continue;
        // Keep the 256 hottest with an insertion sort.
        u32 j = live < 256u ? live : 255u;
        if (live == 256u && s_pc_counts[order[255]] >= s_pc_counts[i])
            continue;
        while (j && s_pc_counts[order[j - 1u]] < s_pc_counts[i]) {
            order[j] = order[j - 1u];
            j--;
        }
        order[j] = i;
        if (live < 256u)
            live++;
    }
    // By symbol first, so the time outside the dispatch loop has names.
    {
        const char* names[256];
        u64 counts[256];
        u32 live_symbols = 0;
        for (u32 i = 0; i < DOLVM_PC_BUCKETS; i++) {
            if (!s_pc_counts[i])
                continue;
            Dl_info info;
            const char* name = "?";
            if (dladdr((void*)s_pc_keys[i], &info) && info.dli_sname)
                name = info.dli_sname;
            u32 j;
            for (j = 0; j < live_symbols; j++)
                if (strcmp(names[j], name) == 0)
                    break;
            if (j == live_symbols) {
                if (live_symbols == 256u)
                    continue;
                names[live_symbols] = name;
                counts[live_symbols] = 0;
                live_symbols++;
            }
            counts[j] += s_pc_counts[i];
        }
        fprintf(stderr, "[dolvm-sample] by symbol:\n");
        for (u32 round = 0; round < 24u && round < live_symbols; round++) {
            u32 best = round;
            for (u32 j = round + 1u; j < live_symbols; j++)
                if (counts[j] > counts[best])
                    best = j;
            const char* name = names[best];
            u64 count = counts[best];
            names[best] = names[round];
            counts[best] = counts[round];
            names[round] = name;
            counts[round] = count;
            fprintf(stderr, "  %6.2f%%  %s\n", 100.0 * (double)count / (double)total, name);
        }
    }
    fprintf(stderr, "[dolvm-sample] hottest program counters (offset from dolvm_dispatch):\n");
    uintptr_t base = (uintptr_t)&dolvm_dispatch;
    // Name each hot pc with the handler it is furthest into. Tail duplication
    // and PGO both move a handler's body away from its entry label, so this is
    // a hint rather than an attribution -- but with 180 handlers in one
    // function it is the difference between a number and a lead.
    uintptr_t hbounds[512];
    u32 hslots[512];
    u32 hcount = 0;
    for (u32 op = 0; op < DOLVM_OP_COUNT && hcount < 512u; op++) {
        if (!g_dolvm_op_handlers[op])
            continue;
        hbounds[hcount] = (uintptr_t)g_dolvm_op_handlers[op];
        hslots[hcount] = op;
        hcount++;
    }
    for (u32 i = 1; i < hcount; i++) {
        uintptr_t key = hbounds[i];
        u32 slot = hslots[i];
        u32 j = i;
        while (j && hbounds[j - 1u] > key) {
            hbounds[j] = hbounds[j - 1u];
            hslots[j] = hslots[j - 1u];
            j--;
        }
        hbounds[j] = key;
        hslots[j] = slot;
    }
    double running = 0.0;
    for (u32 i = 0; i < live && i < 120u; i++) {
        uintptr_t pc = s_pc_keys[order[i]];
        double share = 100.0 * (double)s_pc_counts[order[i]] / (double)total;
        running += share;
        Dl_info info;
        const char* name = "?";
        uintptr_t off = 0;
        if (dladdr((void*)pc, &info) && info.dli_sname) {
            name = info.dli_sname;
            off = pc - (uintptr_t)info.dli_saddr;
        }
        const char* nearest = "";
        unsigned long into = 0;
        if (hcount && pc >= hbounds[0] && pc < hbounds[hcount - 1u] + 65536u) {
            u32 low = 0, high = hcount;
            while (low + 1u < high) {
                u32 middle = low + (high - low) / 2u;
                if (hbounds[middle] <= pc)
                    low = middle;
                else
                    high = middle;
            }
            nearest = dolvm_op_name(hslots[low]);
            into = (unsigned long)(pc - hbounds[low]);
        }
        fprintf(stderr, "  %5.2f%% (cum %5.2f%%)  %s+0x%lx  [dispatch%+ld]  %s+%lu\n",
                share, running, name, (unsigned long)off, (long)(pc - base),
                nearest, into);
    }
}

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
        dolvm_pc_record(pc);
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

volatile int dolvm_sample_running(void) { return s_sampling; }

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
#ifdef DOLVM_PROFILE
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
#endif
    dolvm_pc_report(total);
}
#endif

static volatile int s_bench_at_skip;

int dolvm_bridge_bench_at_skip(void)
{
    return s_bench_at_skip;
}

int dolvm_bridge_dispatch(struct CPUState* ctx, uint32_t address)
{
    if (!s_open)
        return 0;
#if defined(DOLVM_SAMPLE) && !defined(DOLVM_BENCH)
    // The bench starts the sampler when its window opens; with no bench, the
    // first dispatch is the start of everything there is to measure.
    if (!s_sampling)
        dolvm_sample_start();
#endif
#ifdef DOLVM_BENCH
    // Wall time to retire a fixed number of guest cycles. Running for a fixed
    // number of seconds instead measures whatever scene the game happened to
    // reach, and a faster build reaches a different one -- which is how a
    // change that does nothing can look like several percent either way.
    // Guest cycles are read off the timebase, which the chassis advances by
    // every cycle the module charges -- including the ones it flushes from
    // inside a hook, which a per-dispatch difference of downcount would miss.
    {
        // A game is not one workload. Disney skate spends its first twelve
        // guest seconds on logos and fades, which retire far faster than the
        // skating does, so a window that opens at the first dispatch measures
        // the wrong scene. DOLVM_BENCH_SKIP charges past the boot before the
        // clocks start, and DOLVM_BENCH_WINDOW reports every so many cycles --
        // which is how the scene a window landed in gets identified at all.
        static u64 budget;
        static u64 skip;
        static u64 window;
        static u64 window_mark;
        static u64 timebase_start;
        static u64 measure_base;
        static double started;
        static double started_cpu;
        static double window_at;
        static double window_at_cpu;
        static int configured_once;
        static int running;
        if (!configured_once) {
            configured_once = 1;
            const char* configured = getenv("DOLVM_BENCH_CYCLES");
            budget = configured ? strtoull(configured, NULL, 0) : 0;
            if (!budget)
                budget = ~0ull;
            configured = getenv("DOLVM_BENCH_SKIP");
            skip = configured ? strtoull(configured, NULL, 0) : 0;
            configured = getenv("DOLVM_BENCH_WINDOW");
            window = configured ? strtoull(configured, NULL, 0) : 0;
            timebase_start = ctx->timebase;
        }
        int rc = dolvm_dispatch(&s_module, ctx, address);
        // The Gekko's timebase ticks once per twelve cycles.
        u64 booted = (ctx->timebase - timebase_start) * 12u;
        if (!running) {
            if (booted < skip)
                return rc;
            running = 1;
            measure_base = booted;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            started = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
            started_cpu = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
            window_at = started;
            window_at_cpu = started_cpu;
            s_bench_at_skip = 1;
#ifdef DOLVM_SAMPLE
            dolvm_sample_start();
#endif
            return rc;
        }
        u64 charged = booted - measure_base;
        if (window && charged - window_mark >= window && charged < budget) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double wall = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
            double cpu = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
            double span = (double)(charged - window_mark) / 486000000.0;
            fprintf(stderr,
                    "[dolvm-bench] at %6.1fs guest: %.4fx wall  %.4fx cpu\n",
                    (double)booted / 486000000.0, span / (wall - window_at),
                    span / (cpu - window_at_cpu));
            fflush(stderr);
            window_at = wall;
            window_at_cpu = cpu;
            window_mark = charged;
        }
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
