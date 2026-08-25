#include "cpu/cpu.h"
#include "../../GXRuntime/include/core/dispatch_gate.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

void func_80001000(CPUState* cpu);
void func_80002000(CPUState* cpu);
void func_80002100(CPUState* cpu);
void func_80002200(CPUState* cpu);
void func_80002300(CPUState* cpu);
void func_80002400(CPUState* cpu);
void func_80002500(CPUState* cpu);
void func_80002600(CPUState* cpu);
void func_80002700(CPUState* cpu);
void func_80002800(CPUState* cpu);
void func_80002900(CPUState* cpu);
void func_80002A00(CPUState* cpu);
void func_80002B00(CPUState* cpu);
void func_80002B40(CPUState* cpu);
void func_80002B80(CPUState* cpu);
void func_80002C00(CPUState* cpu);
void func_80002D00(CPUState* cpu);
void func_80003000(CPUState* cpu);

static u32 fallback_count;
static int fallback_bad;
static u32 cache_count;
static u8 cache_operation;
static u32 cache_address;
static u32 cache_cia;
static u32 external_write_count;
static u32 journal_count;
static int journal_bad;
static u32 native_gate_calls;
static u32 native_gate_last_chunk;
StaticRecompDispatchGate dolrecomp_native_gate;

static void journal_write(u32 offset, u32 size, void* user) {
    journal_count++;
    journal_bad |= offset != 0 || size != 4 || user != &journal_count;
}

bool dolrecomp_native_gate_allows(CPUState* cpu, u32 chunk_index) {
    native_gate_calls++;
    native_gate_last_chunk = chunk_index;
    return cpu && dolrecomp_native_gate.chunk_open &&
           chunk_index < dolrecomp_native_gate.chunk_count &&
           dolrecomp_native_gate.chunk_open[chunk_index] != 0;
}

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

static void prepare_call(CPUState* cpu, u32 address) {
    cpu->pc = address;
    cpu->lr = 0x81234567u;
    cpu->exception = 0;
    cpu->program_exception = 0;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void) {
    CPUState cpu;
    CHECK(cpu_init(&cpu));
    cpu.pc = 0x80001000u;
    cpu.lr = 0x81234567u;
    cpu.gpr[1] = GC_RAM_BASE;
    cpu.fpr[18] = 2.0;
    cpu.fpr[19] = 3.0;
    cpu.fpr[20] = 4.0;
    cpu.msr = 1u << 13;
    cpu.downcount = 0;
    cpu.instruction_fallback = fallback;
    ppc_set_mem_write_journal(journal_write, &journal_count);
    func_80001000(&cpu);
    ppc_set_mem_write_journal(NULL, NULL);
    CHECK(cpu.gpr[3] == 10);
    CHECK(mem_read32(&cpu, GC_RAM_BASE) == 9);
    CHECK(cpu.pc == 0x81234564u);
    CHECK(cpu.fpr[17] == 10.0 && cpu.ps1[17] == 10.0);
    CHECK(cpu.gpr[5] == 0x12345678u && fallback_count == 1 && !fallback_bad);
    if (journal_count != 10 || journal_bad)
        fprintf(stderr, "journal: count=%u bad=%d\n", journal_count, journal_bad);
    CHECK(journal_count == 10 && !journal_bad);
    CHECK(cpu.downcount < 0 && cpu.downcount > -100);

    prepare_call(&cpu, 0x80002000u);
    cpu.gpr[3] = 0xA5A55A5Au;
    func_80002000(&cpu);
    CHECK(cpu.gpr[4] == 0xA5A55A5Au);
    CHECK(cpu.spr[273] == 0xA5A55A5Au);
    CHECK(cpu.pc == 0x81234564u);

    prepare_call(&cpu, 0x80002100u);
    cpu.gpr[14] = 0x12345678u;
    cpu.gpr[15] = 0xA0000000u;
    cpu.gpr[16] = 0xA0000000u;
    cpu.gpr[13] = 0xA0000000u;
    cpu.sr[3] = 0x87654321u;
    cpu.sr[10] = 0;
    func_80002100(&cpu);
    CHECK(cpu.sr[4] == 0x12345678u);
    CHECK(cpu.gpr[11] == 0x87654321u);
    CHECK(cpu.sr[10] == 0xA0000000u);
    CHECK(cpu.gpr[12] == 0xA0000000u);
    prepare_call(&cpu, 0x80002100u);
    cpu.msr = 1u << 14;
    cpu.gpr[14] = 0xDEADBEEFu;
    cpu.sr[4] = 0x10203040u;
    func_80002100(&cpu);
    CHECK(cpu.exception & PPC_EXC_PROGRAM);
    CHECK(cpu.program_exception & PPC_PROGRAM_PRIV);
    CHECK(cpu.sr[4] == 0x10203040u);
    cpu.msr = 0;

    prepare_call(&cpu, 0x80002200u);
    cpu.msr = 1u << 13;
    cpu.fpscr = 0;
    cpu.fpr[13] = 0;
    func_80002200(&cpu);
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
    func_80002300(&cpu);
    CHECK(cpu.gpr[9] == 0x11223344u);
    CHECK(cpu.gpr[10] == 0x55000000u);

    prepare_call(&cpu, 0x80002400u);
    cpu.cache_control = cache_control;
    cpu.gpr[17] = GC_RAM_BASE + 0x200u;
    cpu.gpr[18] = 0x24u;
    func_80002400(&cpu);
    CHECK(cache_count == 1);
    CHECK(cache_operation == PPC_CACHE_DCBST);
    CHECK(cache_address == GC_RAM_BASE + 0x224u);
    CHECK(cache_cia == 0x80002400u);

    prepare_call(&cpu, 0x80002500u);
    cpu.gpr[3] = 40;
    cpu.gpr[5] = 1;
    func_80002500(&cpu);
    CHECK(cpu.gpr[3] == 41);
    CHECK(cpu.exception == 0);
    prepare_call(&cpu, 0x80002500u);
    cpu.gpr[3] = 40;
    cpu.gpr[5] = 0xFFFFFFFEu;
    func_80002500(&cpu);
    CHECK(cpu.gpr[3] == 40);
    CHECK(cpu.exception & PPC_EXC_PROGRAM);
    CHECK(cpu.program_exception & PPC_PROGRAM_TRAP);
    CHECK(cpu.srr0 == 0x80002500u);

    prepare_call(&cpu, 0x80002600u);
    func_80002600(&cpu);
    CHECK(cpu.exception & PPC_EXC_SYSTEM_CALL);
    CHECK(cpu.srr0 == 0x80002604u);
    CHECK(cpu.pc == PPC_VECTOR_SYSTEM_CALL);

    prepare_call(&cpu, 0x80002700u);
    cpu.msr = 0;
    cpu.srr0 = 0x80004003u;
    cpu.srr1 = 0x00002000u;
    func_80002700(&cpu);
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
    func_80002800(&cpu);
    for (u32 i = 0; i < 32; i++)
        CHECK(mem_read8(&cpu, GC_RAM_BASE + 0x300u + i) == 0);

    prepare_call(&cpu, 0x80002900u);
    cpu.ear = 0x80000007u;
    cpu.external_write32 = external_write32;
    cpu.gpr[11] = 0xCAFEBABEu;
    cpu.gpr[12] = 0x1000u;
    cpu.gpr[13] = 0x20u;
    func_80002900(&cpu);
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
    func_80002A00(&cpu);
    CHECK(cpu.fpr[1] == 3.75 && cpu.ps1[1] == 3.75);
    CHECK(cpu.fpr[4] == 5.5 && cpu.ps1[4] == 5.5);
    CHECK(cpu.fpr[7] == 6.0 && cpu.ps1[7] == 6.0);
    CHECK(cpu.fpr[10] == 3.5 && cpu.ps1[10] == 3.5);
    CHECK(cpu.fpr[13] == 3.75);
    CHECK(cpu.fpr[16] == 5.5);
    CHECK(cpu.fpr[17] == 12.0 && cpu.ps1[17] == 12.0);
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
    func_80002B00(&cpu);
    CHECK(cpu.fpr[1] == 3.75 && cpu.ps1[1] == 6.0);
    CHECK(cpu.fpr[7] == 8.0 && cpu.ps1[7] == 15.0);
    CHECK(cpu.fpr[13] == 9.0 && cpu.ps1[13] == 17.0);
    CHECK(cpu.fpr[5] == ppc_approx_reciprocal(3.0));
    CHECK(cpu.ps1[5] == ppc_approx_reciprocal(4.0));
    CHECK(cpu.fpr[4] == cpu.fpr[5] && cpu.ps1[4] == cpu.ps1[6]);

    prepare_call(&cpu, 0x80002B40u);
    cpu.msr = 1u << 13;
    cpu.fpscr = 4u;
    cpu.fpr[2] = 1.5;
    cpu.fpr[3] = 2.25;
    cpu.fpr[5] = 5.0;
    cpu.fpr[6] = 1.25;
    cpu.fpr[8] = 3.0;
    cpu.fpr[9] = 2.0;
    cpu.fpr[18] = 2.0;
    cpu.ps1[18] = 3.0;
    cpu.fpr[19] = 4.0;
    cpu.fpr[21] = 3.0;
    cpu.ps1[21] = 4.0;
    cpu.ps1[22] = 5.0;
    cpu.fpr[24] = 2.0;
    cpu.ps1[24] = 3.0;
    cpu.fpr[25] = 4.0;
    cpu.fpr[26] = 1.0;
    cpu.ps1[26] = 2.0;
    cpu.fpr[28] = 3.0;
    cpu.ps1[28] = 4.0;
    cpu.ps1[29] = 5.0;
    cpu.fpr[30] = 1.0;
    cpu.ps1[30] = 2.0;
    CPUState expected = cpu;
    ppc_fadds(&expected, 1, 2, 3);
    ppc_fsubs(&expected, 4, 5, 6);
    ppc_fmuls(&expected, 7, 8, 9);
    ppc_ps_muls0(&expected, 17, 18, 19);
    ppc_ps_muls1(&expected, 20, 21, 22);
    ppc_ps_madds0(&expected, 23, 24, 25, 26);
    ppc_ps_madds1(&expected, 27, 28, 29, 30);
    func_80002B40(&cpu);
    CHECK(memcmp(cpu.fpr, expected.fpr, sizeof(cpu.fpr)) == 0);
    CHECK(memcmp(cpu.ps1, expected.ps1, sizeof(cpu.ps1)) == 0);
    CHECK(cpu.fpscr == expected.fpscr);

    // Non-finite results and denormal C operands must leave the inline paths
    // and retain the exact helpers' exception, rounding and result behavior.
    prepare_call(&cpu, 0x80002B40u);
    cpu.fpscr = 4u;
    cpu.fpr[2] = INFINITY;
    cpu.fpr[3] = -INFINITY;
    cpu.fpr[5] = INFINITY;
    cpu.fpr[6] = INFINITY;
    cpu.fpr[9] = 1.0e-310;
    cpu.fpr[19] = 1.0e-310;
    cpu.ps1[22] = 1.0e-310;
    cpu.fpr[25] = 1.0e-310;
    cpu.ps1[29] = 1.0e-310;
    expected = cpu;
    ppc_fadds(&expected, 1, 2, 3);
    ppc_fsubs(&expected, 4, 5, 6);
    ppc_fmuls(&expected, 7, 8, 9);
    ppc_ps_muls0(&expected, 17, 18, 19);
    ppc_ps_muls1(&expected, 20, 21, 22);
    ppc_ps_madds0(&expected, 23, 24, 25, 26);
    ppc_ps_madds1(&expected, 27, 28, 29, 30);
    func_80002B40(&cpu);
    CHECK(memcmp(cpu.fpr, expected.fpr, sizeof(cpu.fpr)) == 0);
    CHECK(memcmp(cpu.ps1, expected.ps1, sizeof(cpu.ps1)) == 0);
    CHECK(cpu.fpscr == expected.fpscr);

    // The common unquantised PSQ form stays inside generated SSA/memory code;
    // quantised and exceptional forms use the exact helper behind the same
    // generated function. Exercise both arms so the fast-path guard cannot
    // silently weaken architectural checks.
    prepare_call(&cpu, 0x80002B80u);
    cpu.msr = 1u << 13;
    cpu.hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
    cpu.gqr[0] = 0;
    cpu.gpr[3] = GC_RAM_BASE + 0x400u;
    mem_write32(&cpu, cpu.gpr[3], 0x3FC00000u);
    mem_write32(&cpu, cpu.gpr[3] + 4u, 0xC0200000u);
    func_80002B80(&cpu);
    CHECK(cpu.fpr[1] == 1.5 && cpu.ps1[1] == -2.5);
    CHECK(mem_read32(&cpu, cpu.gpr[3] + 8u) == 0x3FC00000u);
    CHECK(mem_read32(&cpu, cpu.gpr[3] + 12u) == 0xC0200000u);

    prepare_call(&cpu, 0x80002B80u);
    cpu.msr = 1u << 13;
    cpu.hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
    cpu.gqr[0] = 0x01040104u;
    cpu.gpr[3] = GC_RAM_BASE + 0x420u;
    mem_write8(&cpu, cpu.gpr[3], 10u);
    mem_write8(&cpu, cpu.gpr[3] + 1u, 246u);
    func_80002B80(&cpu);
    CHECK(cpu.fpr[1] == 5.0 && cpu.ps1[1] == 123.0);
    CHECK(mem_read8(&cpu, cpu.gpr[3] + 8u) == 10u);
    CHECK(mem_read8(&cpu, cpu.gpr[3] + 9u) == 246u);

    prepare_call(&cpu, 0x80002B80u);
    cpu.msr = 1u << 13;
    cpu.hid2 = 0;
    cpu.gqr[0] = 0;
    cpu.gpr[3] = GC_RAM_BASE + 0x440u;
    func_80002B80(&cpu);
    CHECK(cpu.exception & PPC_EXC_PROGRAM);
    CHECK(cpu.program_exception & PPC_PROGRAM_ILLEGAL);

    prepare_call(&cpu, 0x80002B80u);
    cpu.msr = 1u << 13;
    cpu.hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
    cpu.gqr[0] = 0;
    cpu.gpr[3] = GC_RAM_BASE + 0x441u;
    func_80002B80(&cpu);
    CHECK(cpu.exception & PPC_EXC_ALIGNMENT);

    CHECK(fallback_count == 1);

    // A fallback in the loop must not restart the dispatcher budget.
    prepare_call(&cpu, 0x80002C00u);
    cpu.gpr[3] = 0;
    cpu.downcount = 0;
    fallback_count = 0;
    u8 open_chunks[] = {1, 1};
    s32 live_budget = 64;
    u32 pending = 0;
    dolrecomp_native_gate = (StaticRecompDispatchGate){
        .chunk_open = open_chunks,
        .chunk_count = 2,
        .budget = &live_budget,
        .pending = &pending,
        .pending_sync = 1u,
        .pending_async = 2u,
    };
    func_80002C00(&cpu);
    fprintf(stderr, "budget guard: %u iterations, downcount %lld\n",
            cpu.gpr[3], (long long)cpu.downcount);
    CHECK(cpu.gpr[3] > 0);
    CHECK(cpu.gpr[3] < 1000);
    CHECK(cpu.pc >= 0x80002C00u && cpu.pc <= 0x80002C10u);
    CHECK(cpu.pc != cpu.lr);
    CHECK(cpu.downcount <= -64 && cpu.downcount > -128);
    CHECK(fallback_count == (u32)cpu.gpr[3]);

    // Re-entry starts a new budget and continues from the saved PC.
    const u32 first_pass = cpu.gpr[3];
    cpu.downcount = 0;
    func_80002C00(&cpu);
    CHECK(cpu.gpr[3] > first_pass);

    live_budget = 256;
    prepare_call(&cpu, 0x80002D00u);
    cpu.downcount = 0;
    open_chunks[1] = 0;
    native_gate_calls = 0;
    native_gate_last_chunk = ~0u;
    func_80002D00(&cpu);
    CHECK(native_gate_calls == 1);
    CHECK(native_gate_last_chunk == 1u);
    CHECK(cpu.pc == 0x80002E00u);

    prepare_call(&cpu, 0x80002D00u);
    cpu.downcount = 0;
    open_chunks[1] = 1;
    native_gate_calls = 0;
    func_80002D00(&cpu);
    CHECK(native_gate_calls > 1);
    CHECK(cpu.pc == 0x80002D00u || cpu.pc == 0x80002E00u);
    CHECK(cpu.downcount <= -128 && cpu.downcount >= -512);

    // Budget exit from a canonicalized mtctr/dcbz loop must resume at the
    // actual branch target, not the synthetic preheader. Otherwise re-entry
    // resets CTR and clears beyond the requested range.
    memset(&dolrecomp_native_gate, 0, sizeof(dolrecomp_native_gate));
    prepare_call(&cpu, 0x80003000u);
    cpu.gpr[3] = GC_RAM_BASE;
    cpu.gpr[4] = 500;
    cpu.downcount = 0;
    func_80003000(&cpu);
    CHECK(cpu.pc == 0x80003004u);
    CHECK(cpu.ctr > 0 && cpu.ctr < 500);
    u32 loop_dispatches = 1;
    const u32 loop_return = cpu.lr & ~3u;
    while (cpu.pc != loop_return && loop_dispatches++ < 16)
        func_80003000(&cpu);
    CHECK(cpu.pc == loop_return);
    CHECK(cpu.ctr == 0);
    CHECK(cpu.gpr[3] == GC_RAM_BASE + 500u * 32u);

    cpu_free(&cpu);
    return 0;
}
