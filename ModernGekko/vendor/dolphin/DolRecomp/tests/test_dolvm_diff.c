// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every opcode the decoder knows, run through both backends on identical state,
// compared field by field.
//
// The C backend is the shipping one, so it is the definition of correct here.
// Each opcode gets its own two-instruction function, generated at build time by
// the C emitter and compiled by the host compiler; the same encoding is lowered
// to bytecode in-process. Both arms then run against one CPUState, reset from
// the same seed between them, with the same helper callbacks installed. Any
// difference in architectural state or in the memory window is a bug in the
// bytecode path.

#include "../src/backend/vm/dolvm_emit.h"
#include "../src/cpu/cpu.h"
#include "../src/ir/dolir_builder.h"
#include "../src/vm/dolvm_interp.h"
#include "dolvm_diff_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void (*const dolvm_diff_native[DOLRECOMP_OPCODE_RAW_COUNT])(CPUState*);
extern const u32 dolvm_diff_group_count;
extern const u32 dolvm_diff_group_length;
extern const u32 dolvm_diff_group_words[];
extern void (*const dolvm_diff_group_native[])(CPUState*);

// Guest addresses the pointer-shaped seed hands out sit at the middle of this
// window, so a signed 16-bit displacement in either direction stays inside it
// and the whole window is cheap to reset and compare.
#define DIFF_WINDOW 0x20000u
#define DIFF_POINTER_BASE 0x8000u
#define DIFF_SEEDS 6u

static u64 rng_state;

static u32 next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (u32)(rng_state >> 32);
}

// Deterministic stand-ins for everything the chassis would provide, so both
// arms see identical answers from identical calls.
static u64 diff_external_read(CPUState* cpu, u32 ea, u8 size) {
    (void)cpu;
    return 0xA5A5A5A5A5A5A5A5ull ^ ((u64)ea << 8) ^ size;
}

static void diff_external_write(CPUState* cpu, u32 ea, u64 value, u8 size) {
    cpu->external_addr = ea;
    cpu->external_value = (u32)value;
    cpu->external_rid = size;
}

static u32 diff_external_read32(CPUState* cpu, u32 ea, u8 rid) {
    (void)cpu;
    return 0x5A5A0000u ^ ea ^ ((u32)rid << 8);
}

static void diff_external_write32(CPUState* cpu, u32 ea, u32 value, u8 rid) {
    cpu->external_addr = ea;
    cpu->external_value = value;
    cpu->external_rid = rid;
}

static void diff_fallback(CPUState* cpu, u32 raw, u32 cia) {
    cpu->gpr[0] ^= raw;
    cpu->pc = cia + 4u;
}

static bool diff_host_call(CPUState* cpu, u32 address) {
    cpu->external_addr = address;
    return false;
}

static void diff_cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia) {
    cpu->external_addr = ea;
    cpu->external_value = cia;
    cpu->external_rid = operation;
}

static void install_callbacks(CPUState* cpu) {
    cpu->external_read = diff_external_read;
    cpu->external_write = diff_external_write;
    cpu->external_read32 = diff_external_read32;
    cpu->external_write32 = diff_external_write32;
    cpu->instruction_fallback = diff_fallback;
    cpu->host_call = diff_host_call;
    cpu->cache_control = diff_cache_control;
}

// `pointers` seeds every GPR into the RAM window so memory operations take the
// fast path; otherwise they get values that are nowhere near mapped memory, so
// the same operations take the chassis path instead.
static void seed_state(CPUState* cpu, u8* window, u32 seed, bool pointers) {
    rng_state = 0x9E3779B97F4A7C15ull ^ ((u64)seed * 0x100000001B3ull);
    if (!rng_state)
        rng_state = 1;

    for (u32 i = 0; i < 32; i++) {
        if (pointers)
            cpu->gpr[i] = GC_RAM_BASE + DIFF_POINTER_BASE +
                          (next_random() % 0x800u & ~7u);
        else
            cpu->gpr[i] = (next_random() & 0x0FFFFFFFu) | 0x10000000u;
    }
    for (u32 i = 0; i < 32; i++) {
        u64 bits = ((u64)next_random() << 32) | next_random();
        // Keep exponents in a range that cannot produce a NaN payload the two
        // arms would be free to disagree about.
        bits = (bits & 0x800FFFFFFFFFFFFFull) | 0x3F00000000000000ull;
        memcpy(&cpu->fpr[i], &bits, sizeof(bits));
        bits = ((u64)next_random() << 32) | next_random();
        bits = (bits & 0x800FFFFFFFFFFFFFull) | 0x3F00000000000000ull;
        memcpy(&cpu->ps1[i], &bits, sizeof(bits));
    }
    cpu->pc = 0;
    cpu->lr = DOLVM_DIFF_RETURN;
    cpu->ctr = next_random();
    cpu->cr = next_random();
    cpu->xer = next_random();
    cpu->fpscr = next_random() & 0x0000FFFFu;
    // Supervisor, floating point enabled: the interesting paths, not the
    // privilege trap on every other instruction. User mode is not seeded here
    // because the C backend omits the privilege check the IR builder emits, so
    // the two arms are entitled to disagree about it; test_dolvm.c covers the
    // trap against a hand model instead.
    cpu->msr = 1u << 13;
    cpu->srr0 = next_random() & ~3u;
    cpu->srr1 = next_random();
    cpu->dar = next_random();
    cpu->dsisr = next_random();
    cpu->ear = next_random() | 0x80000000u;
    cpu->hid2 = PPC_HID2_LSQE | PPC_HID2_PSE | PPC_HID2_LCE;
    cpu->timebase = next_random();
    for (u32 i = 0; i < 16; i++)
        cpu->sr[i] = next_random();
    for (u32 i = 0; i < 8; i++)
        cpu->gqr[i] = next_random() & 0x00070007u;
    for (u32 i = 0; i < 1024; i++)
        cpu->spr[i] = 0;
    memset(cpu->locked_cache_tag, 0, sizeof(cpu->locked_cache_tag));
    memset(cpu->locked_cache_valid, 0, sizeof(cpu->locked_cache_valid));
    cpu->exception = 0;
    cpu->program_exception = 0;
    cpu->reserve_addr = GC_RAM_BASE + DIFF_POINTER_BASE;
    cpu->reserve_valid = (next_random() & 1u) != 0;
    cpu->external_addr = 0;
    cpu->external_value = 0;
    cpu->external_rid = 0;
    cpu->external_read_count = 0;
    cpu->external_write_count = 0;
    cpu->downcount = 1 << 20;

    for (u32 i = 0; i < DIFF_WINDOW; i++)
        window[i] = (u8)next_random();
    memcpy(cpu->ram, window, DIFF_WINDOW);
}

typedef struct {
    u32 gpr[32];
    u64 fpr[32];
    u64 ps1[32];
    u32 pc, lr, ctr, cr, xer, fpscr, msr, srr0, srr1, dar, dsisr, ear, hid2;
    u64 timebase;
    u32 sr[16];
    u32 gqr[8];
    u32 exception, program_exception;
    u32 external_addr, external_value;
    u8 external_rid;
    u32 reserve_addr;
    bool reserve_valid;
    s64 downcount;
    u32 spr[1024];
} Snapshot;

static void snapshot(const CPUState* cpu, Snapshot* out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->gpr, cpu->gpr, sizeof(out->gpr));
    memcpy(out->fpr, cpu->fpr, sizeof(out->fpr));
    memcpy(out->ps1, cpu->ps1, sizeof(out->ps1));
    out->pc = cpu->pc;
    out->lr = cpu->lr;
    out->ctr = cpu->ctr;
    out->cr = cpu->cr;
    out->xer = cpu->xer;
    out->fpscr = cpu->fpscr;
    out->msr = cpu->msr;
    out->srr0 = cpu->srr0;
    out->srr1 = cpu->srr1;
    out->dar = cpu->dar;
    out->dsisr = cpu->dsisr;
    out->ear = cpu->ear;
    out->hid2 = cpu->hid2;
    out->timebase = cpu->timebase;
    memcpy(out->sr, cpu->sr, sizeof(out->sr));
    memcpy(out->gqr, cpu->gqr, sizeof(out->gqr));
    out->exception = cpu->exception;
    out->program_exception = cpu->program_exception;
    out->external_addr = cpu->external_addr;
    out->external_value = cpu->external_value;
    out->external_rid = cpu->external_rid;
    out->reserve_addr = cpu->reserve_addr;
    out->reserve_valid = cpu->reserve_valid;
    out->downcount = cpu->downcount;
    memcpy(out->spr, cpu->spr, sizeof(out->spr));
}

static int failures;
static int skipped_fallback;
static int skipped_known;

static void report(u32 raw, u32 seed, bool pointers, const char* field,
                   unsigned long long native, unsigned long long dolvm) {
    if (failures < 40)
        fprintf(stderr,
                "raw 0x%08X seed %u (%s): %s native 0x%llX, dolvm 0x%llX\n", raw,
                seed, pointers ? "pointers" : "scalars", field, native, dolvm);
    failures++;
}

#define COMPARE(field)                                                        \
    do {                                                                      \
        if (native.field != vm.field)                                         \
            report(raw, seed, pointers, #field,                               \
                   (unsigned long long)native.field,                          \
                   (unsigned long long)vm.field);                             \
    } while (0)

#define COMPARE_INDEXED(field, index)                                         \
    do {                                                                      \
        if (native.field[index] != vm.field[index]) {                         \
            char name[32];                                                    \
            snprintf(name, sizeof(name), "%s[%u]", #field, index);            \
            report(raw, seed, pointers, name,                                 \
                   (unsigned long long)native.field[index],                   \
                   (unsigned long long)vm.field[index]);                      \
        }                                                                     \
    } while (0)

// One (program, entry address, seed) pair, both ways.
static void compare_run(const DolVMModule* module, CPUState* cpu, u8* window,
                        u8* native_memory, void (*native_fn)(CPUState*),
                        u32 raw, u32 address, u32 seed, bool pointers) {
    Snapshot native, vm;

    seed_state(cpu, window, seed, pointers);
    cpu->pc = address;
    native_fn(cpu);
    snapshot(cpu, &native);
    memcpy(native_memory, cpu->ram, DIFF_WINDOW);

    seed_state(cpu, window, seed, pointers);
    cpu->pc = address;
    if (dolvm_dispatch(module, cpu, address) != 1) {
        report(raw, seed, pointers, "dispatch", 1, 0);
        return;
    }
    snapshot(cpu, &vm);

    for (u32 n = 0; n < 32; n++) {
        COMPARE_INDEXED(gpr, n);
        COMPARE_INDEXED(fpr, n);
        COMPARE_INDEXED(ps1, n);
    }
    COMPARE(pc);
    COMPARE(lr);
    COMPARE(ctr);
    COMPARE(cr);
    COMPARE(xer);
    COMPARE(fpscr);
    COMPARE(msr);
    COMPARE(srr0);
    COMPARE(srr1);
    COMPARE(dar);
    COMPARE(dsisr);
    COMPARE(ear);
    COMPARE(hid2);
    COMPARE(timebase);
    COMPARE(exception);
    COMPARE(program_exception);
    COMPARE(external_addr);
    COMPARE(external_value);
    COMPARE(external_rid);
    COMPARE(reserve_addr);
    COMPARE(reserve_valid);
    COMPARE(downcount);
    for (u32 n = 0; n < 16; n++)
        COMPARE_INDEXED(sr, n);
    for (u32 n = 0; n < 8; n++)
        COMPARE_INDEXED(gqr, n);
    for (u32 n = 0; n < 1024; n++)
        COMPARE_INDEXED(spr, n);
    if (memcmp(native_memory, cpu->ram, DIFF_WINDOW) != 0) {
        for (u32 n = 0; n < DIFF_WINDOW; n++) {
            if (native_memory[n] == ((const u8*)cpu->ram)[n])
                continue;
            char name[48];
            snprintf(name, sizeof(name), "ram[0x%X]", n);
            report(raw, seed, pointers, name, native_memory[n],
                   ((const u8*)cpu->ram)[n]);
            break;
        }
    }
}

int main(int argc, char** argv) {
    // DOLVM_POISON_REGS=1 fills the bytecode register file with an even
    // pattern at every dispatch, so any read of a register the stream never
    // wrote is deterministic and wrong instead of quietly plausible.
    {
        extern int g_dolvm_poison_registers;
        extern int g_dolvm_poison_byte;
        const char* poison = getenv("DOLVM_POISON_REGS");
        if (poison && *poison && *poison != '0')
            g_dolvm_poison_registers = 1;
        const char* byte = getenv("DOLVM_POISON_BYTE");
        if (byte && *byte)
            g_dolvm_poison_byte = (int)strtoul(byte, NULL, 0) & 0xFF;
    }

    // The corpus runs twice in CI: once against the homed registers, which is
    // how a shipped module is lowered, and once with them off, because the
    // plain state path is still what every non-homed slot uses and a change
    // that only breaks one of the two is the easy mistake to make.
    bool home_state = true;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--no-home-state"))
            home_state = false;
    printf("dolvm diff: guest state %s\n",
           home_state ? "in homed registers" : "in CPUState");

    const int count = DOLRECOMP_OPCODE_RAW_COUNT;
    if ((PPC_OP_COUNT - 1) != count) {
        fprintf(stderr, "opcode count mismatch: enum has %d, table has %d\n",
                PPC_OP_COUNT - 1, count);
        return 1;
    }

    DolIRModule ir;
    dolir_module_init(&ir);
    for (int i = 0; i < count; i++) {
        u32 address = dolvm_diff_address(i);
        PPCInst pair[2];
        pair[0] = ppc_decode(dolrecomp_opcode_raws[i], address);
        pair[1] = ppc_decode(0x4E800020u, address + 4u);
        if (!dolir_build_chunk(&ir, pair, 2, address)) {
            fprintf(stderr, "cannot build IR for 0x%08X\n",
                    dolrecomp_opcode_raws[i]);
            return 1;
        }
    }
    if (!dolir_verify(&ir, stderr))
        return 1;

    // Recorded before lowering, which merges the blocks away.
    bool* not_lowered = (bool*)calloc((size_t)count, sizeof(bool));
    if (!not_lowered)
        return 1;
    for (int i = 0; i < count; i++)
        not_lowered[i] =
            ir.functions[i].blocks[0].terminator.kind == DOLIR_TERM_FALLBACK;

    DolVMOptStats stats;
    memset(&stats, 0, sizeof(stats));
    DolVMEmitOptions options;
    memset(&options, 0, sizeof(options));
    options.home_state = home_state;
    void* image = NULL;
    size_t size = 0;
    if (!dolvm_build_module(&ir, &options, &image, &size, &stats, stderr)) {
        fprintf(stderr, "cannot lower opcode corpus\n");
        return 1;
    }
    dolir_module_free(&ir);
    // A mismatch here is a bytecode bug, and the fastest way to see one is to
    // read the bytecode: DOLVM_DIFF_DUMP=<path> writes the corpus out for
    // dolvm_dis, which reports the guest address of every entry point.
    {
        const char* dump = getenv("DOLVM_DIFF_DUMP");
        if (dump)
            dolvm_write_module(image, size, dump, stderr);
    }
    DolVMModule module;
    char error[256] = "";
    if (!dolvm_module_open(&module, image, size, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    dolvm_stats_report(&stats, "opcodes", stderr);

    CPUState cpu;
    if (!cpu_init(&cpu)) {
        fprintf(stderr, "cannot initialize CPU state\n");
        return 1;
    }
    install_callbacks(&cpu);
    u8* window = (u8*)malloc(DIFF_WINDOW);
    u8* native_memory = (u8*)malloc(DIFF_WINDOW);
    if (!window || !native_memory)
        return 1;

    for (int i = 0; i < count; i++) {
        u32 raw = dolrecomp_opcode_raws[i];
        u32 address = dolvm_diff_address(i);
        PPCInst decoded = ppc_decode(raw, address);
        // An opcode the IR builder does not lower becomes a fallback, and the
        // C backend inlines it instead. With a real chassis interpreter behind
        // the fallback the two agree; with this test's stub they cannot, so
        // there is nothing here to compare. Detecting it from the IR rather
        // than from a list means the exclusion disappears by itself the day
        // the builder learns to lower the opcode.
        if (not_lowered[i]) {
            skipped_fallback++;
            continue;
        }
        if (dolvm_diff_ir_path_differs(decoded.op)) {
            skipped_known++;
            continue;
        }
        for (u32 seed = 0; seed < DIFF_SEEDS; seed++) {
            for (u32 mode = 0; mode < 2u; mode++) {
                compare_run(&module, &cpu, window, native_memory,
                            dolvm_diff_native[i], raw, address, seed,
                            mode == 0);
            }
        }
    }

    // The same opcodes again, chained into straight-line groups so superblock
    // formation has something to merge -- and then entered at every address
    // inside a merged block, which is the case the chassis produces whenever an
    // exception resumes partway through one.
    DolIRModule group_ir;
    dolir_module_init(&group_ir);
    for (u32 g = 0; g < dolvm_diff_group_count; g++) {
        u32 base = dolvm_diff_group_address(g);
        PPCInst* body =
            (PPCInst*)malloc(dolvm_diff_group_length * sizeof(*body));
        if (!body)
            return 1;
        for (u32 n = 0; n < dolvm_diff_group_length; n++)
            body[n] = ppc_decode(
                dolvm_diff_group_words[g * dolvm_diff_group_length + n],
                base + n * 4u);
        bool ok = dolir_build_chunk(&group_ir, body, dolvm_diff_group_length,
                                    base);
        free(body);
        if (!ok)
            return 1;
    }
    DolVMOptStats group_stats;
    memset(&group_stats, 0, sizeof(group_stats));
    void* group_image = NULL;
    size_t group_size = 0;
    if (!dolvm_build_module(&group_ir, &options, &group_image, &group_size,
                            &group_stats, stderr))
        return 1;
    dolir_module_free(&group_ir);
    DolVMModule group_module;
    if (!dolvm_module_open(&group_module, group_image, group_size, error,
                           sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    dolvm_stats_report(&group_stats, "groups", stderr);
    // The point of the exercise is that the groups really did merge.
    if (group_stats.blocks_after >= group_stats.blocks_before) {
        fprintf(stderr, "groups did not merge: %u -> %u blocks\n",
                group_stats.blocks_before, group_stats.blocks_after);
        return 1;
    }

    for (u32 g = 0; g < dolvm_diff_group_count; g++) {
        u32 base = dolvm_diff_group_address(g);
        for (u32 entry = 0; entry + 1u < dolvm_diff_group_length; entry++) {
            for (u32 seed = 0; seed < DIFF_SEEDS; seed++) {
                for (u32 mode = 0; mode < 2u; mode++) {
                    compare_run(&group_module, &cpu, window, native_memory,
                                dolvm_diff_group_native[g], base, base + entry * 4u,
                                seed, mode == 0);
                }
            }
        }
    }
    dolvm_module_close(&group_module);
    free(group_image);

    free(not_lowered);
    free(window);
    free(native_memory);
    cpu_free(&cpu);
    dolvm_module_close(&module);
    free(image);
    if (failures) {
        fprintf(stderr, "dolvm differential: %d mismatches\n", failures);
        return 1;
    }
    printf("dolvm differential: %d of %d opcodes x %u seeds x 2 seedings agree "
           "(%d not lowered by the IR builder, %d differ upstream of the VM)\n",
           count - skipped_fallback - skipped_known, count, DIFF_SEEDS,
           skipped_fallback, skipped_known);
    printf("dolvm differential: %u merged groups entered at %u addresses each "
           "agree\n",
           dolvm_diff_group_count, dolvm_diff_group_length - 1u);
    return 0;
}
