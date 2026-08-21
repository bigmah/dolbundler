// SPDX-License-Identifier: GPL-3.0-or-later
//
// Executes the same programs the LLVM backend's tests execute, but through the
// bytecode path: build DolIR, optimize it, lower it, load the container, and
// interpret it. The expectations are the LLVM tests' expectations, so a
// divergence between the two backends shows up as a failure here.

#include "backend/vm/dolvm_emit.h"
#include "cpu/cpu.h"
#include "ir/dolir_builder.h"
#include "vm/dolvm_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__,   \
                    #x);                                                      \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static u32 fallback_count;
static int fallback_bad;
static u32 cache_count;
static u8 cache_operation;
static u32 cache_address;
static u32 cache_cia;
static u32 external_write_count;

static void fallback(CPUState* cpu, u32 raw, u32 cia) {
    fallback_bad |= raw != 0;
    fallback_count++;
    cpu->gpr[5] = 0x12345678u;
    cpu->pc = cia + 4u;
}

static void cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia) {
    (void)cpu;
    cache_count++;
    cache_operation = operation;
    cache_address = ea;
    cache_cia = cia;
}

static void external_write32(CPUState* cpu, u32 ea, u32 value, u8 rid) {
    external_write_count++;
    cpu->external_addr = ea;
    cpu->external_value = value;
    cpu->external_rid = rid;
}

static u32 encode_spr(u16 spr) {
    return ((spr & 31u) << 5) | ((spr >> 5) & 31u);
}

static u32 mfspr(u8 destination, u16 spr) {
    return 0x7C0002A6u | ((u32)destination << 21) | (encode_spr(spr) << 11);
}

static u32 mtspr(u8 source, u16 spr) {
    return 0x7C0003A6u | ((u32)source << 21) | (encode_spr(spr) << 11);
}

static bool add_chunk(DolIRModule* module, const u32* words, u32 count,
                      u32 address) {
    PPCInst* instructions = (PPCInst*)malloc((size_t)count * sizeof(PPCInst));
    if (!instructions)
        return false;
    for (u32 i = 0; i < count; i++)
        instructions[i] = ppc_decode(words[i], address + i * 4u);
    bool ok = dolir_build_chunk(module, instructions, count, address);
    free(instructions);
    return ok;
}

typedef struct {
    DolVMModule module;
    void* image;
} LoadedModule;

static bool build_and_load(DolIRModule* ir, LoadedModule* loaded,
                           bool direct_calls, DolVMOptStats* stats) {
    void* image = NULL;
    size_t size = 0;
    DolVMEmitOptions options;
    memset(&options, 0, sizeof(options));
    options.direct_calls = direct_calls;
    options.home_state = true;
    if (!dolvm_build_module(ir, &options, &image, &size, stats, stderr))
        return false;
    char error[256] = "";
    if (!dolvm_module_open(&loaded->module, image, size, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        free(image);
        return false;
    }
    loaded->image = image;
    return true;
}

static void unload(LoadedModule* loaded) {
    free(loaded->image);
    memset(loaded, 0, sizeof(*loaded));
}

static int run(const LoadedModule* loaded, CPUState* cpu, u32 address) {
    cpu->pc = address;
    return dolvm_dispatch(&loaded->module, cpu, address);
}

static void prepare_call(CPUState* cpu, u32 address) {
    cpu->pc = address;
    cpu->lr = 0x81234567u;
    cpu->exception = 0;
    cpu->program_exception = 0;
}

// ---------------------------------------------------------------------------
// The LLVM backend's execution corpus, run through the interpreter
// ---------------------------------------------------------------------------

static int test_programs(void) {
    DolIRModule ir;
    dolir_module_init(&ir);

    const u32 main_words[] = {
        0x38600000u, 0x00000000u, 0x38800000u, 0x7C841A14u,
        0x90610000u, 0x38630001u, 0x2C03000Au, 0x4180FFF4u,
        0xEE32A4FAu, 0x4E800020u,
    };
    CHECK(add_chunk(&ir, main_words, 10, 0x80001000u));

    const u32 spr_words[] = {mtspr(3, 273), mfspr(4, 273), 0x4E800020u};
    CHECK(add_chunk(&ir, spr_words, 3, 0x80002000u));

    const u32 segment_words[] = {
        0x7DC401A4u, 0x7D6304A6u, 0x7DE081E4u, 0x7D806D26u, 0x4E800020u,
    };
    CHECK(add_chunk(&ir, segment_words, 5, 0x80002100u));

    const u32 fpscr_words[] = {0xFFE0004Cu, 0xFDA0048Eu, 0x4E800020u};
    CHECK(add_chunk(&ir, fpscr_words, 3, 0x80002200u));

    const u32 lswx_words[] = {0x7D34AC2Au, 0x4E800020u};
    CHECK(add_chunk(&ir, lswx_words, 2, 0x80002300u));

    const u32 cache_words[] = {0x7C11906Cu, 0x4E800020u};
    CHECK(add_chunk(&ir, cache_words, 2, 0x80002400u));

    const u32 trap_words[] = {0x0C85FFFEu, 0x38630001u, 0x4E800020u};
    CHECK(add_chunk(&ir, trap_words, 3, 0x80002500u));

    const u32 sc_words[] = {0x44000002u};
    CHECK(add_chunk(&ir, sc_words, 1, 0x80002600u));

    const u32 rfi_words[] = {0x4C000064u};
    CHECK(add_chunk(&ir, rfi_words, 1, 0x80002700u));

    const u32 dcbz_l_words[] = {0x100537ECu, 0x4E800020u};
    CHECK(add_chunk(&ir, dcbz_l_words, 2, 0x80002800u));

    const u32 ecowx_words[] = {0x7D6C6B6Cu, 0x4E800020u};
    CHECK(add_chunk(&ir, ecowx_words, 2, 0x80002900u));

    const u32 float_words[] = {
        0xEC22182Au, 0xEC853028u, 0xECE80272u, 0xED4B6024u,
        0xFDAE782Au, 0xFE119028u, 0xFE740572u, 0xFED7C024u,
        0x4E800020u,
    };
    CHECK(add_chunk(&ir, float_words, 9, 0x80002A00u));

    const u32 paired_words[] = {
        0x1022182Au, 0x10E80272u, 0x11AE83FAu, 0x10A03030u,
        0x110D7000u, 0x10853460u, 0x4E800020u,
    };
    CHECK(add_chunk(&ir, paired_words, 7, 0x80002B00u));

    const u32 budget_words[] = {
        0x38630001u, 0x00000000u, 0x2C032710u, 0x4180FFF4u, 0x4E800020u,
    };
    CHECK(add_chunk(&ir, budget_words, 5, 0x80002C00u));

    CHECK(dolir_verify(&ir, stderr));

    DolVMOptStats stats;
    memset(&stats, 0, sizeof(stats));
    LoadedModule loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(build_and_load(&ir, &loaded, false, &stats));
    dolir_module_free(&ir);
    dolvm_stats_report(&stats, "corpus", stderr);

    CPUState cpu;
    CHECK(cpu_init(&cpu));
    cpu.lr = 0x81234567u;
    cpu.gpr[1] = GC_RAM_BASE;
    cpu.fpr[18] = 2.0;
    cpu.fpr[19] = 3.0;
    cpu.fpr[20] = 4.0;
    cpu.msr = 1u << 13;
    cpu.downcount = 0;
    cpu.instruction_fallback = fallback;
    CHECK(run(&loaded, &cpu, 0x80001000u) == 1);
    CHECK(cpu.gpr[3] == 10);
    CHECK(mem_read32(&cpu, GC_RAM_BASE) == 9);
    CHECK(cpu.pc == 0x81234564u);
    CHECK(cpu.fpr[17] == 10.0 && cpu.ps1[17] == 10.0);
    CHECK(cpu.gpr[5] == 0x12345678u && fallback_count == 1 && !fallback_bad);
    CHECK(cpu.downcount < 0 && cpu.downcount > -100);

    prepare_call(&cpu, 0x80002000u);
    cpu.gpr[3] = 0xA5A55A5Au;
    CHECK(run(&loaded, &cpu, 0x80002000u) == 1);
    CHECK(cpu.gpr[4] == 0xA5A55A5Au);
    CHECK(cpu.spr[273] == 0xA5A55A5Au);
    CHECK(cpu.pc == 0x81234564u);

    // The same privileged pair from user mode. The check the IR builder puts in
    // front of every privileged instruction -- an MSR read, a mask, a compare
    // and a raise -- lowers to a single opcode, and nothing else in the suite
    // would notice if that opcode tested the wrong bit or the wrong sense:
    // a running game is in supervisor mode, and the differential test cannot
    // cover it because the C backend omits the check altogether.
    prepare_call(&cpu, 0x80002000u);
    cpu.gpr[3] = 0x11112222u;
    cpu.spr[273] = 0;
    cpu.msr |= 1u << 14;
    CHECK(run(&loaded, &cpu, 0x80002000u) == 1);
    CHECK(cpu.exception != 0);
    CHECK(cpu.spr[273] == 0);
    cpu.msr &= ~(1u << 14);
    cpu.exception = 0;

    prepare_call(&cpu, 0x80002100u);
    cpu.gpr[14] = 0x12345678u;
    cpu.gpr[15] = 0xA0000000u;
    cpu.gpr[16] = 0xA0000000u;
    cpu.gpr[13] = 0xA0000000u;
    cpu.sr[3] = 0x87654321u;
    cpu.sr[10] = 0;
    CHECK(run(&loaded, &cpu, 0x80002100u) == 1);
    CHECK(cpu.sr[4] == 0x12345678u);
    CHECK(cpu.gpr[11] == 0x87654321u);
    CHECK(cpu.sr[10] == 0xA0000000u);
    CHECK(cpu.gpr[12] == 0xA0000000u);
    prepare_call(&cpu, 0x80002100u);
    cpu.msr = 1u << 14;
    cpu.gpr[14] = 0xDEADBEEFu;
    cpu.sr[4] = 0x10203040u;
    CHECK(run(&loaded, &cpu, 0x80002100u) == 1);
    CHECK(cpu.exception & PPC_EXC_PROGRAM);
    CHECK(cpu.program_exception & PPC_PROGRAM_PRIV);
    CHECK(cpu.sr[4] == 0x10203040u);
    cpu.msr = 0;

    prepare_call(&cpu, 0x80002200u);
    cpu.msr = 1u << 13;
    cpu.fpscr = 0;
    cpu.fpr[13] = 0;
    CHECK(run(&loaded, &cpu, 0x80002200u) == 1);
    CHECK((cpu.fpscr & 1u) != 0);
    u64 ffs_bits;
    memcpy(&ffs_bits, &cpu.fpr[13], sizeof(ffs_bits));
    CHECK((u32)ffs_bits == cpu.fpscr);
    CHECK((ffs_bits >> 32) == 0xFFF80000u);

    prepare_call(&cpu, 0x80002300u);
    cpu.xer = 5;
    cpu.gpr[20] = GC_RAM_BASE + 0x100u;
    cpu.gpr[21] = 3;
    mem_write8(&cpu, GC_RAM_BASE + 0x103u, 0x11);
    mem_write8(&cpu, GC_RAM_BASE + 0x104u, 0x22);
    mem_write8(&cpu, GC_RAM_BASE + 0x105u, 0x33);
    mem_write8(&cpu, GC_RAM_BASE + 0x106u, 0x44);
    mem_write8(&cpu, GC_RAM_BASE + 0x107u, 0x55);
    CHECK(run(&loaded, &cpu, 0x80002300u) == 1);
    CHECK(cpu.gpr[9] == 0x11223344u);
    CHECK(cpu.gpr[10] == 0x55000000u);

    prepare_call(&cpu, 0x80002400u);
    cpu.cache_control = cache_control;
    cpu.gpr[17] = GC_RAM_BASE + 0x200u;
    cpu.gpr[18] = 0x24u;
    CHECK(run(&loaded, &cpu, 0x80002400u) == 1);
    CHECK(cache_count == 1);
    CHECK(cache_operation == PPC_CACHE_DCBST);
    CHECK(cache_address == GC_RAM_BASE + 0x224u);
    CHECK(cache_cia == 0x80002400u);

    prepare_call(&cpu, 0x80002500u);
    cpu.gpr[3] = 40;
    cpu.gpr[5] = 1;
    CHECK(run(&loaded, &cpu, 0x80002500u) == 1);
    CHECK(cpu.gpr[3] == 41);
    CHECK(cpu.exception == 0);
    prepare_call(&cpu, 0x80002500u);
    cpu.gpr[3] = 40;
    cpu.gpr[5] = 0xFFFFFFFEu;
    CHECK(run(&loaded, &cpu, 0x80002500u) == 1);
    CHECK(cpu.gpr[3] == 40);
    CHECK(cpu.exception & PPC_EXC_PROGRAM);
    CHECK(cpu.program_exception & PPC_PROGRAM_TRAP);
    CHECK(cpu.srr0 == 0x80002500u);

    prepare_call(&cpu, 0x80002600u);
    CHECK(run(&loaded, &cpu, 0x80002600u) == 1);
    CHECK(cpu.exception & PPC_EXC_SYSTEM_CALL);
    CHECK(cpu.srr0 == 0x80002604u);
    CHECK(cpu.pc == PPC_VECTOR_SYSTEM_CALL);

    prepare_call(&cpu, 0x80002700u);
    cpu.msr = 0;
    cpu.srr0 = 0x80004003u;
    cpu.srr1 = 0x00002000u;
    CHECK(run(&loaded, &cpu, 0x80002700u) == 1);
    CHECK(cpu.exception == 0);
    CHECK(cpu.pc == 0x80004000u);
    CHECK(cpu.msr == 0x00002000u);

    prepare_call(&cpu, 0x80002800u);
    cpu.msr = 0;
    cpu.hid2 = PPC_HID2_LCE;
    cpu.gpr[5] = GC_RAM_BASE + 0x300u;
    cpu.gpr[6] = 4;
    for (u32 i = 0; i < 32; i++)
        mem_write8(&cpu, GC_RAM_BASE + 0x300u + i, 0x5Au);
    CHECK(run(&loaded, &cpu, 0x80002800u) == 1);
    for (u32 i = 0; i < 32; i++)
        CHECK(mem_read8(&cpu, GC_RAM_BASE + 0x300u + i) == 0);

    prepare_call(&cpu, 0x80002900u);
    cpu.ear = 0x80000007u;
    cpu.external_write32 = external_write32;
    cpu.gpr[11] = 0xCAFEBABEu;
    cpu.gpr[12] = 0x1000u;
    cpu.gpr[13] = 0x20u;
    CHECK(run(&loaded, &cpu, 0x80002900u) == 1);
    CHECK(external_write_count == 1);
    CHECK(cpu.external_addr == 0x1020u);
    CHECK(cpu.external_value == 0xCAFEBABEu);
    CHECK(cpu.external_rid == 7);

    prepare_call(&cpu, 0x80002A00u);
    cpu.msr = 1u << 13;
    cpu.fpr[2] = 1.25;
    cpu.fpr[3] = 2.5;
    cpu.fpr[5] = 7.0;
    cpu.fpr[6] = 1.5;
    cpu.fpr[8] = 3.0;
    cpu.fpr[9] = 2.0;
    cpu.fpr[11] = 7.0;
    cpu.fpr[12] = 2.0;
    cpu.fpr[14] = 1.25;
    cpu.fpr[15] = 2.5;
    cpu.fpr[17] = 7.0;
    cpu.fpr[18] = 1.5;
    cpu.fpr[20] = 3.0;
    cpu.fpr[21] = 2.0;
    cpu.fpr[23] = 7.0;
    cpu.fpr[24] = 2.0;
    CHECK(run(&loaded, &cpu, 0x80002A00u) == 1);
    CHECK(cpu.fpr[1] == 3.75 && cpu.ps1[1] == 3.75);
    CHECK(cpu.fpr[4] == 5.5 && cpu.ps1[4] == 5.5);
    CHECK(cpu.fpr[7] == 6.0 && cpu.ps1[7] == 6.0);
    CHECK(cpu.fpr[10] == 3.5 && cpu.ps1[10] == 3.5);
    CHECK(cpu.fpr[13] == 3.75);
    CHECK(cpu.fpr[16] == 5.5);
    CHECK(cpu.fpr[19] == 6.0);
    CHECK(cpu.fpr[22] == 3.5);

    prepare_call(&cpu, 0x80002B00u);
    cpu.msr = 1u << 13;
    cpu.fpr[2] = 1.25;
    cpu.ps1[2] = 2.0;
    cpu.fpr[3] = 2.5;
    cpu.ps1[3] = 4.0;
    cpu.fpr[5] = 10.0;
    cpu.ps1[5] = 20.0;
    cpu.fpr[6] = 3.0;
    cpu.ps1[6] = 4.0;
    cpu.fpr[8] = 2.0;
    cpu.ps1[8] = 3.0;
    cpu.fpr[9] = 4.0;
    cpu.ps1[9] = 5.0;
    cpu.fpr[14] = 2.0;
    cpu.ps1[14] = 3.0;
    cpu.fpr[15] = 4.0;
    cpu.ps1[15] = 5.0;
    cpu.fpr[16] = 1.0;
    cpu.ps1[16] = 2.0;
    CHECK(run(&loaded, &cpu, 0x80002B00u) == 1);
    CHECK(cpu.fpr[1] == 3.75 && cpu.ps1[1] == 6.0);
    CHECK(cpu.fpr[7] == 8.0 && cpu.ps1[7] == 15.0);
    CHECK(cpu.fpr[13] == 9.0 && cpu.ps1[13] == 17.0);
    CHECK(cpu.fpr[5] == ppc_approx_reciprocal(3.0));
    CHECK(cpu.ps1[5] == ppc_approx_reciprocal(4.0));
    CHECK(cpu.fpr[4] == cpu.fpr[5] && cpu.ps1[4] == cpu.ps1[6]);

    CHECK(fallback_count == 1);

    // The loop budget has to stop an interpreted loop and hand the pc back.
    prepare_call(&cpu, 0x80002C00u);
    cpu.gpr[3] = 0;
    cpu.downcount = 0;
    fallback_count = 0;
    CHECK(run(&loaded, &cpu, 0x80002C00u) == 1);
    fprintf(stderr, "budget guard: %u iterations, downcount %lld\n", cpu.gpr[3],
            (long long)cpu.downcount);
    CHECK(cpu.gpr[3] > 0);
    CHECK(cpu.gpr[3] < 1000);
    CHECK(cpu.pc >= 0x80002C00u && cpu.pc <= 0x80002C10u);
    CHECK(cpu.pc != cpu.lr);
    CHECK(cpu.downcount < 0 && cpu.downcount > -1024);
    CHECK(fallback_count == (u32)cpu.gpr[3]);

    // Re-entry continues from the saved pc once the chassis has serviced the
    // cycle budget, which is what a real dispatch loop does between calls.
    const u32 first_pass = cpu.gpr[3];
    cpu.downcount = 0;
    CHECK(run(&loaded, &cpu, cpu.pc) == 1);
    CHECK(cpu.gpr[3] > first_pass);

    // An address the module does not cover is reported as a miss, not run.
    CHECK(dolvm_dispatch(&loaded.module, &cpu, 0x80009000u) == 0);
    CHECK(dolvm_dispatch(&loaded.module, &cpu, 0x80001002u) == 0);

    cpu_free(&cpu);
    unload(&loaded);
    return 0;
}

// ---------------------------------------------------------------------------
// Mid-block entry
// ---------------------------------------------------------------------------

// Superblock formation lets values live across guest instructions, and the
// chassis can still dispatch to any address inside one -- an exception resumes
// at the faulting instruction. This walks a straight-line block and enters at
// every address in it, checking each tail against a hand model of the same
// program.
static int test_midblock_entry(void) {
    const u32 words[] = {
        0x38630001u,  // addi  r3, r3, 1
        0x38630002u,  // addi  r3, r3, 2
        0x7C831A14u,  // add   r4, r3, r3
        0x7C852378u,  // or    r5, r4, r4
        0x90A10010u,  // stw   r5, 16(r1)
        0x80C10010u,  // lwz   r6, 16(r1)
        0x38C60007u,  // addi  r6, r6, 7
        0x7CE33214u,  // add   r7, r3, r6
        0x4E800020u,  // blr
    };
    const u32 base = 0x80010000u;
    const u32 count = (u32)(sizeof(words) / sizeof(words[0]));

    DolIRModule ir;
    dolir_module_init(&ir);
    CHECK(add_chunk(&ir, words, count, base));
    CHECK(dolir_verify(&ir, stderr));

    DolVMOptStats stats;
    memset(&stats, 0, sizeof(stats));
    LoadedModule loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(build_and_load(&ir, &loaded, false, &stats));
    dolir_module_free(&ir);
    dolvm_stats_report(&stats, "midblock", stderr);
    // The whole run is straight-line, so it has to have become one block.
    CHECK(stats.blocks_before == count);
    CHECK(stats.blocks_after == 1);
    CHECK(stats.state_reads_forwarded > 0);

    CPUState cpu;
    CHECK(cpu_init(&cpu));
    cpu.msr = 1u << 13;

    for (u32 start = 0; start < count - 1u; start++) {
        // Model the state the guest would be in on arrival at `start`.
        u32 r[8] = {0, 0, 0, 5, 0, 0, 0, 0};
        u32 stack = GC_RAM_BASE + 0x400u;
        u32 memory = 0xDEADBEEFu;
        for (u32 i = 0; i < count - 1u; i++) {
            switch (i) {
            case 0: r[3] += 1; break;
            case 1: r[3] += 2; break;
            case 2: r[4] = r[3] + r[3]; break;
            case 3: r[5] = r[4] | r[4]; break;
            case 4: memory = r[5]; break;
            case 5: r[6] = memory; break;
            case 6: r[6] += 7; break;
            case 7: r[7] = r[3] + r[6]; break;
            default: break;
            }
            if (i + 1u == start) {
                for (u32 n = 3; n < 8; n++)
                    cpu.gpr[n] = r[n];
                cpu.gpr[1] = stack;
                mem_write32(&cpu, stack + 16u, memory);
            }
        }
        if (start == 0) {
            cpu.gpr[3] = 5;
            for (u32 n = 4; n < 8; n++)
                cpu.gpr[n] = 0;
            cpu.gpr[1] = stack;
            mem_write32(&cpu, stack + 16u, 0xDEADBEEFu);
        }
        cpu.lr = 0x80020000u;
        cpu.downcount = 0;
        cpu.exception = 0;
        CHECK(run(&loaded, &cpu, base + start * 4u) == 1);
        CHECK(cpu.pc == 0x80020000u);
        // r7 is the accumulation of everything from `start` onward, so a stale
        // or missing entry recipe shows up as a wrong value here.
        CHECK(cpu.gpr[7] == r[7]);
        CHECK(cpu.gpr[3] == r[3]);
        if (start <= 5)
            CHECK(cpu.gpr[6] == r[6]);
    }

    cpu_free(&cpu);
    unload(&loaded);
    return 0;
}

// ---------------------------------------------------------------------------
// Fused condition-register field updates
// ---------------------------------------------------------------------------

static u32 ppc_cmp(u32 field, u32 ra, u32 rb, bool logical) {
    return 0x7C000000u | (field << 23) | (ra << 16) | (rb << 11) |
           (logical ? 0x40u : 0u);
}

// The optimizer replaces the builder's twenty-nine-instruction CR update with
// one opcode, so the one opcode has to agree with the definition it replaced:
// signed and unsigned compares, every field, and the summary-overflow bit.
static int test_cr_fields(void) {
    const u32 base = 0x80050000u;
    const u32 words[] = {
        ppc_cmp(0, 3, 4, false),  // cmp   cr0, r3, r4
        ppc_cmp(1, 3, 4, true),   // cmpl  cr1, r3, r4
        ppc_cmp(7, 5, 6, false),  // cmp   cr7, r5, r6
        ppc_cmp(4, 5, 6, true),   // cmpl  cr4, r5, r6
        0x4E800020u,              // blr
    };
    const u32 count = (u32)(sizeof(words) / sizeof(words[0]));

    DolIRModule ir;
    dolir_module_init(&ir);
    CHECK(add_chunk(&ir, words, count, base));
    CHECK(dolir_verify(&ir, stderr));

    DolVMOptStats stats;
    memset(&stats, 0, sizeof(stats));
    LoadedModule loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(build_and_load(&ir, &loaded, false, &stats));
    dolir_module_free(&ir);
    CHECK(stats.cr_fields_fused == 4);

    static const u32 values[] = {
        0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu,
    };
    const u32 value_count = (u32)(sizeof(values) / sizeof(values[0]));

    CPUState cpu;
    CHECK(cpu_init(&cpu));
    for (u32 so = 0; so < 2u; so++) {
        for (u32 i = 0; i < value_count; i++) {
            for (u32 j = 0; j < value_count; j++) {
                u32 a = values[i];
                u32 b = values[j];
                cpu.gpr[3] = a;
                cpu.gpr[4] = b;
                cpu.gpr[5] = b;
                cpu.gpr[6] = a;
                cpu.xer = so ? 0x80000000u : 0u;
                cpu.cr = 0x89ABCDEFu;
                cpu.lr = 0x80060000u;
                cpu.downcount = 0;
                cpu.exception = 0;
                CHECK(run(&loaded, &cpu, base) == 1);

                u32 signed_ab = (s32)a < (s32)b ? 8u : (s32)a > (s32)b ? 4u : 2u;
                u32 unsigned_ab = a < b ? 8u : a > b ? 4u : 2u;
                u32 signed_ba = (s32)b < (s32)a ? 8u : (s32)b > (s32)a ? 4u : 2u;
                u32 unsigned_ba = b < a ? 8u : b > a ? 4u : 2u;
                u32 expected = 0x89ABCDEFu;
                struct {
                    u32 field;
                    u32 bits;
                } updates[4] = {
                    {0u, signed_ab}, {1u, unsigned_ab},
                    {7u, signed_ba}, {4u, unsigned_ba},
                };
                for (u32 u = 0; u < 4u; u++) {
                    u32 shift = 4u * (7u - updates[u].field);
                    u32 bits = updates[u].bits | so;
                    expected = (expected & ~(0xFu << shift)) | (bits << shift);
                }
                CHECK(cpu.cr == expected);
            }
        }
    }

    cpu_free(&cpu);
    unload(&loaded);
    return 0;
}

// ---------------------------------------------------------------------------
// Direct intra-module calls
// ---------------------------------------------------------------------------

static int test_direct_calls(void) {
    // A caller that calls a callee twice and returns; with direct calls the
    // whole thing runs in one dispatch.
    // The caller does not save lr, so it leaves with an explicit branch rather
    // than a blr that the second bl would have overwritten.
    const u32 caller[] = {
        0x4800002Du,  // bl   +0x2C   -> 0x80030030
        0x48000029u,  // bl   +0x28   -> 0x80030030
        0x4800FFF4u,  // b    +0xFFF4 -> 0x80040000
    };
    const u32 callee[] = {
        0x38630001u,  // addi r3, r3, 1
        0x4E800020u,  // blr
    };
    DolIRModule ir;
    dolir_module_init(&ir);
    CHECK(add_chunk(&ir, caller, 3, 0x80030004u));
    CHECK(add_chunk(&ir, callee, 2, 0x80030030u));
    CHECK(dolir_verify(&ir, stderr));

    LoadedModule loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(build_and_load(&ir, &loaded, true, NULL));
    dolir_module_free(&ir);

    CPUState cpu;
    CHECK(cpu_init(&cpu));
    cpu.gpr[3] = 0;
    cpu.lr = 0x80040000u;
    cpu.downcount = 0;
    CHECK(run(&loaded, &cpu, 0x80030004u) == 1);
    CHECK(cpu.gpr[3] == 2);
    CHECK(cpu.pc == 0x80040000u);
    cpu_free(&cpu);
    unload(&loaded);

    // Without the opt-in the same branches leave to the chassis instead.
    dolir_module_init(&ir);
    CHECK(add_chunk(&ir, caller, 3, 0x80030004u));
    CHECK(add_chunk(&ir, callee, 2, 0x80030030u));
    memset(&loaded, 0, sizeof(loaded));
    CHECK(build_and_load(&ir, &loaded, false, NULL));
    dolir_module_free(&ir);

    CHECK(cpu_init(&cpu));
    cpu.gpr[3] = 0;
    cpu.lr = 0x80040000u;
    cpu.downcount = 0;
    CHECK(run(&loaded, &cpu, 0x80030004u) == 1);
    CHECK(cpu.pc == 0x80030030u);
    CHECK(cpu.lr == 0x80030008u);
    CHECK(cpu.gpr[3] == 0);
    cpu_free(&cpu);
    unload(&loaded);
    return 0;
}

int main(void) {
    if (test_programs())
        return 1;
    if (test_midblock_entry())
        return 1;
    if (test_cr_fields())
        return 1;
    if (test_direct_calls())
        return 1;
    printf("dolvm: all checks passed\n");
    return 0;
}
