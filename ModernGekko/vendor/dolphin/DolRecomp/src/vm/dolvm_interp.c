// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DolVM interpreter.
//
// This is the whole runtime half of the no-JIT port: it walks a bytecode module
// and mutates `CPUState`. It allocates nothing, maps nothing executable, and
// calls only the same `ppc_*` helpers the native backends call, so a module that
// runs correctly here runs correctly there.
//
// Four decisions carry most of the speed.
//
// A dispatch runs until the chassis needs it back, not until the next call.
// Given a gate (dolvm.h) saying which regions the chassis would dispatch into
// itself, the interpreter follows calls, tail calls and returns into any of
// them and stops only when the chassis's slice counter says to, when an edge
// leads somewhere closed, or when the chassis is holding an exception. Without
// one it behaves exactly as the native backends do. And a block that does
// nothing but poll one word of memory and branch back to itself -- a guest
// waiting for an interrupt -- is skipped rather than run, the way Dolphin's
// JITs skip it: the back edge charges the rest of the slice and leaves.
//
// The guest's registers live in the VM's. All 32 general purpose registers plus
// LR, CTR and XER have a fixed home at the top of the register file, filled
// from CPUState when a dispatch begins and written back when it ends. The point
// is not that a register file is faster than a struct field -- both are memory
// -- but that the *opcodes* disappear: a guest register access stops being a
// dispatched load or store and becomes an operand of the instruction that
// wanted it. On a real title that was a quarter to a third of everything the
// interpreter executed. The price is a fill and a flush per dispatch, which
// vectorize into a few dozen instructions, and the rule that a helper reaching
// into CPUState for something a home is holding has to be bracketed.
//
// The rest of the register file is still scratch: block-local DolIR
// temporaries, never read across a dispatch boundary.
//
// Dispatch is direct-threaded where the compiler supports computed goto, and a
// plain switch elsewhere. Both share one body via NEXT(), and every way out of
// it goes through `leave` so the flush cannot be forgotten on a path. Threaded
// code only pays if every handler keeps its own indirect branch, and LLVM's
// tail merger folds them into one unless told not to; the build flags that
// tell it are in ModernGekko's CMakeLists.txt, and the README under src/vm
// says what they are worth.

#include "vm/dolvm.h"
#include "vm/dolvm_interp.h"
#include "cpu/cpu.h"
#include "ir/dolir.h"

#include <math.h>
#include <string.h>

#if defined(__GNUC__) && !defined(DOLVM_FORCE_SWITCH)
#define DOLVM_THREADED 1
#endif

#if defined(__GNUC__)
#define DOLVM_ALWAYS_INLINE inline __attribute__((always_inline))
#define DOLVM_NOINLINE __attribute__((noinline))
#else
#define DOLVM_ALWAYS_INLINE inline
#define DOLVM_NOINLINE
#endif

// CPUState words a profiled build keeps a per-slot counter for.
#define DOLVM_PROFILE_SLOTS 2048u

typedef union {
    u64 u;
    f64 d;
} DolVMReg;

// Filling and flushing the homed registers.
//
// A dispatch reads the guest's general purpose registers, LR and CTR out of
// CPUState once on the way in and writes them back once on the way out;
// everything between operates on the register file. Both directions are a
// straight run over contiguous memory -- 128 bytes of CPUState against 256 of
// the register file -- so the host vectorizes them into a handful of
// instructions, against the two dispatched opcodes per access they replace.
// XER is homed whether or not the module homes anything else: the carrying
// arithmetic reads it, inserts into it and writes it back once per instruction,
// and every condition-register update reads its summary-overflow bit.
static inline void dolvm_xer_in(const CPUState* ctx, DolVMReg* regs) {
    regs[DOLVM_HOME_XER].u = ctx->xer;
}

static inline void dolvm_xer_out(CPUState* ctx, const DolVMReg* regs) {
    ctx->xer = (u32)regs[DOLVM_HOME_XER].u;
}

static void dolvm_homes_fill(const CPUState* ctx, DolVMReg* regs) {
    for (u32 i = 0; i < 32u; i++)
        regs[DOLVM_HOME_BASE + i].u = ctx->gpr[i];
    regs[DOLVM_HOME_LR].u = ctx->lr;
    regs[DOLVM_HOME_CTR].u = ctx->ctr;
}

static void dolvm_homes_flush(CPUState* ctx, const DolVMReg* regs) {
    for (u32 i = 0; i < 32u; i++)
        ctx->gpr[i] = (u32)regs[DOLVM_HOME_BASE + i].u;
    ctx->lr = (u32)regs[DOLVM_HOME_LR].u;
    ctx->ctr = (u32)regs[DOLVM_HOME_CTR].u;
}

// The LT, GT and EQ bits of a condition-register field, as the compare
// instructions set them: exactly one of the three, at bit 3, 2 or 1. Written as
// a shift of a two-bit index so it compiles to two flag extracts and a shift
// rather than a chain of selects -- this runs on every counted loop's back
// edge.
static inline u32 dolvm_cr_bits(u32 left, u32 right, bool is_signed) {
    u32 lt, gt;
    if (is_signed) {
        lt = (s32)left < (s32)right;
        gt = (s32)left > (s32)right;
    } else {
        lt = left < right;
        gt = left > right;
    }
    return 2u << ((lt << 1) | gt);
}

// XER's summary-overflow bit, which every condition-register field update
// copies into its low bit. XER is homed whatever else the module does, so this
// costs a register read and no test -- the alternative was a select on the
// hottest handler a counted loop has.
static inline u32 dolvm_summary_overflow(const DolVMReg* regs) {
    return ((u32)regs[DOLVM_HOME_XER].u >> 31) & 1u;
}

// The MSR bit that says the floating-point unit is enabled (PPC bit 18).
#define DOLVM_MSR_FP 0x00002000u
// And the one that says the guest is in user mode (PPC bit 17).
#define DOLVM_MSR_PR 0x00004000u
// And the one that says external interrupts are enabled (PPC bit 16).
#define DOLVM_MSR_EE 0x00008000u

static inline u64 dolvm_payload(const DolVMInst* inst) {
    u64 value;
    memcpy(&value, inst, sizeof(value));
    return value;
}

// The Gekko's single-precision conversions, byte for byte the ones the C
// backend emits (dolrecomp_f32_from_bits / dolrecomp_f32_to_bits) and the ones
// Dolphin uses. They are not an IEEE narrow-and-round: lfs leaves a denormal
// single's bit pattern alone rather than normalizing it, and stfs truncates the
// mantissa rather than rounding it. Games depend on the difference, so the
// interpreter reproduces the shipping behaviour instead of the IR's nominal
// fpext/fptrunc.
static f64 dolvm_single_to_double(u32 bits) {
    u64 x = bits;
    u64 exponent = (x >> 23) & 0xFFu;
    u64 fraction = x & 0x007FFFFFu;
    u64 result;
    if (exponent > 0 && exponent < 255) {
        u64 y = !(exponent >> 7);
        u64 z = (y << 61) | (y << 60) | (y << 59);
        result = ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    } else if (exponent == 0 && fraction != 0) {
        exponent = 1023 - 126;
        do {
            fraction <<= 1;
            exponent -= 1;
        } while ((fraction & 0x00800000u) == 0);
        result = ((x & 0x80000000u) << 32) | (exponent << 52) |
                 ((fraction & 0x007FFFFFu) << 29);
    } else {
        u64 y = exponent >> 7;
        u64 z = (y << 61) | (y << 60) | (y << 59);
        result = ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    }
    f64 value;
    memcpy(&value, &result, sizeof(value));
    return value;
}

static u32 dolvm_double_to_single(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    u32 exponent = (u32)((bits >> 52) & 0x7FFu);
    if (exponent > 896 || (bits & 0x7FFFFFFFFFFFFFFFull) == 0)
        return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
    if (exponent >= 874) {
        u32 result =
            (u32)(0x80000000u | ((bits & 0x000FFFFFFFFFFFFFull) >> 21));
        result >>= 905 - exponent;
        result |= (u32)((bits >> 32) & 0x80000000u);
        return result;
    }
    return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
}

static inline u64 dolvm_f64_to_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline f64 dolvm_f64_from_bits(u64 bits) {
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline u32 dolvm_clz32(u32 value) {
#if defined(__GNUC__)
    return value ? (u32)__builtin_clz(value) : 32u;
#else
    u32 count = 0;
    if (!value)
        return 32u;
    while (!(value & 0x80000000u)) {
        value <<= 1;
        count++;
    }
    return count;
#endif
}

static inline u32 dolvm_clz64(u64 value) {
#if defined(__GNUC__)
    return value ? (u32)__builtin_clzll(value) : 64u;
#else
    u32 count = 0;
    if (!value)
        return 64u;
    while (!(value & 0x8000000000000000ull)) {
        value <<= 1;
        count++;
    }
    return count;
#endif
}

// The guest's memory map does not move while a dispatch runs, but the compiler
// cannot know that -- every helper call could in principle write through ctx --
// so it reloads the base and the size on each access. Taking a copy once per
// dispatch is two fewer loads on the hottest path there is.
typedef struct {
    const u8* ram;
    u32 ram_size;
    const u8* exram;
    u32 exram_size;
    PPCMemWriteJournal journal;
    void* journal_user;
    // The register file, so an access the chassis has to service can put
    // CPUState back together first. `homed` says whether that is everything or
    // only XER, which is homed whatever the module does.
    DolVMReg* homes;
    bool homed;
    // The last read the chassis had to service, and how many times in a row it
    // has come back unchanged. A guest that reads one hardware register from
    // one instruction and gets the same answer every time is waiting, not
    // working: nothing it does in the loop can change what it is reading, so
    // the answer can only come from an interrupt or from an event the chassis
    // has yet to run. See DOLVM_POLL_SPIN.
    u32 poll_pc;
    u32 poll_address;
    u64 poll_value;
    u32 poll_run;
    // Whether the run was extended since the last back edge looked at it. A
    // long run left behind by a loop that has since exited must not make the
    // *next* loop's first back edge look like a wait.
    u32 poll_fresh;
} DolVMMemory;

// How many identical hardware reads in a row make a wait. The interpreter then
// treats the next back edge the way it treats a recognised idle loop: charge
// what is left of the slice and hand control back, so whatever the guest is
// waiting for actually happens.
//
// The shape this exists for is the one the simple idle-loop test cannot see,
// because the poll is behind a call:
//
//     loop: bl GXGetGPStatus       ; lhz from 0xCC000000, unpacks five bits
//           lbz r0, overhi
//           cmpwi r0, 0
//           beq loop
//
// Disney's Extreme Skate Adventure sits in exactly that loop for 45% of every
// frame -- it is the game's idle thread -- and `overhi` is the command
// processor's FIFO-overflow bit, which cannot become set while the loop runs
// because only a FIFO *write* can set it. Running it faithfully cost more than
// half of everything the interpreter did.
//
// A run only counts while the address, the instruction reading it and the value
// all stay the same, and any write the chassis has to service resets it, so a
// loop that is making progress never reaches the threshold.
#ifndef DOLVM_POLL_SPIN
#define DOLVM_POLL_SPIN 16u
#endif

// Waits that have already been proved. The threshold above exists to be sure a
// loop really is waiting, and paying it again in every timing slice is pure
// repetition: the spin in Disney skate is re-entered around six hundred
// thousand times over a run, and each re-entry spun the full threshold before
// the interpreter would believe it again. A read that has once come back
// unchanged DOLVM_POLL_SPIN times in a row is trusted from its first
// occurrence afterwards -- but only while the address, the instruction and the
// value all still match, so the read that finally answers the wait is not
// covered by the site that recorded the waiting.
#define DOLVM_POLL_SITES 8u
static struct {
    u32 pc;
    u32 address;
    u64 value;
    u32 live;
} g_dolvm_poll_sites[DOLVM_POLL_SITES];
static u32 g_dolvm_poll_next;

// Runtime off switch for the whole polled-wait mechanism. The skip trades
// timing fidelity for speed, and the failure mode if it is too aggressive is
// the guest running ahead of the emulated GPU -- full speed, wrong picture --
// which only shows up on a host slow enough for the GPU to be the thing being
// outrun. Being able to turn it off in one run is the difference between
// knowing and guessing.
int g_dolvm_poll_skip_enabled = 1;

// See dolvm_dispatch: fills the local register file with an even poison so a
// read-before-write is deterministic rather than lucky. Off by default.
int g_dolvm_poison_registers = 0;

// The byte the poison fills with. Both polarities matter: a condition is tested
// for non-zero, so an all-zero fill makes an unwritten flag read false and a
// non-zero fill makes it read true. A bug that survives one is caught by the
// other, which is why the suite runs both.
int g_dolvm_poison_byte = 0xA0;

static void dolvm_poll_remember(u32 pc, u32 address, u64 value) {
    for (u32 i = 0; i < DOLVM_POLL_SITES; i++)
        if (g_dolvm_poll_sites[i].live && g_dolvm_poll_sites[i].pc == pc &&
            g_dolvm_poll_sites[i].address == address) {
            g_dolvm_poll_sites[i].value = value;
            return;
        }
    u32 slot = g_dolvm_poll_next++ % DOLVM_POLL_SITES;
    g_dolvm_poll_sites[slot].pc = pc;
    g_dolvm_poll_sites[slot].address = address;
    g_dolvm_poll_sites[slot].value = value;
    g_dolvm_poll_sites[slot].live = 1;
}

static bool dolvm_poll_known(u32 pc, u32 address, u64 value) {
    for (u32 i = 0; i < DOLVM_POLL_SITES; i++)
        if (g_dolvm_poll_sites[i].live && g_dolvm_poll_sites[i].pc == pc &&
            g_dolvm_poll_sites[i].address == address &&
            g_dolvm_poll_sites[i].value == value)
            return true;
    return false;
}

// The MEM1 hit is the whole of a guest access almost always, and it is inlined
// into every load and store handler with the width a constant: a range check,
// the load and a byte swap. Everything else -- MEM2, the chassis, the homed
// registers going back and forth around it -- is out of line, because code
// that runs once in a million accesses only costs instruction cache when it
// sits next to code that runs every time.
static inline bool dolvm_ram_load(const DolVMMemory* mem, u32 address,
                                  u32 width, u64* out) {
    u32 offset = (address & ~0x40000000u) - GC_RAM_BASE;
    if (mem->ram_size < width || offset > mem->ram_size - width)
        return false;
    const u8* p = mem->ram + offset;
    switch (width) {
    case 1: *out = p[0]; break;
    case 2: *out = read_be16(p); break;
    case 4: *out = read_be32(p); break;
    default: *out = read_be64(p); break;
    }
    return true;
}

#ifdef DOLVM_PROFILE
// Which guest addresses leave RAM. An access the chassis has to service costs
// two orders of magnitude more than one that hits MEM1 -- the homed registers
// go out and come back around it, and on this chassis a command-processor read
// runs the emulated GPU -- so knowing *which* register a title polls is worth
// more than knowing how many slow accesses there were.
#define DOLVM_PROFILE_MMIO_SITES 4096u
static u32 g_dolvm_mmio_addr[DOLVM_PROFILE_MMIO_SITES];
static u64 g_dolvm_mmio_count[DOLVM_PROFILE_MMIO_SITES];
static void dolvm_count_mmio(u32 address) {
    u32 slot = ((address >> 1) * 2654435761u) & (DOLVM_PROFILE_MMIO_SITES - 1u);
    for (u32 probe = 0; probe < DOLVM_PROFILE_MMIO_SITES; probe++) {
        u32 i = (slot + probe) & (DOLVM_PROFILE_MMIO_SITES - 1u);
        if (!g_dolvm_mmio_count[i] || g_dolvm_mmio_addr[i] == address) {
            g_dolvm_mmio_addr[i] = address;
            g_dolvm_mmio_count[i]++;
            return;
        }
    }
}
#define DOLVM_COUNT_MMIO(a) dolvm_count_mmio(a)

// Same, for the write side, which on a title that draws a lot is by far the
// busier of the two.
static u32 g_dolvm_mmio_waddr[DOLVM_PROFILE_MMIO_SITES];
static u64 g_dolvm_mmio_wcount[DOLVM_PROFILE_MMIO_SITES];
static void dolvm_count_mmio_write(u32 address) {
    u32 slot = ((address >> 1) * 2654435761u) & (DOLVM_PROFILE_MMIO_SITES - 1u);
    for (u32 probe = 0; probe < DOLVM_PROFILE_MMIO_SITES; probe++) {
        u32 i = (slot + probe) & (DOLVM_PROFILE_MMIO_SITES - 1u);
        if (!g_dolvm_mmio_wcount[i] || g_dolvm_mmio_waddr[i] == address) {
            g_dolvm_mmio_waddr[i] = address;
            g_dolvm_mmio_wcount[i]++;
            return;
        }
    }
}
#define DOLVM_COUNT_MMIO_WRITE(a) dolvm_count_mmio_write(a)
#else
#define DOLVM_COUNT_MMIO(a) ((void)0)
#define DOLVM_COUNT_MMIO_WRITE(a) ((void)0)
#endif

static DOLVM_NOINLINE bool dolvm_guest_load_slow(CPUState* ctx,
                                                 DolVMMemory* mem,
                                                 u32 address, u32 width, u32 pc,
                                                 u64* out) {
    u32 normalized = address & ~0x40000000u;
    u32 mem2_offset = normalized - WII_MEM2_BASE;
    if (mem->exram && mem->exram_size >= width &&
        mem2_offset <= mem->exram_size - width) {
        const u8* p = mem->exram + mem2_offset;
        switch (width) {
        case 1: *out = p[0]; break;
        case 2: *out = read_be16(p); break;
        case 4: *out = read_be32(p); break;
        default: *out = read_be64(p); break;
        }
        return true;
    }
    if (!ctx->external_read) {
        *out = 0;
        return true;
    }
    DOLVM_COUNT_MMIO(address);
    ctx->pc = pc;
    // The chassis owns CPUState for the length of this call and may do anything
    // with it, so the homed registers go back before it and are read out again
    // after. Only an access the chassis has to service pays this; a hit in RAM
    // never reaches here.
    dolvm_xer_out(ctx, mem->homes);
    if (mem->homed)
        dolvm_homes_flush(ctx, mem->homes);
    u64 value = ctx->external_read(ctx, address, (u8)width);
    dolvm_xer_in(ctx, mem->homes);
    if (mem->homed)
        dolvm_homes_fill(ctx, mem->homes);
    // The chassis hands back a full word whatever the access width was; only
    // the accessed bytes are the load's result, exactly as cpu.c's mem_read8
    // and friends narrow it.
    *out = width >= 8u ? value : (value & ((1ull << (width * 8u)) - 1ull));
    if (address == mem->poll_address && pc == mem->poll_pc &&
        *out == mem->poll_value) {
        if (mem->poll_run < DOLVM_POLL_SPIN &&
            ++mem->poll_run == DOLVM_POLL_SPIN)
            dolvm_poll_remember(pc, address, *out);
    } else {
        mem->poll_pc = pc;
        mem->poll_address = address;
        mem->poll_value = *out;
        mem->poll_run = (g_dolvm_poll_skip_enabled &&
                         dolvm_poll_known(pc, address, *out))
                            ? DOLVM_POLL_SPIN
                            : 1u;
    }
    mem->poll_fresh = 1;
    return ctx->exception == 0;
}

// Guest loads and stores follow the native backends exactly: fold away the
// uncached-window bit, try MEM1, then MEM2, then hand the access to the
// chassis. A chassis access can raise, and a raised exception means this
// dispatch is over, so both return false to unwind the interpreter.
static DOLVM_ALWAYS_INLINE bool dolvm_guest_load(CPUState* ctx,
                                                 DolVMMemory* mem,
                                                 u32 address, u32 width, u32 pc,
                                                 u64* out) {
    if (dolvm_ram_load(mem, address, width, out))
        return true;
    return dolvm_guest_load_slow(ctx, mem, address, width, pc, out);
}

// A store to mapped memory anywhere on the reserved cache line drops the
// reservation. Both addresses are folded out of the uncached window first, and
// a store the chassis handles does not clear anything -- which is what cpu.c
// does, and cpu.c is what the shipping backend calls.
static inline void dolvm_clear_reservation(CPUState* ctx, u32 address) {
    // Almost no store lands on a reserved line, and most of the time there is
    // no reservation at all, so the cheap test goes first: reading
    // reserve_addr before knowing there is one is a load per guest store.
    if (!ctx->reserve_valid)
        return;
    u32 reserved = ctx->reserve_addr & ~0x40000000u;
    u32 stored = address & ~0x40000000u;
    if (((reserved ^ stored) & ~31u) == 0)
        ctx->reserve_valid = false;
}

static DOLVM_NOINLINE bool dolvm_guest_store_slow(CPUState* ctx,
                                                  DolVMMemory* mem,
                                                  u32 address, u64 value,
                                                  u32 width, u32 pc) {
    u32 normalized = address & ~0x40000000u;
    u32 mem2_offset = normalized - WII_MEM2_BASE;
    if (mem->exram && mem->exram_size >= width &&
        mem2_offset <= mem->exram_size - width) {
        dolvm_clear_reservation(ctx, address);
        u8* p = (u8*)mem->exram + mem2_offset;
        switch (width) {
        case 1: p[0] = (u8)value; break;
        case 2: write_be16(p, (u16)value); break;
        case 4: write_be32(p, (u32)value); break;
        default: write_be64(p, value); break;
        }
        return true;
    }
    if (!ctx->external_write)
        return true;
    DOLVM_COUNT_MMIO_WRITE(address);
    mem->poll_run = 0;
    ctx->pc = pc;
    // Only the accessed bytes reach the chassis, matching the narrowed value
    // cpu.c's mem_write8/16/32 pass on.
    if (width < 8u)
        value &= (1ull << (width * 8u)) - 1ull;
    dolvm_xer_out(ctx, mem->homes);
    if (mem->homed)
        dolvm_homes_flush(ctx, mem->homes);
    ctx->external_write(ctx, address, value, (u8)width);
    dolvm_xer_in(ctx, mem->homes);
    if (mem->homed)
        dolvm_homes_fill(ctx, mem->homes);
    return ctx->exception == 0;
}

static DOLVM_ALWAYS_INLINE bool dolvm_guest_store(CPUState* ctx,
                                                  DolVMMemory* mem,
                                                  u32 address, u64 value,
                                                  u32 width, u32 pc) {
    u32 offset = (address & ~0x40000000u) - GC_RAM_BASE;
    if (mem->ram_size >= width && offset <= mem->ram_size - width) {
        dolvm_clear_reservation(ctx, address);
        if (mem->journal)
            mem->journal(offset, width, mem->journal_user);
        u8* p = (u8*)mem->ram + offset;
        switch (width) {
        case 1: p[0] = (u8)value; break;
        case 2: write_be16(p, (u16)value); break;
        case 4: write_be32(p, (u32)value); break;
        default: write_be64(p, value); break;
        }
        return true;
    }
    return dolvm_guest_store_slow(ctx, mem, address, value, width, pc);
}

// fadd/fmul/fmadd and friends are not plain IEEE here: the Gekko rounds through
// FPSCR, flushes to single where the mnemonic says so, and may decline to write
// the destination at all. cpu.c already encodes all of that, so the exact-float
// helpers reproduce the native backends' lowering call for call.
static void dolvm_exact_float(CPUState* ctx, u64 descriptor) {
    u32 op = (u32)(descriptor & 0xFFu);
    u32 d = (u32)((descriptor >> 8) & 0xFFu);
    u32 a = (u32)((descriptor >> 16) & 0xFFu);
    u32 b = (u32)((descriptor >> 24) & 0xFFu);
    u32 c = (u32)((descriptor >> 32) & 0xFFu);
    u32 crfd = (u32)((descriptor >> 40) & 0xFFu);

    switch (op) {
    case DOLIR_EXACT_FCMPU:
    case DOLIR_EXACT_FCMPO:
        ppc_fcmp(ctx, (u8)crfd, ctx->fpr[a], ctx->fpr[b],
                 op == DOLIR_EXACT_FCMPO);
        return;
    case DOLIR_EXACT_FADDS: ppc_fadds(ctx, (u8)d, (u8)a, (u8)b); return;
    case DOLIR_EXACT_FSUBS: ppc_fsubs(ctx, (u8)d, (u8)a, (u8)b); return;
    case DOLIR_EXACT_FMULS: ppc_fmuls(ctx, (u8)d, (u8)a, (u8)c); return;
    case DOLIR_EXACT_FDIVS: ppc_fdivs(ctx, (u8)d, (u8)a, (u8)b); return;
    case DOLIR_EXACT_FADD: ppc_fadd(ctx, (u8)d, (u8)a, (u8)b); return;
    case DOLIR_EXACT_FSUB: ppc_fsub(ctx, (u8)d, (u8)a, (u8)b); return;
    case DOLIR_EXACT_FMUL: ppc_fmul(ctx, (u8)d, (u8)a, (u8)c); return;
    case DOLIR_EXACT_FDIV: ppc_fdiv(ctx, (u8)d, (u8)a, (u8)b); return;
    case DOLIR_EXACT_FRSP: ppc_frsp(ctx, (u8)d, (u8)b); return;
    case DOLIR_EXACT_FCTIW:
    case DOLIR_EXACT_FCTIWZ: {
        u64 result = dolvm_f64_to_bits(ctx->fpr[d]);
        if (ppc_fctiw(ctx, ctx->fpr[b], op == DOLIR_EXACT_FCTIWZ, &result))
            ctx->fpr[d] = dolvm_f64_from_bits(result);
        return;
    }
    case DOLIR_EXACT_FRES:
    case DOLIR_EXACT_FRSQRTE: {
        f64 estimate = ctx->fpr[d];
        bool ok = op == DOLIR_EXACT_FRES
                      ? ppc_fres(ctx, ctx->fpr[b], &estimate)
                      : ppc_frsqrte(ctx, ctx->fpr[b], &estimate);
        if (ok) {
            ctx->fpr[d] = estimate;
            if (op == DOLIR_EXACT_FRES)
                ctx->ps1[d] = estimate;
        }
        return;
    }
    default: {
        bool single = op >= DOLIR_EXACT_FMADDS && op <= DOLIR_EXACT_FNMSUBS;
        bool subtract = op == DOLIR_EXACT_FMSUB || op == DOLIR_EXACT_FNMSUB ||
                        op == DOLIR_EXACT_FMSUBS || op == DOLIR_EXACT_FNMSUBS;
        bool negative = op == DOLIR_EXACT_FNMADD || op == DOLIR_EXACT_FNMSUB ||
                        op == DOLIR_EXACT_FNMADDS || op == DOLIR_EXACT_FNMSUBS;
        f64 fused = ctx->fpr[d];
        if (ppc_fma(ctx, ctx->fpr[a], ctx->fpr[c], ctx->fpr[b], single,
                    subtract, negative, &fused)) {
            ctx->fpr[d] = fused;
            if (single)
                ctx->ps1[d] = fused;
        }
        return;
    }
    }
}

static void dolvm_exact_paired(CPUState* ctx, u64 descriptor) {
    u32 op = (u32)(descriptor & 0xFFu);
    u8 d = (u8)((descriptor >> 8) & 0xFFu);
    u8 a = (u8)((descriptor >> 16) & 0xFFu);
    u8 b = (u8)((descriptor >> 24) & 0xFFu);
    u8 c = (u8)((descriptor >> 32) & 0xFFu);
    u8 crfd = (u8)((descriptor >> 40) & 0xFFu);

    switch (op) {
    case DOLIR_EXACT_PS_ADD: ppc_ps_add_op(ctx, d, a, b); return;
    case DOLIR_EXACT_PS_SUB: ppc_ps_sub_op(ctx, d, a, b); return;
    case DOLIR_EXACT_PS_MUL: ppc_ps_mul_op(ctx, d, a, c); return;
    case DOLIR_EXACT_PS_DIV: ppc_ps_div_op(ctx, d, a, b); return;
    case DOLIR_EXACT_PS_MADD: ppc_ps_madd_op(ctx, d, a, c, b, false, false); return;
    case DOLIR_EXACT_PS_MSUB: ppc_ps_madd_op(ctx, d, a, c, b, true, false); return;
    case DOLIR_EXACT_PS_NMADD: ppc_ps_madd_op(ctx, d, a, c, b, false, true); return;
    case DOLIR_EXACT_PS_NMSUB: ppc_ps_madd_op(ctx, d, a, c, b, true, true); return;
    case DOLIR_EXACT_PS_MADDS0: ppc_ps_madds0(ctx, d, a, c, b); return;
    case DOLIR_EXACT_PS_MADDS1: ppc_ps_madds1(ctx, d, a, c, b); return;
    case DOLIR_EXACT_PS_SUM0: ppc_ps_sum0(ctx, d, a, c, b); return;
    case DOLIR_EXACT_PS_SUM1: ppc_ps_sum1(ctx, d, a, c, b); return;
    case DOLIR_EXACT_PS_MULS0: ppc_ps_muls0(ctx, d, a, c); return;
    case DOLIR_EXACT_PS_MULS1: ppc_ps_muls1(ctx, d, a, c); return;
    case DOLIR_EXACT_PS_RES: ppc_ps_res_op(ctx, d, b); return;
    case DOLIR_EXACT_PS_RSQRTE: ppc_ps_rsqrte_op(ctx, d, b); return;
    case DOLIR_EXACT_PS_CMPU0:
        ppc_fcmp(ctx, crfd, ctx->fpr[a], ctx->fpr[b], false);
        return;
    case DOLIR_EXACT_PS_CMPO0:
        ppc_fcmp(ctx, crfd, ctx->fpr[a], ctx->fpr[b], true);
        return;
    case DOLIR_EXACT_PS_CMPU1:
        ppc_fcmp(ctx, crfd, ctx->ps1[a], ctx->ps1[b], false);
        return;
    case DOLIR_EXACT_PS_CMPO1:
        ppc_fcmp(ctx, crfd, ctx->ps1[a], ctx->ps1[b], true);
        return;
    default:
        return;
    }
}

// Resolve an indirect branch target inside the module. `region_index` is the
// region the branch was emitted in.
//
// Without a gate this is what the native backends route locally and nothing
// more: a target inside the same region, and only one a linked branch could
// return to. Everything else goes back to the chassis, because the chassis
// checks things on the way in -- that the region still matches guest RAM, that
// no mod has hooked the address -- which the interpreter cannot see.
//
// With a gate it can. The gate says, per region, whether the chassis would
// dispatch there right now, and that makes any entry in an open region as good
// a landing site as a dispatch: the entry stubs exist precisely so the chassis
// can resume at any instruction. On a title that calls and returns every thirty
// guest instructions, this is the difference between a dispatch per return and
// a dispatch per timing slice.
#ifdef DOLVM_PROFILE
u64 g_dolvm_miss_region;
u64 g_dolvm_miss_target;
u64 g_dolvm_miss_gap;
u64 g_dolvm_miss_closed;
#endif

static inline const DolVMEntryPoint* dolvm_resolve_indirect(
    const DolVMModule* module, const DolVMGate* gate, u32 region_index,
    u32 target) {
    if (target & 3u)
        return NULL;
    const DolVMRegion* region = NULL;
    if (region_index != DOLVM_NO_ENTRY) {
        // Most returns land in the region they came from, and that needs no
        // lookup at all.
        region = &module->regions[region_index];
        if (target < region->guest_start || target >= region->guest_end)
            region = NULL;
    }
    if (!region) {
        if (!gate) {
#ifdef DOLVM_PROFILE
            ++g_dolvm_miss_region;
#endif
            return NULL;
        }
        region = dolvm_module_region_inline(module, target);
        if (!region)
            return NULL;
        region_index = (u32)(region - module->regions);
    }
    if (gate && !gate->region_open[region_index]) {
#ifdef DOLVM_PROFILE
        ++g_dolvm_miss_closed;
#endif
        return NULL;
    }
    const DolVMEntryPoint* entry =
        &module->map[region->map_index + (target - region->guest_start) / 4u];
    if (entry->entry == DOLVM_NO_ENTRY) {
#ifdef DOLVM_PROFILE
        ++g_dolvm_miss_gap;
#endif
        return NULL;
    }
    if (!gate && !(entry->entry & DOLVM_ENTRY_RETURN_TARGET)) {
#ifdef DOLVM_PROFILE
        ++g_dolvm_miss_target;
#endif
        return NULL;
    }
    return entry;
}

// Whether the chassis is holding an exception the interpreter should hand
// control back for: a synchronous one always, an asynchronous one once the
// guest has interrupts enabled. Tested at every resolved edge and nowhere else,
// which is often enough -- an interrupt a guest store raised is delivered by
// the time the function that raised it returns -- and cheap enough, since the
// word is almost always zero.
static inline bool dolvm_pending(const DolVMGate* gate, const CPUState* ctx) {
    u32 pending = *gate->pending;
    if (!pending)
        return false;
    u32 mask = gate->pending_sync;
    if (ctx->msr & DOLVM_MSR_EE)
        mask |= gate->pending_async;
    return (pending & mask) != 0;
}

// Opt-in dynamic opcode histogram. Static counts over a module are a poor guide
// to where interpretation time goes -- entry stubs alone outnumber the code
// they lead into -- so speed work needs the executed mix, not the emitted one.
// Off by default and compiled out entirely; the dispatch below is unchanged.
// Where each opcode's handler begins. A sampling profiler reports addresses
// inside one enormous function, so this is what turns those back into opcodes.
// Kept outside DOLVM_PROFILE: the per-opcode counters below are two global
// increments on the hot path, which is enough to let LLVM's tail merger fold
// the handlers' epilogues back into one shared indirect branch -- so a build
// that counts opcodes is no longer the build whose time is being measured.
// This table costs one compare per dispatch and leaves the handlers alone.
#if defined(DOLVM_PROFILE) || defined(DOLVM_SAMPLE)
const void* g_dolvm_op_handlers[DOLVM_OP_COUNT];
#endif

#ifdef DOLVM_PROFILE
u64 g_dolvm_op_counts[DOLVM_OP_COUNT];
u64 g_dolvm_dispatches;
// Guest state traffic, by the slot it names, plus how many distinct general
// purpose registers one dispatch touches. Together they say how large a set of
// slots a global allocator would have to hold to be worth its prologue.
// How a dispatch ends, which is what decides whether its fixed costs -- the
// home fill and flush, the prologue, the entry lookup -- are amortized over
// thirty guest instructions or three hundred.
u64 g_dolvm_leave_indirect;
u64 g_dolvm_leave_resolved;
u64 g_dolvm_leave_guard;
u64 g_dolvm_leave_exit;
u64 g_dolvm_leave_call;
u64 g_dolvm_leave_called;
u64 g_dolvm_leave_idle;
u64 g_dolvm_leave_poll;
u64 g_dolvm_slot_counts[DOLVM_PROFILE_SLOTS];
u64 g_dolvm_gpr_span[33];
u32 g_dolvm_gpr_mask;
#define DOLVM_COUNT_SLOT(off)                                                 \
    do {                                                                      \
        u32 word__ = (u32)(off) / 4u;                                         \
        if (word__ < DOLVM_PROFILE_SLOTS)                                     \
            ++g_dolvm_slot_counts[word__];                                    \
        if ((u32)(off) < 128u)                                                \
            g_dolvm_gpr_mask |= 1u << ((u32)(off) / 4u);                      \
    } while (0)
// Where dispatches end, by guest pc: a loop that spins for whole slices shows
// up here as one address with most of the leaves.
#define DOLVM_PROFILE_LEAVE_SITES 4096u
static u32 g_dolvm_leave_pc[DOLVM_PROFILE_LEAVE_SITES];
static u64 g_dolvm_leave_pc_count[DOLVM_PROFILE_LEAVE_SITES];
static void dolvm_count_leave(u32 pc) {
    u32 slot = (pc >> 2) * 2654435761u >> 20;
    for (u32 probe = 0; probe < DOLVM_PROFILE_LEAVE_SITES; probe++) {
        u32 i = (slot + probe) & (DOLVM_PROFILE_LEAVE_SITES - 1u);
        if (!g_dolvm_leave_pc_count[i] || g_dolvm_leave_pc[i] == pc) {
            g_dolvm_leave_pc[i] = pc;
            g_dolvm_leave_pc_count[i]++;
            return;
        }
    }
}
// Which guest blocks the time is spent in. An opcode mix says what the
// interpreter runs; it does not say which of the game's functions asked for it,
// and the two questions have different answers -- one chain of four opcodes
// came to 40% of everything executed here and belonged to a single loop.
// Sampled at every block head, keyed by the block's guest pc.
#define DOLVM_PROFILE_BLOCK_SITES 65536u
static u32 g_dolvm_block_pc[DOLVM_PROFILE_BLOCK_SITES];
static u64 g_dolvm_block_count[DOLVM_PROFILE_BLOCK_SITES];
static u64 g_dolvm_block_total;
static u64 g_dolvm_block_dropped;
static void dolvm_count_block(u32 pc) {
    g_dolvm_block_total++;
    u32 slot = ((pc >> 2) * 2654435761u) & (DOLVM_PROFILE_BLOCK_SITES - 1u);
    for (u32 probe = 0; probe < DOLVM_PROFILE_BLOCK_SITES; probe++) {
        u32 i = (slot + probe) & (DOLVM_PROFILE_BLOCK_SITES - 1u);
        if (!g_dolvm_block_count[i] || g_dolvm_block_pc[i] == pc) {
            g_dolvm_block_pc[i] = pc;
            g_dolvm_block_count[i]++;
            return;
        }
    }
    g_dolvm_block_dropped++;
}
#define DOLVM_COUNT_BLOCK(pc) dolvm_count_block(pc)

// Dynamic opcode pairs: what follows what, which is what decides whether a
// fused form would pay.
u64 g_dolvm_pair_counts[DOLVM_OP_COUNT][DOLVM_OP_COUNT];
static u32 g_dolvm_previous_op;
#define DOLVM_COUNT(op)                                                       \
    do {                                                                      \
        ++g_dolvm_op_counts[(op)];                                            \
        ++g_dolvm_pair_counts[g_dolvm_previous_op][(op)];                     \
        g_dolvm_previous_op = (op);                                           \
    } while (0)
#else
#define DOLVM_COUNT(op) ((void)0)
#define DOLVM_COUNT_SLOT(off) ((void)0)
#define DOLVM_COUNT_BLOCK(pc) ((void)0)
#endif

// A helper that reaches into CPUState for something the register file is now
// holding needs it assembled first and read back after. Which helpers those are
// is decided one by one rather than by blanket conservatism: bracketing every
// call would put this on `fp.available`, which a float-heavy title executes tens
// of millions of times a second and which touches nothing homed.
#define DOLVM_HOMES_OUT()                                                     \
    do {                                                                      \
        dolvm_xer_out(ctx, regs);                                             \
        if (homed)                                                            \
            dolvm_homes_flush(ctx, regs);                                     \
    } while (0)
#define DOLVM_HOMES_IN()                                                      \
    do {                                                                      \
        dolvm_xer_in(ctx, regs);                                              \
        if (homed)                                                            \
            dolvm_homes_fill(ctx, regs);                                      \
    } while (0)

#ifdef DOLVM_THREADED
#define OP(name) L_##name
#define NEXT()                                                                \
    do {                                                                      \
        inst = ip++;                                                          \
        DOLVM_COUNT(inst->op);                                                \
        goto *dispatch_table[inst->op];                                       \
    } while (0)
#define LABEL(name) [name] = &&L_##name
#else
#define OP(name) case name
#define NEXT() goto dispatch
#endif

// One dispatch: fill the homed registers, run, write them back.
//
// The loop below has three dozen ways out and every one of them goes through
// `leave`, which is what makes the flush hard to forget on a path -- and a
// forgotten one loses guest register writes silently, which is the worst way
// for this to be wrong. It is a label rather than a wrapper function because a
// wrapper is a second stack frame per dispatch, and a dispatch is thirty guest
// instructions long.
int dolvm_dispatch(const DolVMModule* module, CPUState* ctx, u32 address) {
    const DolVMEntryPoint* entry = dolvm_module_entry_inline(module, address);
    if (!entry)
        return 0;
    // Aligned because filling and flushing the homes is a vector copy over a
    // contiguous run of it, and an unaligned base costs an address computation
    // per store.
    _Alignas(16) DolVMReg regs[DOLVM_MAX_REGISTERS];
    // Diagnostic mode: a local register is stack memory, so a bytecode stream
    // that reads one before writing it usually gets a plausible leftover and
    // behaves -- until the day it does not. Poisoning with an even pattern
    // makes such a read deterministic and wrong: a select on an unwritten
    // condition takes the false arm every time. This is how the missing
    // psq_lu success flag would have been caught by the differential test
    // instead of by a corrupted menu on a phone.
    if (g_dolvm_poison_registers)
        memset(regs, g_dolvm_poison_byte, sizeof(regs));
    const bool homed = (module->flags & DOLVM_FLAG_HOMED_STATE) != 0;
    dolvm_xer_in(ctx, regs);
    if (homed)
        dolvm_homes_fill(ctx, regs);
#ifdef DOLVM_PROFILE
    ++g_dolvm_dispatches;
    {
        u32 mask = g_dolvm_gpr_mask;
        u32 span = 0;
        while (mask) {
            span += mask & 1u;
            mask >>= 1;
        }
        ++g_dolvm_gpr_span[span];
        g_dolvm_gpr_mask = 0;
    }
#endif
    DolVMMemory mem;
    mem.ram = ctx->ram;
    mem.ram_size = ctx->ram_size;
    mem.exram = ctx->exram;
    mem.exram_size = ctx->exram_size;
    mem.journal = g_mem_write_journal;
    mem.journal_user = g_mem_write_journal_user;
    mem.homes = regs;
    mem.homed = homed;
    mem.poll_pc = 0;
    mem.poll_address = 0;
    mem.poll_value = 0;
    mem.poll_run = 0;
    mem.poll_fresh = 0;
    const DolVMInst* code = module->code;
    const DolVMInst* ip = code + (entry->entry & DOLVM_ENTRY_OFFSET_MASK);
    const DolVMInst* inst;
    // Guest pc of the instruction being executed, kept as a base plus a word
    // index so the hot memory ops can name their pc in a spare byte instead of
    // spending a whole dispatch on materializing it. The entry supplies the
    // base, so landing in the middle of a block still resolves to real pcs.
    u32 pc_base = entry->pc_base;
    u32 steps = 0;
    // How long this dispatch may run. Ungated, it is the fixed allowance the
    // native backends use. Gated, it is whatever is left of the chassis's
    // timing slice, and the dispatch runs to the end of that, returning only
    // when a resolved edge is closed or an exception is waiting. The slice
    // counter is read live at every guard: a hook the interpreter calls can
    // schedule an event and shorten it, and an event the guest is polling for
    // has to land when it was scheduled, not at the end of the slice.
    static const s32 fixed_budget = DOLVM_LOOP_CYCLE_BUDGET;
    const DolVMGate* gate = module->gate;
    const s32* budget = gate ? gate->budget : &fixed_budget;
    u32 step_budget = DOLVM_LOOP_STEP_BUDGET;
    if (gate && *budget > (s32)DOLVM_LOOP_STEP_BUDGET) {
        // A loop whose blocks the cycle table prices at zero is bounded by
        // steps instead, so that bound has to grow with the cycles.
        step_budget = (u32)*budget;
    }
#define DOLVM_OVER_BUDGET() (ctx->downcount <= -(s64)*budget)
// An idle loop's back edge: nothing the loop does can end the wait, so the
// rest of the slice is charged in one go and the chassis gets control back to
// run whatever event the guest is waiting on. Leaves at the loop head, so the
// next dispatch polls once more.
#define DOLVM_IDLE_LEAVE(leave_pc)                                            \
    do {                                                                      \
        s64 whole__ = -(s64)*budget;                                          \
        if (whole__ < ctx->downcount)                                         \
            ctx->downcount = whole__;                                         \
        ctx->pc = (leave_pc);                                                 \
        goto leave;                                                           \
    } while (0)
// A back edge taken while the chassis keeps handing back the same answer to the
// same hardware read. Nothing in the loop can change that answer, so this is
// the recognised idle loop's situation reached by a route the emitter cannot
// see -- through a call, past stores to scratch memory. Treated identically:
// charge the rest of the slice, leave at the loop head, poll once more next
// time. The run is cleared so a loop that spins across several slices pays the
// threshold again in each, which is what bounds how wrong this can be.
#define DOLVM_POLLING()                                                       \
    (g_dolvm_poll_skip_enabled && mem.poll_fresh                              \
         ? (mem.poll_fresh = 0,                                               \
            mem.poll_run >= DOLVM_POLL_SPIN ? (mem.poll_run = 0, 1) : 0)      \
         : 0)

// Landing on a block head through a resolved edge: the head opens with its
// cycle charge, and paying it here is one dispatch fewer per call and return.
#define DOLVM_LAND()                                                          \
    do {                                                                      \
        if (ip->op == DOLVM_OP_CHARGE) {                                      \
            ctx->downcount -= (s64)ip->imm;                                   \
            ip++;                                                             \
        }                                                                     \
    } while (0)

#ifdef DOLVM_THREADED
    static const void* const dispatch_table[DOLVM_OP_COUNT] = {
        LABEL(DOLVM_OP_NOP), LABEL(DOLVM_OP_MOV), LABEL(DOLVM_OP_CONST32),
        LABEL(DOLVM_OP_CONST64), LABEL(DOLVM_OP_LOAD_STATE8),
        LABEL(DOLVM_OP_LOAD_STATE32), LABEL(DOLVM_OP_LOAD_STATE64),
        LABEL(DOLVM_OP_LOAD_STATEF), LABEL(DOLVM_OP_STORE_STATE8),
        LABEL(DOLVM_OP_STORE_STATE32), LABEL(DOLVM_OP_STORE_STATE64),
        LABEL(DOLVM_OP_STORE_STATEF), LABEL(DOLVM_OP_ADD32),
        LABEL(DOLVM_OP_ADD32I), LABEL(DOLVM_OP_SUB32), LABEL(DOLVM_OP_MUL32),
        LABEL(DOLVM_OP_MUL32I), LABEL(DOLVM_OP_UDIV32), LABEL(DOLVM_OP_SDIV32),
        LABEL(DOLVM_OP_ADD64), LABEL(DOLVM_OP_SUB64), LABEL(DOLVM_OP_MUL64),
        LABEL(DOLVM_OP_UDIV64), LABEL(DOLVM_OP_SDIV64), LABEL(DOLVM_OP_AND),
        LABEL(DOLVM_OP_ANDI), LABEL(DOLVM_OP_OR), LABEL(DOLVM_OP_ORI),
        LABEL(DOLVM_OP_XOR), LABEL(DOLVM_OP_XORI), LABEL(DOLVM_OP_NOT),
        LABEL(DOLVM_OP_SHL32), LABEL(DOLVM_OP_SHL32I), LABEL(DOLVM_OP_LSHR32),
        LABEL(DOLVM_OP_LSHR32I), LABEL(DOLVM_OP_ASHR32),
        LABEL(DOLVM_OP_ASHR32I), LABEL(DOLVM_OP_SHL64), LABEL(DOLVM_OP_SHL64I),
        LABEL(DOLVM_OP_LSHR64), LABEL(DOLVM_OP_LSHR64I), LABEL(DOLVM_OP_ASHR64),
        LABEL(DOLVM_OP_ASHR64I), LABEL(DOLVM_OP_ROTL32),
        LABEL(DOLVM_OP_ROTL32I), LABEL(DOLVM_OP_CLZ32), LABEL(DOLVM_OP_CLZ64),
        LABEL(DOLVM_OP_BSWAP16), LABEL(DOLVM_OP_BSWAP32),
        LABEL(DOLVM_OP_BSWAP64), LABEL(DOLVM_OP_TRUNC), LABEL(DOLVM_OP_SEXT),
        LABEL(DOLVM_OP_ICMP_EQ), LABEL(DOLVM_OP_ICMP_EQI),
        LABEL(DOLVM_OP_ICMP_NE), LABEL(DOLVM_OP_ICMP_NEI),
        LABEL(DOLVM_OP_ICMP_ULT), LABEL(DOLVM_OP_ICMP_ULTI),
        LABEL(DOLVM_OP_ICMP_ULE), LABEL(DOLVM_OP_ICMP_ULEI),
        LABEL(DOLVM_OP_ICMP_SLT32), LABEL(DOLVM_OP_ICMP_SLT32I),
        LABEL(DOLVM_OP_ICMP_SLE32), LABEL(DOLVM_OP_ICMP_SLE32I),
        LABEL(DOLVM_OP_ICMP_SLT64), LABEL(DOLVM_OP_ICMP_SLE64),
        LABEL(DOLVM_OP_FCMP_OEQ), LABEL(DOLVM_OP_FCMP_OLT),
        LABEL(DOLVM_OP_FCMP_OGE), LABEL(DOLVM_OP_SELECT), LABEL(DOLVM_OP_FADD),
        LABEL(DOLVM_OP_FSUB), LABEL(DOLVM_OP_FMUL), LABEL(DOLVM_OP_FDIV),
        LABEL(DOLVM_OP_FNEG), LABEL(DOLVM_OP_FABS), LABEL(DOLVM_OP_FPTRUNC),
        LABEL(DOLVM_OP_FPEXT), LABEL(DOLVM_OP_LOAD8), LABEL(DOLVM_OP_LOAD16),
        LABEL(DOLVM_OP_LOAD32), LABEL(DOLVM_OP_LOAD64), LABEL(DOLVM_OP_LOAD8S),
        LABEL(DOLVM_OP_LOAD16S), LABEL(DOLVM_OP_LOAD32S),
        LABEL(DOLVM_OP_STORE8), LABEL(DOLVM_OP_STORE16),
        LABEL(DOLVM_OP_STORE32), LABEL(DOLVM_OP_STORE64),
        LABEL(DOLVM_OP_CHARGE), LABEL(DOLVM_OP_PC_BASE),
        LABEL(DOLVM_OP_LOOP_GUARD), LABEL(DOLVM_OP_JMP), LABEL(DOLVM_OP_JMP_IF),
        LABEL(DOLVM_OP_JMP_IFNOT), LABEL(DOLVM_OP_EXIT),
        LABEL(DOLVM_OP_EXIT_REG), LABEL(DOLVM_OP_INDIRECT), LABEL(DOLVM_OP_CALL),
        LABEL(DOLVM_OP_FALLBACK), LABEL(DOLVM_OP_SYSCALL), LABEL(DOLVM_OP_RFI),
        LABEL(DOLVM_OP_FP_AVAILABLE), LABEL(DOLVM_OP_EXACT_FLOAT),
        LABEL(DOLVM_OP_EXACT_PAIRED), LABEL(DOLVM_OP_PSQ_LOAD),
        LABEL(DOLVM_OP_PSQ_STORE), LABEL(DOLVM_OP_STWCX),
        LABEL(DOLVM_OP_FPSCR_UPDATED), LABEL(DOLVM_OP_FPSCR_BIT),
        LABEL(DOLVM_OP_PROGRAM_EXC), LABEL(DOLVM_OP_SPR_READ),
        LABEL(DOLVM_OP_SPR_WRITE), LABEL(DOLVM_OP_LSWX), LABEL(DOLVM_OP_DCBZ_L),
        LABEL(DOLVM_OP_ECIWX), LABEL(DOLVM_OP_ECOWX), LABEL(DOLVM_OP_TLBIE),
        LABEL(DOLVM_OP_CACHE_CONTROL), LABEL(DOLVM_OP_FENCE),
        LABEL(DOLVM_OP_SET_CR_FIELD), LABEL(DOLVM_OP_JMP_GUARD),
        LABEL(DOLVM_OP_JMP_IF_CR), LABEL(DOLVM_OP_CMP_STATE_I),
        LABEL(DOLVM_OP_CMP_STATE), LABEL(DOLVM_OP_LOAD_MEM_STATE),
        LABEL(DOLVM_OP_STORE_MEM_STATE), LABEL(DOLVM_OP_LOAD_MEM_TO_STATE),
        LABEL(DOLVM_OP_STORE_MEM_FROM_STATE), LABEL(DOLVM_OP_SET_CR_FIELDI),
        LABEL(DOLVM_OP_JMP_CHARGE), LABEL(DOLVM_OP_JMP_IF_CR_CHARGE),
        LABEL(DOLVM_OP_JMP_IF_CR_GUARD), LABEL(DOLVM_OP_SUPERVISOR),
        LABEL(DOLVM_OP_CMP_JMP_IF_CR),
        LABEL(DOLVM_OP_CMP_JMP_IF_CR_CHARGE),
        LABEL(DOLVM_OP_CMP_JMP_IF_CR_GUARD),
        LABEL(DOLVM_OP_ROTL32I_AND), LABEL(DOLVM_OP_SHL32I_AND),
        LABEL(DOLVM_OP_LSHR32I_AND), LABEL(DOLVM_OP_ASHR32I_AND),
    };
#if defined(DOLVM_PROFILE) || defined(DOLVM_SAMPLE)
    if (!g_dolvm_op_handlers[DOLVM_OP_NOP])
        memcpy(g_dolvm_op_handlers, dispatch_table, sizeof(dispatch_table));
#endif
    NEXT();
#else
dispatch:
    inst = ip++;
    DOLVM_COUNT(inst->op);
    switch (inst->op) {
#endif

    OP(DOLVM_OP_NOP):
        NEXT();

    OP(DOLVM_OP_MOV):
        regs[inst->a] = regs[inst->b];
        NEXT();

    OP(DOLVM_OP_CONST32):
        regs[inst->a].u = inst->imm;
        NEXT();

    OP(DOLVM_OP_CONST64):
        regs[inst->a].u = module->constants[inst->imm];
        NEXT();

    OP(DOLVM_OP_LOAD_STATE8):
        DOLVM_COUNT_SLOT(inst->imm);
        regs[inst->a].u = *(const u8*)((const u8*)ctx + inst->imm);
        NEXT();

    OP(DOLVM_OP_LOAD_STATE32):
        DOLVM_COUNT_SLOT(inst->imm);
        regs[inst->a].u = *(const u32*)(const void*)((const u8*)ctx + inst->imm);
        NEXT();

    OP(DOLVM_OP_LOAD_STATE64):
        DOLVM_COUNT_SLOT(inst->imm);
        regs[inst->a].u = *(const u64*)(const void*)((const u8*)ctx + inst->imm);
        NEXT();

    OP(DOLVM_OP_LOAD_STATEF):
        DOLVM_COUNT_SLOT(inst->imm);
        regs[inst->a].d = *(const f64*)(const void*)((const u8*)ctx + inst->imm);
        NEXT();

    OP(DOLVM_OP_STORE_STATE8):
        DOLVM_COUNT_SLOT(inst->imm);
        *(u8*)((u8*)ctx + inst->imm) = (u8)regs[inst->b].u;
        NEXT();

    OP(DOLVM_OP_STORE_STATE32):
        DOLVM_COUNT_SLOT(inst->imm);
        *(u32*)(void*)((u8*)ctx + inst->imm) = (u32)regs[inst->b].u;
        NEXT();

    OP(DOLVM_OP_STORE_STATE64):
        DOLVM_COUNT_SLOT(inst->imm);
        *(u64*)(void*)((u8*)ctx + inst->imm) = regs[inst->b].u;
        NEXT();

    OP(DOLVM_OP_STORE_STATEF):
        DOLVM_COUNT_SLOT(inst->imm);
        *(f64*)(void*)((u8*)ctx + inst->imm) = regs[inst->b].d;
        NEXT();

    OP(DOLVM_OP_ADD32):
        regs[inst->a].u = (u32)(regs[inst->b].u + regs[inst->c].u);
        NEXT();

    OP(DOLVM_OP_ADD32I):
        regs[inst->a].u = (u32)(regs[inst->b].u + inst->imm);
        NEXT();

    OP(DOLVM_OP_SUB32):
        regs[inst->a].u = (u32)(regs[inst->b].u - regs[inst->c].u);
        NEXT();

    OP(DOLVM_OP_MUL32):
        regs[inst->a].u = (u32)(regs[inst->b].u * regs[inst->c].u);
        NEXT();

    OP(DOLVM_OP_MUL32I):
        regs[inst->a].u = (u32)(regs[inst->b].u * inst->imm);
        NEXT();

    OP(DOLVM_OP_UDIV32):
        regs[inst->a].u = regs[inst->c].u
                              ? (u32)((u32)regs[inst->b].u / (u32)regs[inst->c].u)
                              : 0u;
        NEXT();

    OP(DOLVM_OP_SDIV32): {
        s32 divisor = (s32)(u32)regs[inst->c].u;
        s32 dividend = (s32)(u32)regs[inst->b].u;
        // The builder guards division by zero and the INT_MIN/-1 overflow with
        // a select ahead of this, so both arms are dead in practice; keeping
        // them defined here means a malformed module cannot fault the host.
        regs[inst->a].u =
            (divisor == 0 || (divisor == -1 && dividend == (s32)0x80000000))
                ? 0u
                : (u32)(dividend / divisor);
        NEXT();
    }

    OP(DOLVM_OP_ADD64):
        regs[inst->a].u = regs[inst->b].u + regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_SUB64):
        regs[inst->a].u = regs[inst->b].u - regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_MUL64):
        regs[inst->a].u = regs[inst->b].u * regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_UDIV64):
        regs[inst->a].u =
            regs[inst->c].u ? regs[inst->b].u / regs[inst->c].u : 0ull;
        NEXT();

    OP(DOLVM_OP_SDIV64): {
        s64 divisor = (s64)regs[inst->c].u;
        s64 dividend = (s64)regs[inst->b].u;
        regs[inst->a].u =
            (divisor == 0 ||
             (divisor == -1 && dividend == (s64)0x8000000000000000ull))
                ? 0ull
                : (u64)(dividend / divisor);
        NEXT();
    }

    OP(DOLVM_OP_AND):
        regs[inst->a].u = regs[inst->b].u & regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ANDI):
        regs[inst->a].u = regs[inst->b].u & inst->imm;
        NEXT();

    OP(DOLVM_OP_OR):
        regs[inst->a].u = regs[inst->b].u | regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ORI):
        regs[inst->a].u = regs[inst->b].u | inst->imm;
        NEXT();

    OP(DOLVM_OP_XOR):
        regs[inst->a].u = regs[inst->b].u ^ regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_XORI):
        regs[inst->a].u = regs[inst->b].u ^ inst->imm;
        NEXT();

    OP(DOLVM_OP_NOT):
        regs[inst->a].u = ~regs[inst->b].u & dolvm_width_mask(inst->imm);
        NEXT();

    OP(DOLVM_OP_SHL32):
        regs[inst->a].u = (u32)(regs[inst->b].u << (regs[inst->c].u & 31u));
        NEXT();

    OP(DOLVM_OP_SHL32I):
        regs[inst->a].u = (u32)(regs[inst->b].u << (inst->imm & 31u));
        NEXT();

    OP(DOLVM_OP_LSHR32):
        regs[inst->a].u = (u32)regs[inst->b].u >> (regs[inst->c].u & 31u);
        NEXT();

    OP(DOLVM_OP_LSHR32I):
        regs[inst->a].u = (u32)regs[inst->b].u >> (inst->imm & 31u);
        NEXT();

    OP(DOLVM_OP_ASHR32):
        regs[inst->a].u =
            (u32)((s32)(u32)regs[inst->b].u >> (regs[inst->c].u & 31u));
        NEXT();

    OP(DOLVM_OP_ASHR32I):
        regs[inst->a].u = (u32)((s32)(u32)regs[inst->b].u >> (inst->imm & 31u));
        NEXT();

    OP(DOLVM_OP_SHL64):
        regs[inst->a].u = regs[inst->b].u << (regs[inst->c].u & 63u);
        NEXT();

    OP(DOLVM_OP_SHL64I):
        regs[inst->a].u = regs[inst->b].u << (inst->imm & 63u);
        NEXT();

    OP(DOLVM_OP_LSHR64):
        regs[inst->a].u = regs[inst->b].u >> (regs[inst->c].u & 63u);
        NEXT();

    OP(DOLVM_OP_LSHR64I):
        regs[inst->a].u = regs[inst->b].u >> (inst->imm & 63u);
        NEXT();

    OP(DOLVM_OP_ASHR64):
        regs[inst->a].u = (u64)((s64)regs[inst->b].u >> (regs[inst->c].u & 63u));
        NEXT();

    OP(DOLVM_OP_ASHR64I):
        regs[inst->a].u = (u64)((s64)regs[inst->b].u >> (inst->imm & 63u));
        NEXT();

    OP(DOLVM_OP_ROTL32): {
        u32 value = (u32)regs[inst->b].u;
        u32 shift = (u32)regs[inst->c].u & 31u;
        regs[inst->a].u = shift ? ((value << shift) | (value >> (32u - shift)))
                                : value;
        NEXT();
    }

    // `rlwinm` and the rest of the field extractions, as one opcode. The mask
    // is applied to a value that is already zero-extended from 32 bits, which
    // is what the rotate or shift that used to precede this left behind.
    OP(DOLVM_OP_ROTL32I_AND): {
        u32 value = (u32)regs[inst->b].u;
        u32 shift = inst->c & 31u;
        u32 rotated = shift ? ((value << shift) | (value >> (32u - shift)))
                            : value;
        regs[inst->a].u = rotated & inst->imm;
        NEXT();
    }

    OP(DOLVM_OP_SHL32I_AND):
        regs[inst->a].u =
            (u32)(regs[inst->b].u << (inst->c & 31u)) & inst->imm;
        NEXT();

    OP(DOLVM_OP_LSHR32I_AND):
        regs[inst->a].u = ((u32)regs[inst->b].u >> (inst->c & 31u)) & inst->imm;
        NEXT();

    OP(DOLVM_OP_ASHR32I_AND):
        regs[inst->a].u =
            (u32)((s32)(u32)regs[inst->b].u >> (inst->c & 31u)) & inst->imm;
        NEXT();

    OP(DOLVM_OP_ROTL32I): {
        u32 value = (u32)regs[inst->b].u;
        u32 shift = inst->imm & 31u;
        regs[inst->a].u = shift ? ((value << shift) | (value >> (32u - shift)))
                                : value;
        NEXT();
    }

    OP(DOLVM_OP_CLZ32):
        regs[inst->a].u = dolvm_clz32((u32)regs[inst->b].u);
        NEXT();

    OP(DOLVM_OP_CLZ64):
        regs[inst->a].u = dolvm_clz64(regs[inst->b].u);
        NEXT();

    OP(DOLVM_OP_BSWAP16):
        regs[inst->a].u = bswap16((u16)regs[inst->b].u);
        NEXT();

    OP(DOLVM_OP_BSWAP32):
        regs[inst->a].u = bswap32((u32)regs[inst->b].u);
        NEXT();

    OP(DOLVM_OP_BSWAP64): {
        u64 value = regs[inst->b].u;
        regs[inst->a].u = ((u64)bswap32((u32)value) << 32) |
                          bswap32((u32)(value >> 32));
        NEXT();
    }

    OP(DOLVM_OP_TRUNC):
        regs[inst->a].u = regs[inst->b].u & dolvm_width_mask(inst->imm);
        NEXT();

    OP(DOLVM_OP_SEXT): {
        u32 source_bits = dolvm_width_bits(inst->imm & 0xFFu);
        u64 sign = 1ull << (source_bits - 1u);
        u64 value = regs[inst->b].u & dolvm_width_mask(inst->imm & 0xFFu);
        regs[inst->a].u =
            ((value ^ sign) - sign) & dolvm_width_mask((inst->imm >> 8) & 0xFFu);
        NEXT();
    }

    OP(DOLVM_OP_ICMP_EQ):
        regs[inst->a].u = regs[inst->b].u == regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ICMP_EQI):
        regs[inst->a].u = regs[inst->b].u == inst->imm;
        NEXT();

    OP(DOLVM_OP_ICMP_NE):
        regs[inst->a].u = regs[inst->b].u != regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ICMP_NEI):
        regs[inst->a].u = regs[inst->b].u != inst->imm;
        NEXT();

    OP(DOLVM_OP_ICMP_ULT):
        regs[inst->a].u = regs[inst->b].u < regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ICMP_ULTI):
        regs[inst->a].u = regs[inst->b].u < inst->imm;
        NEXT();

    OP(DOLVM_OP_ICMP_ULE):
        regs[inst->a].u = regs[inst->b].u <= regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ICMP_ULEI):
        regs[inst->a].u = regs[inst->b].u <= inst->imm;
        NEXT();

    OP(DOLVM_OP_ICMP_SLT32):
        regs[inst->a].u = (s32)(u32)regs[inst->b].u < (s32)(u32)regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ICMP_SLT32I):
        regs[inst->a].u = (s32)(u32)regs[inst->b].u < (s32)inst->imm;
        NEXT();

    OP(DOLVM_OP_ICMP_SLE32):
        regs[inst->a].u = (s32)(u32)regs[inst->b].u <= (s32)(u32)regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ICMP_SLE32I):
        regs[inst->a].u = (s32)(u32)regs[inst->b].u <= (s32)inst->imm;
        NEXT();

    OP(DOLVM_OP_ICMP_SLT64):
        regs[inst->a].u = (s64)regs[inst->b].u < (s64)regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_ICMP_SLE64):
        regs[inst->a].u = (s64)regs[inst->b].u <= (s64)regs[inst->c].u;
        NEXT();

    OP(DOLVM_OP_FCMP_OEQ):
        regs[inst->a].u = regs[inst->b].d == regs[inst->c].d;
        NEXT();

    OP(DOLVM_OP_FCMP_OLT):
        regs[inst->a].u = regs[inst->b].d < regs[inst->c].d;
        NEXT();

    OP(DOLVM_OP_FCMP_OGE):
        regs[inst->a].u = regs[inst->b].d >= regs[inst->c].d;
        NEXT();

    OP(DOLVM_OP_SELECT):
        regs[inst->a] = regs[inst->b].u ? regs[inst->c]
                                        : regs[inst->imm & 0xFFu];
        NEXT();

    OP(DOLVM_OP_FADD):
        regs[inst->a].d = regs[inst->b].d + regs[inst->c].d;
        NEXT();

    OP(DOLVM_OP_FSUB):
        regs[inst->a].d = regs[inst->b].d - regs[inst->c].d;
        NEXT();

    OP(DOLVM_OP_FMUL):
        regs[inst->a].d = regs[inst->b].d * regs[inst->c].d;
        NEXT();

    OP(DOLVM_OP_FDIV):
        regs[inst->a].d = regs[inst->b].d / regs[inst->c].d;
        NEXT();

    OP(DOLVM_OP_FNEG):
        regs[inst->a].d = -regs[inst->b].d;
        NEXT();

    OP(DOLVM_OP_FABS):
        regs[inst->a].d = fabs(regs[inst->b].d);
        NEXT();

    OP(DOLVM_OP_FPTRUNC):
        regs[inst->a].u = dolvm_double_to_single(regs[inst->b].d);
        NEXT();

    OP(DOLVM_OP_FPEXT):
        regs[inst->a].d = dolvm_single_to_double((u32)regs[inst->b].u);
        NEXT();

#define DOLVM_LOAD(width, transform)                                          \
    do {                                                                      \
        u64 value;                                                            \
        if (!dolvm_guest_load(ctx, &mem, (u32)(regs[inst->b].u + inst->imm), width, \
                              pc_base + inst->c * 4u, &value))                \
            goto leave;                                                       \
        regs[inst->a].u = transform;                                          \
        NEXT();                                                               \
    } while (0)

    OP(DOLVM_OP_LOAD8): DOLVM_LOAD(1u, value);
    OP(DOLVM_OP_LOAD16): DOLVM_LOAD(2u, value);
    OP(DOLVM_OP_LOAD32): DOLVM_LOAD(4u, value);
    OP(DOLVM_OP_LOAD64): DOLVM_LOAD(8u, value);
    OP(DOLVM_OP_LOAD8S): DOLVM_LOAD(1u, (u32)(s32)(s8)(u8)value);
    OP(DOLVM_OP_LOAD16S): DOLVM_LOAD(2u, (u32)(s32)(s16)(u16)value);
    OP(DOLVM_OP_LOAD32S): DOLVM_LOAD(4u, (u64)(s64)(s32)(u32)value);

#undef DOLVM_LOAD

#define DOLVM_STORE(width)                                                    \
    do {                                                                      \
        if (!dolvm_guest_store(ctx, &mem, (u32)(regs[inst->b].u + inst->imm), \
                               regs[inst->c].u, width,                        \
                               pc_base + inst->a * 4u))                       \
            goto leave;                                                       \
        NEXT();                                                               \
    } while (0)

    OP(DOLVM_OP_STORE8): DOLVM_STORE(1u);
    OP(DOLVM_OP_STORE16): DOLVM_STORE(2u);
    OP(DOLVM_OP_STORE32): DOLVM_STORE(4u);
    OP(DOLVM_OP_STORE64): DOLVM_STORE(8u);

#undef DOLVM_STORE

    OP(DOLVM_OP_CHARGE):
        DOLVM_COUNT_BLOCK(pc_base);
        ctx->downcount -= (s64)inst->imm;
        NEXT();

    OP(DOLVM_OP_PC_BASE):
        pc_base = inst->imm;
        NEXT();

    OP(DOLVM_OP_LOOP_GUARD):
        // Two ceilings, exactly the pair the native backends use: cycles bound
        // an ordinary loop, and the step count bounds a loop whose body the
        // cycle table happens to price at zero.
        if (DOLVM_POLLING()) {
#ifdef DOLVM_PROFILE
            ++g_dolvm_leave_poll;
#endif
            DOLVM_IDLE_LEAVE(inst->imm);
        }
        if (DOLVM_OVER_BUDGET() || ++steps >= step_budget) {
            ctx->pc = inst->imm;
            goto leave;
        }
        NEXT();

    OP(DOLVM_OP_JMP):
        ip = code + inst->imm;
        NEXT();

    OP(DOLVM_OP_JMP_IF):
        if (regs[inst->b].u)
            ip = code + inst->imm;
        NEXT();

    OP(DOLVM_OP_JMP_IFNOT):
        if (!regs[inst->b].u)
            ip = code + inst->imm;
        NEXT();

    OP(DOLVM_OP_EXIT):
        ctx->pc = inst->imm;
#ifdef DOLVM_PROFILE
        ++g_dolvm_leave_exit;
#endif
        goto leave;

    OP(DOLVM_OP_EXIT_REG):
        ctx->pc = (u32)regs[inst->b].u;
        goto leave;

    OP(DOLVM_OP_INDIRECT): {
        u32 target = (u32)regs[inst->b].u & (inst->a ? ~3u : ~0u);
        const DolVMEntryPoint* landing =
            dolvm_resolve_indirect(module, gate, inst->imm, target);
        // A resolvable target still goes back to the chassis once the budget
        // is spent, so a mutually recursive pair cannot hold the dispatch --
        // and as soon as the chassis has an exception to deliver.
        if (!landing || DOLVM_OVER_BUDGET() || ++steps >= step_budget ||
            (gate && dolvm_pending(gate, ctx))) {
            ctx->pc = target;
#ifdef DOLVM_PROFILE
            ++g_dolvm_leave_indirect;
#endif
            goto leave;
        }
#ifdef DOLVM_PROFILE
        ++g_dolvm_leave_resolved;
#endif
        ip = code + (landing->entry & DOLVM_ENTRY_OFFSET_MASK);
        pc_base = landing->pc_base;
        NEXT();
    }

    OP(DOLVM_OP_CALL): {
        // payload: target guest pc in the high half, target pc base in the low.
        // Followed only where the gate says the chassis itself would go;
        // otherwise this is the EXIT it stands in for.
        u64 payload = dolvm_payload(ip);
        if (!gate || !gate->region_open[(u32)inst->a | (u32)inst->b << 8] ||
            DOLVM_OVER_BUDGET() || ++steps >= step_budget ||
            dolvm_pending(gate, ctx)) {
            ctx->pc = (u32)(payload >> 32);
#ifdef DOLVM_PROFILE
            ++g_dolvm_leave_call;
#endif
            goto leave;
        }
#ifdef DOLVM_PROFILE
        ++g_dolvm_leave_called;
#endif
        ip = code + inst->imm;
        pc_base = (u32)payload;
        NEXT();
    }

    OP(DOLVM_OP_FALLBACK): {
        u32 raw = (u32)dolvm_payload(ip);
        u32 pc = inst->imm;
        ip++;
        ctx->pc = pc;
        // The fallback executes a whole guest instruction of its own, so it
        // reaches every register the file is holding.
        DOLVM_HOMES_OUT();
        ppc_fallback_instruction(ctx, raw, pc);
        DOLVM_HOMES_IN();
        // Resume inline only if the fallback did what a plain instruction
        // would have: advanced one instruction and raised nothing.
        if (ctx->pc != pc + 4u || ctx->exception != 0)
            goto leave;
        NEXT();
    }

    OP(DOLVM_OP_SYSCALL):
        ctx->pc = inst->imm;
        ppc_system_call_exception(ctx, inst->imm);
        goto leave;

    OP(DOLVM_OP_RFI):
        ctx->pc = inst->imm;
        ppc_rfi(ctx, inst->imm);
        goto leave;

    OP(DOLVM_OP_FP_AVAILABLE):
        if (!(ctx->msr & DOLVM_MSR_FP)) {
            ctx->pc = inst->imm;
            if (!ppc_fp_available(ctx, inst->imm))
                goto leave;
        }
        NEXT();

    OP(DOLVM_OP_EXACT_FLOAT):
        dolvm_exact_float(ctx, dolvm_payload(ip));
        ip++;
        NEXT();

    OP(DOLVM_OP_EXACT_PAIRED):
        dolvm_exact_paired(ctx, dolvm_payload(ip));
        ip++;
        NEXT();

    OP(DOLVM_OP_PSQ_LOAD):
    OP(DOLVM_OP_PSQ_STORE): {
        u64 descriptor = dolvm_payload(ip);
        bool load = inst->op == DOLVM_OP_PSQ_LOAD;
        ip++;
        u8 reg = (u8)(descriptor & 0xFFu);
        bool w = ((descriptor >> 8) & 1u) != 0;
        u8 gqr = (u8)((descriptor >> 9) & 7u);
        bool indexed = ((descriptor >> 12) & 1u) != 0;
        u32 ea = (u32)regs[inst->b].u;
        ctx->pc = inst->imm;
        // The helper touches nothing the register file is holding -- the
        // quantization registers, the floating point file, and memory -- so the
        // homed registers only have to be put back when the access can leave
        // RAM and reach the chassis. A pair is at most eight bytes, and almost
        // every one lands in MEM1, where the bracket was most of the cost.
        bool ok;
        if (mem.ram_size >= 8u &&
            ((ea & ~0x40000000u) - GC_RAM_BASE) <= mem.ram_size - 8u) {
            ok = load ? ppc_psq_load_inline(ctx, reg, ea, w, gqr, indexed,
                                            inst->imm)
                      : ppc_psq_store_inline(ctx, reg, ea, w, gqr, indexed,
                                             inst->imm);
        } else {
            DOLVM_HOMES_OUT();
            ok = load ? ppc_psq_load_inline(ctx, reg, ea, w, gqr, indexed,
                                            inst->imm)
                      : ppc_psq_store_inline(ctx, reg, ea, w, gqr, indexed,
                                             inst->imm);
            DOLVM_HOMES_IN();
        }
        if (!ok)
            goto leave;
        // Produce the success flag the IR asked for. psq_lu and psq_stu write
        // the effective address back to rA only `if` the access succeeded, and
        // the builder expresses that as a select on this value -- so leaving it
        // unwritten makes the write-back depend on whatever the register last
        // held, and a pointer walk that sometimes does not advance turns a
        // matrix into rubbish. Control only reaches here on success, so the
        // answer is 1; the point is that it has to be *written*. 0xFF is the
        // emitter's "no register", used when nothing consumes the flag.
        if (inst->a != 0xFFu)
            regs[inst->a].u = 1u;
        NEXT();
    }

    OP(DOLVM_OP_STWCX):
        ctx->pc = inst->imm;
        // Names its source by guest register number rather than taking a value.
        DOLVM_HOMES_OUT();
        ppc_stwcx_op(ctx, inst->c, (u32)regs[inst->b].u, inst->imm);
        DOLVM_HOMES_IN();
        if (ctx->exception)
            goto leave;
        NEXT();

    OP(DOLVM_OP_FPSCR_UPDATED):
        ppc_fpscr_control_updated(ctx);
        NEXT();

    OP(DOLVM_OP_FPSCR_BIT):
        if ((inst->imm >> 8) & 1u)
            ppc_mtfsb1_op(ctx, (u8)(inst->imm & 0xFFu));
        else
            ppc_mtfsb0_op(ctx, (u8)(inst->imm & 0xFFu));
        NEXT();

    // The compare half of the three fused forms below: the field is written as
    // it always was, and the branch's bit falls out of the same four bits
    // rather than being read back out of `ctx->cr`.
#define DOLVM_CMP_BRANCH(payload)                                             \
    u32 left = (u32)regs[inst->a].u;                                          \
    u32 right = (u32)(payload);                                               \
    u32 bits = dolvm_cr_bits(left, right, (inst->b & 0x08u) != 0) |           \
               dolvm_summary_overflow(regs);                                  \
    u32 shift = 4u * (7u - (u32)(inst->b & 7u));                              \
    ctx->cr = (ctx->cr & ~(0xFu << shift)) | (bits << shift);                 \
    bool taken = ((bits >> (3u - ((u32)inst->b >> 4 & 3u))) & 1u) ==          \
                 ((u32)inst->b >> 6 & 1u)

    OP(DOLVM_OP_CMP_JMP_IF_CR): {
        u64 payload = dolvm_payload(ip);
        ip++;
        DOLVM_CMP_BRANCH(payload);
        if (taken)
            ip = code + inst->imm;
        NEXT();
    }

    OP(DOLVM_OP_CMP_JMP_IF_CR_CHARGE): {
        u64 payload = dolvm_payload(ip);
        ip++;
        DOLVM_CMP_BRANCH(payload);
        if (taken) {
            ctx->downcount -= (s64)(u32)(payload >> 32);
            ip = code + inst->imm;
        }
        NEXT();
    }

    OP(DOLVM_OP_CMP_JMP_IF_CR_GUARD): {
        u64 payload = dolvm_payload(ip);
        const DolVMInst* leave_word = ip + 1;
        ip += 2;
        DOLVM_CMP_BRANCH(payload);
        if (!taken)
            NEXT();
        if ((inst->b & 0x80u) || DOLVM_POLLING()) {
#ifdef DOLVM_PROFILE
            ++g_dolvm_leave_idle;
#endif
            DOLVM_IDLE_LEAVE((u32)dolvm_payload(leave_word));
        }
        if (DOLVM_OVER_BUDGET() || ++steps >= step_budget) {
            ctx->pc = (u32)dolvm_payload(leave_word);
#ifdef DOLVM_PROFILE
            ++g_dolvm_leave_guard;
#endif
            goto leave;
        }
        ctx->downcount -= (s64)(u32)(payload >> 32);
        ip = code + inst->imm;
        NEXT();
    }

#undef DOLVM_CMP_BRANCH

    OP(DOLVM_OP_SUPERVISOR):
        // A privileged instruction, and the whole test that it is allowed to
        // run. A running game is in supervisor mode, so this falls through
        // essentially always -- which is the point of it being one dispatch
        // rather than four.
        if (ctx->msr & DOLVM_MSR_PR) {
            ctx->pc = inst->imm;
            ppc_program_exception(ctx, DOLIR_PROGRAM_PRIV, inst->imm);
            goto leave;
        }
        NEXT();

    OP(DOLVM_OP_PROGRAM_EXC): {
        u32 cause = (u32)dolvm_payload(ip);
        ip++;
        if (regs[inst->b].u) {
            ctx->pc = inst->imm;
            ppc_program_exception(ctx, cause, inst->imm);
            goto leave;
        }
        NEXT();
    }

    OP(DOLVM_OP_SPR_READ): {
        u16 spr = (u16)dolvm_payload(ip);
        ip++;
        ctx->pc = inst->imm;
        // mfspr reads LR and CTR among the rest, and the file is holding them.
        DOLVM_HOMES_OUT();
        u64 value = ppc_mfspr(ctx, spr, inst->imm);
        DOLVM_HOMES_IN();
        regs[inst->a].u = value;
        if (ctx->exception)
            goto leave;
        NEXT();
    }

    OP(DOLVM_OP_SPR_WRITE): {
        u16 spr = (u16)dolvm_payload(ip);
        ip++;
        ctx->pc = inst->imm;
        DOLVM_HOMES_OUT();
        ppc_mtspr(ctx, spr, (u32)regs[inst->b].u, inst->imm);
        DOLVM_HOMES_IN();
        if (ctx->exception)
            goto leave;
        NEXT();
    }

    OP(DOLVM_OP_LSWX): {
        u32 descriptor = (u32)dolvm_payload(ip);
        ip++;
        ctx->pc = inst->imm;
        // Names a run of guest registers to fill, by number.
        DOLVM_HOMES_OUT();
        ppc_lswx(ctx, (u8)(descriptor & 0xFFu), (u8)((descriptor >> 8) & 0xFFu),
                 (u8)((descriptor >> 16) & 0xFFu), inst->imm);
        DOLVM_HOMES_IN();
        if (ctx->exception)
            goto leave;
        NEXT();
    }

    OP(DOLVM_OP_DCBZ_L):
        ctx->pc = inst->imm;
        DOLVM_HOMES_OUT();
        ppc_dcbz_l(ctx, (u32)regs[inst->b].u, inst->imm);
        DOLVM_HOMES_IN();
        if (ctx->exception)
            goto leave;
        NEXT();

    OP(DOLVM_OP_ECIWX): {
        ctx->pc = inst->imm;
        DOLVM_HOMES_OUT();
        u64 external = ppc_eciwx(ctx, (u32)regs[inst->b].u, inst->imm);
        DOLVM_HOMES_IN();
        regs[inst->a].u = external;
        if (ctx->exception)
            goto leave;
        NEXT();
    }

    OP(DOLVM_OP_ECOWX):
        ctx->pc = inst->imm;
        DOLVM_HOMES_OUT();
        ppc_ecowx(ctx, (u32)regs[inst->b].u, (u32)regs[inst->c].u, inst->imm);
        DOLVM_HOMES_IN();
        if (ctx->exception)
            goto leave;
        NEXT();

    OP(DOLVM_OP_TLBIE):
        ctx->pc = inst->imm;
        DOLVM_HOMES_OUT();
        ppc_tlbie(ctx, (u32)regs[inst->b].u, inst->imm);
        DOLVM_HOMES_IN();
        if (ctx->exception)
            goto leave;
        NEXT();

    OP(DOLVM_OP_CACHE_CONTROL): {
        u32 operation = (u32)dolvm_payload(ip);
        ip++;
        ctx->pc = inst->imm;
        DOLVM_HOMES_OUT();
        ppc_cache_control(ctx, (u8)operation, (u32)regs[inst->b].u, inst->imm);
        DOLVM_HOMES_IN();
        if (ctx->exception)
            goto leave;
        NEXT();
    }

    OP(DOLVM_OP_FENCE):
        ppc_memory_fence();
        NEXT();

    OP(DOLVM_OP_SET_CR_FIELD): {
        // Exactly the builder's expansion: less-than, greater-than and equal
        // are mutually exclusive, and XER's summary-overflow bit rides along
        // in bit 0 of the field.
        u32 shift = 4u * (7u - (inst->imm & 0xFFu));
        u32 bits = dolvm_cr_bits((u32)regs[inst->b].u, (u32)regs[inst->c].u,
                                 ((inst->imm >> 8) & 1u) != 0) |
                   dolvm_summary_overflow(regs);
        ctx->cr = (ctx->cr & ~(0xFu << shift)) | (bits << shift);
        NEXT();
    }

// The width of a fused memory operation is a field, not a constant, and letting
// it stay one costs more than the dispatch the fusion saved: dolvm_guest_load
// inlines to a handful of instructions when the width is known and to a switch
// over all of them when it is not. Naming each width in its own arm hands the
// compiler back what the plain opcodes never gave up.
#define DOLVM_FUSED_LOAD(store_result)                                        \
    do {                                                                      \
        u32 address = (u32)(base + inst->imm);                                \
        u32 pc = pc_base + inst->a * 4u;                                      \
        u64 value;                                                            \
        u32 stored;                                                           \
        switch (inst->b) {                                                    \
        case DOLVM_OP_LOAD8:                                                  \
            if (!dolvm_guest_load(ctx, &mem, address, 1u, pc, &value))        \
                goto leave;                                                   \
            stored = (u32)value;                                              \
            break;                                                            \
        case DOLVM_OP_LOAD8S:                                                 \
            if (!dolvm_guest_load(ctx, &mem, address, 1u, pc, &value))        \
                goto leave;                                                   \
            stored = (u32)(s32)(s8)(u8)value;                                 \
            break;                                                            \
        case DOLVM_OP_LOAD16:                                                 \
            if (!dolvm_guest_load(ctx, &mem, address, 2u, pc, &value))        \
                goto leave;                                                   \
            stored = (u32)value;                                              \
            break;                                                            \
        case DOLVM_OP_LOAD16S:                                                \
            if (!dolvm_guest_load(ctx, &mem, address, 2u, pc, &value))        \
                goto leave;                                                   \
            stored = (u32)(s32)(s16)(u16)value;                               \
            break;                                                            \
        default:                                                              \
            if (!dolvm_guest_load(ctx, &mem, address, 4u, pc, &value))        \
                goto leave;                                                   \
            stored = (u32)value;                                              \
            break;                                                            \
        }                                                                     \
        store_result;                                                         \
        NEXT();                                                               \
    } while (0)

#define DOLVM_FUSED_STORE()                                                   \
    do {                                                                      \
        u32 address = (u32)(base + inst->imm);                                \
        u32 pc = pc_base + inst->a * 4u;                                      \
        bool ok;                                                              \
        switch (inst->b) {                                                    \
        case DOLVM_OP_STORE8:                                                 \
            ok = dolvm_guest_store(ctx, &mem, address, value, 1u, pc);        \
            break;                                                            \
        case DOLVM_OP_STORE16:                                                \
            ok = dolvm_guest_store(ctx, &mem, address, value, 2u, pc);        \
            break;                                                            \
        default:                                                              \
            ok = dolvm_guest_store(ctx, &mem, address, value, 4u, pc);        \
            break;                                                            \
        }                                                                     \
        if (!ok)                                                              \
            goto leave;                                                       \
        NEXT();                                                               \
    } while (0)

    OP(DOLVM_OP_LOAD_MEM_STATE): {
        // `lwz rD,off(rA)` end to end: the base out of CPUState, the guest
        // load, and the result back into CPUState and its register.
        u64 payload = dolvm_payload(ip);
        ip++;
        u32 base = *(const u32*)((const u8*)ctx + (u32)payload);
        u32 destination = (u32)(payload >> 32);
        u8 reg = inst->c;
        DOLVM_FUSED_LOAD((*(u32*)((u8*)ctx + destination) = stored,
                          regs[reg].u = stored));
    }

    OP(DOLVM_OP_STORE_MEM_STATE): {
        u64 payload = dolvm_payload(ip);
        ip++;
        u32 base = *(const u32*)((const u8*)ctx + (u32)payload);
        u32 value = *(const u32*)((const u8*)ctx + (u32)(payload >> 32));
        DOLVM_FUSED_STORE();
    }

    OP(DOLVM_OP_LOAD_MEM_TO_STATE): {
        u64 payload = dolvm_payload(ip);
        ip++;
        u32 base = (u32)regs[inst->c].u;
        u32 destination = (u32)(payload >> 32);
        u8 reg = (u8)payload;
        DOLVM_FUSED_LOAD((*(u32*)((u8*)ctx + destination) = stored,
                          regs[reg].u = stored));
    }

    OP(DOLVM_OP_STORE_MEM_FROM_STATE): {
        u64 payload = dolvm_payload(ip);
        ip++;
        u32 base = (u32)regs[inst->c].u;
        u32 value = *(const u32*)((const u8*)ctx + (u32)payload);
        DOLVM_FUSED_STORE();
    }

    OP(DOLVM_OP_SET_CR_FIELDI): {
        u64 payload = dolvm_payload(ip);
        ip++;
        u32 left = (u32)regs[inst->b].u;
        u32 right = (u32)payload;
        u32 bits = dolvm_cr_bits(left, right, ((inst->imm >> 8) & 1u) != 0) |
                   dolvm_summary_overflow(regs);
        u32 shift = 4u * (7u - (inst->imm & 0xFFu));
        ctx->cr = (ctx->cr & ~(0xFu << shift)) | (bits << shift);
        NEXT();
    }

    OP(DOLVM_OP_JMP_CHARGE):
        ctx->downcount -= (s64)(u32)dolvm_payload(ip);
        ip = code + inst->imm;
        NEXT();

    OP(DOLVM_OP_JMP_IF_CR_CHARGE):
        if ((((ctx->cr << inst->a) >> 31) & 1u) == inst->b) {
            ctx->downcount -= (s64)(u32)dolvm_payload(ip);
            ip = code + inst->imm;
        } else {
            // Not taken: step over the payload the taken path would have read.
            ip++;
        }
        NEXT();

    OP(DOLVM_OP_JMP_IF_CR_GUARD): {
        // The whole of a loop's back edge. Not taken, this costs the payload
        // step and nothing else; taken, it is the budget check and the charge
        // that the separate guarded jump used to be.
        if ((((ctx->cr << inst->a) >> 31) & 1u) != (inst->b & 1u)) {
            ip++;
            NEXT();
        }
        u64 payload = dolvm_payload(ip);
        if ((inst->b & 0x80u) || DOLVM_POLLING()) {
#ifdef DOLVM_PROFILE
            ++g_dolvm_leave_idle;
#endif
            DOLVM_IDLE_LEAVE((u32)payload);
        }
        if (DOLVM_OVER_BUDGET() || ++steps >= step_budget) {
            ctx->pc = (u32)payload;
            goto leave;
        }
        ctx->downcount -= (s64)(u32)(payload >> 32);
        ip = code + inst->imm;
        NEXT();
    }

    OP(DOLVM_OP_JMP_GUARD): {
        // LOOP_GUARD and the JMP it always preceded, in one dispatch. The guard
        // is on the edge rather than the loop header for the same reason it was
        // before: entering a header with the budget already spent still has to
        // run the body once. The target's cycle charge rides in the high half
        // and is only paid when the edge is actually taken.
        u64 payload = dolvm_payload(ip);
        if (inst->a || DOLVM_POLLING()) {
#ifdef DOLVM_PROFILE
            ++g_dolvm_leave_idle;
#endif
            DOLVM_IDLE_LEAVE((u32)payload);
        }
        if (DOLVM_OVER_BUDGET() || ++steps >= step_budget) {
            ctx->pc = (u32)payload;
            goto leave;
        }
        ctx->downcount -= (s64)(u32)(payload >> 32);
        // The loop header this edge lands on. Counting only at CHARGE misses
        // every loop whose header charge was folded into its own back edge --
        // which is every hot loop the emitter could fold, so the block
        // histogram used to be blind to exactly the ones worth finding.
        DOLVM_COUNT_BLOCK((u32)payload);
        ip = code + inst->imm;
        NEXT();
    }

    OP(DOLVM_OP_JMP_IF_CR):
        // `bc` reduced to what it is: one bit of CR against one sense. The
        // builder's state load, mask and compare are all folded in here.
        if ((((ctx->cr << inst->a) >> 31) & 1u) == inst->b)
            ip = code + inst->imm;
        NEXT();

    OP(DOLVM_OP_CMP_STATE_I): {
        u64 payload = dolvm_payload(ip);
        ip++;
        u32 left = *(const u32*)((const u8*)ctx + (u32)(payload >> 32));
        u32 right = (u32)payload;
        u32 bits = dolvm_cr_bits(left, right, ((inst->imm >> 8) & 1u) != 0) |
                   dolvm_summary_overflow(regs);
        u32 shift = 4u * (7u - (inst->imm & 0xFFu));
        ctx->cr = (ctx->cr & ~(0xFu << shift)) | (bits << shift);
        NEXT();
    }

    OP(DOLVM_OP_CMP_STATE): {
        u64 payload = dolvm_payload(ip);
        ip++;
        u32 left = *(const u32*)((const u8*)ctx + (u32)(payload >> 32));
        u32 right = *(const u32*)((const u8*)ctx + (u32)payload);
        u32 bits = dolvm_cr_bits(left, right, ((inst->imm >> 8) & 1u) != 0) |
                   dolvm_summary_overflow(regs);
        u32 shift = 4u * (7u - (inst->imm & 0xFFu));
        ctx->cr = (ctx->cr & ~(0xFu << shift)) | (bits << shift);
        NEXT();
    }

#ifndef DOLVM_THREADED
    default:
        break;
    }
#endif
    // dolvm_module_open rejects every unknown opcode, so this is reachable only
    // if the image changed after it was validated. Leaving is the safe move.
    ctx->pc = pc_base;

leave:
#ifdef DOLVM_PROFILE
    dolvm_count_leave(ctx->pc);
#endif
    dolvm_xer_out(ctx, regs);
    if (homed)
        dolvm_homes_flush(ctx, regs);
    return 1;
}

#ifdef DOLVM_PROFILE
static void dolvm_slot_report(FILE* out, u64 total);

void dolvm_profile_report(FILE* out) {
    u64 total = 0;
    for (u32 op = 0; op < DOLVM_OP_COUNT; op++)
        total += g_dolvm_op_counts[op];
    if (!total)
        return;
    for (u32 op = 0; op < DOLVM_OP_COUNT; op++) {
        if (g_dolvm_op_handlers[op])
            fprintf(out, "dolvm handler %p %s\n", g_dolvm_op_handlers[op],
                    dolvm_op_name(op));
    }
    fprintf(out,
            "dolvm leaves: %llu unresolved indirect, %llu exit, %llu guard, "
            "%llu idle, %llu call; %llu indirects and %llu calls resolved in place\n",
            (unsigned long long)g_dolvm_leave_indirect,
            (unsigned long long)g_dolvm_leave_exit,
            (unsigned long long)g_dolvm_leave_guard,
            (unsigned long long)g_dolvm_leave_idle,
            (unsigned long long)g_dolvm_leave_call,
            (unsigned long long)g_dolvm_leave_resolved,
            (unsigned long long)g_dolvm_leave_called);
    fprintf(out,
            "dolvm indirect misses: %llu outside the region, %llu no entry, "
            "%llu not a return target, %llu region closed\n",
            (unsigned long long)g_dolvm_miss_region,
            (unsigned long long)g_dolvm_miss_gap,
            (unsigned long long)g_dolvm_miss_target,
            (unsigned long long)g_dolvm_miss_closed);
    fprintf(out, "dolvm profile: %llu ops over %llu dispatches (%.1f per dispatch)\n",
            (unsigned long long)total, (unsigned long long)g_dolvm_dispatches,
            g_dolvm_dispatches ? (double)total / (double)g_dolvm_dispatches : 0.0);
    for (u32 round = 0; round < 8u; round++) {
        u32 best = DOLVM_PROFILE_LEAVE_SITES;
        for (u32 i = 0; i < DOLVM_PROFILE_LEAVE_SITES; i++) {
            if (g_dolvm_leave_pc_count[i] &&
                (best == DOLVM_PROFILE_LEAVE_SITES ||
                 g_dolvm_leave_pc_count[i] > g_dolvm_leave_pc_count[best]))
                best = i;
        }
        if (best == DOLVM_PROFILE_LEAVE_SITES)
            break;
        fprintf(out, "  leaves at 0x%08X: %llu\n", g_dolvm_leave_pc[best],
                (unsigned long long)g_dolvm_leave_pc_count[best]);
        g_dolvm_leave_pc_count[best] = 0;
    }
    // Insertion sort by count; DOLVM_OP_COUNT is small and this runs once.
    u32 order[DOLVM_OP_COUNT];
    for (u32 op = 0; op < DOLVM_OP_COUNT; op++)
        order[op] = op;
    for (u32 i = 1; i < DOLVM_OP_COUNT; i++) {
        u32 key = order[i];
        u32 j = i;
        while (j && g_dolvm_op_counts[order[j - 1u]] < g_dolvm_op_counts[key]) {
            order[j] = order[j - 1u];
            j--;
        }
        order[j] = key;
    }
    for (u32 i = 0; i < DOLVM_OP_COUNT; i++) {
        u64 count = g_dolvm_op_counts[order[i]];
        if (!count)
            break;
        fprintf(out, "  %6.2f%%  %14llu  %s\n", 100.0 * (double)count / (double)total,
                (unsigned long long)count, dolvm_op_name(order[i]));
    }
    dolvm_slot_report(out, total);
    fprintf(out, "dolvm hottest guest addresses outside RAM:\n");
    for (u32 round = 0; round < 16u; round++) {
        u32 best = DOLVM_PROFILE_MMIO_SITES;
        for (u32 i = 0; i < DOLVM_PROFILE_MMIO_SITES; i++)
            if (g_dolvm_mmio_count[i] &&
                (best == DOLVM_PROFILE_MMIO_SITES ||
                 g_dolvm_mmio_count[i] > g_dolvm_mmio_count[best]))
                best = i;
        if (best == DOLVM_PROFILE_MMIO_SITES)
            break;
        fprintf(out, "  0x%08X  %14llu\n", g_dolvm_mmio_addr[best],
                (unsigned long long)g_dolvm_mmio_count[best]);
        g_dolvm_mmio_count[best] = 0;
    }
    fprintf(out, "dolvm hottest guest addresses written outside RAM:\n");
    for (u32 round = 0; round < 12u; round++) {
        u32 best = DOLVM_PROFILE_MMIO_SITES;
        for (u32 i = 0; i < DOLVM_PROFILE_MMIO_SITES; i++)
            if (g_dolvm_mmio_wcount[i] &&
                (best == DOLVM_PROFILE_MMIO_SITES ||
                 g_dolvm_mmio_wcount[i] > g_dolvm_mmio_wcount[best]))
                best = i;
        if (best == DOLVM_PROFILE_MMIO_SITES)
            break;
        fprintf(out, "  0x%08X  %14llu\n", g_dolvm_mmio_waddr[best],
                (unsigned long long)g_dolvm_mmio_wcount[best]);
        g_dolvm_mmio_wcount[best] = 0;
    }
    fprintf(out, "dolvm hottest guest blocks (%llu block entries, %llu dropped):\n",
            (unsigned long long)g_dolvm_block_total,
            (unsigned long long)g_dolvm_block_dropped);
    for (u32 round = 0; round < 24u; round++) {
        u32 best = DOLVM_PROFILE_BLOCK_SITES;
        for (u32 i = 0; i < DOLVM_PROFILE_BLOCK_SITES; i++)
            if (g_dolvm_block_count[i] &&
                (best == DOLVM_PROFILE_BLOCK_SITES ||
                 g_dolvm_block_count[i] > g_dolvm_block_count[best]))
                best = i;
        if (best == DOLVM_PROFILE_BLOCK_SITES)
            break;
        fprintf(out, "  0x%08X  %14llu\n", g_dolvm_block_pc[best],
                (unsigned long long)g_dolvm_block_count[best]);
        g_dolvm_block_count[best] = 0;
    }
    fprintf(out, "dolvm hottest opcode pairs:\n");
    for (u32 round = 0; round < 40u; round++) {
        u32 best_a = 0, best_b = 0;
        u64 best = 0;
        for (u32 a = 0; a < DOLVM_OP_COUNT; a++)
            for (u32 b = 0; b < DOLVM_OP_COUNT; b++)
                if (g_dolvm_pair_counts[a][b] > best) {
                    best = g_dolvm_pair_counts[a][b];
                    best_a = a;
                    best_b = b;
                }
        if (!best)
            break;
        fprintf(out, "  %6.2f%%  %14llu  %s -> %s\n", 100.0 * (double)best / (double)total,
                (unsigned long long)best, dolvm_op_name(best_a), dolvm_op_name(best_b));
        g_dolvm_pair_counts[best_a][best_b] = 0;
    }
}

// Which slots the state traffic names, and how wide a working set one dispatch
// has. A global allocator pays its prologue per dispatch and earns it per
// access, so both halves of that trade are here.
static void dolvm_slot_report(FILE* out, u64 total) {
    u64 slot_total = 0;
    for (u32 i = 0; i < DOLVM_PROFILE_SLOTS; i++)
        slot_total += g_dolvm_slot_counts[i];
    if (!slot_total)
        return;
    fprintf(out, "dolvm state traffic: %llu accesses (%.2f%% of ops)\n",
            (unsigned long long)slot_total,
            total ? 100.0 * (double)slot_total / (double)total : 0.0);
    u32 order[DOLVM_PROFILE_SLOTS];
    u32 live = 0;
    for (u32 i = 0; i < DOLVM_PROFILE_SLOTS; i++)
        if (g_dolvm_slot_counts[i])
            order[live++] = i;
    for (u32 i = 1; i < live; i++) {
        u32 key = order[i];
        u32 j = i;
        while (j && g_dolvm_slot_counts[order[j - 1u]] < g_dolvm_slot_counts[key]) {
            order[j] = order[j - 1u];
            j--;
        }
        order[j] = key;
    }
    u64 running = 0;
    for (u32 i = 0; i < live && i < 40u; i++) {
        u32 word = order[i];
        running += g_dolvm_slot_counts[word];
        fprintf(out, "  slot 0x%04X %6.2f%% cumulative %6.2f%%%s\n", word * 4u,
                100.0 * (double)g_dolvm_slot_counts[word] / (double)slot_total,
                100.0 * (double)running / (double)slot_total,
                word * 4u < 128u ? "  gpr" : "");
    }
    u64 dispatches = 0;
    u64 weighted = 0;
    for (u32 i = 0; i <= 32u; i++) {
        dispatches += g_dolvm_gpr_span[i];
        weighted += g_dolvm_gpr_span[i] * i;
    }
    if (!dispatches)
        return;
    fprintf(out, "dolvm gpr working set: %.2f distinct per dispatch\n",
            (double)weighted / (double)dispatches);
    u64 running_span = 0;
    for (u32 i = 0; i <= 32u; i++) {
        if (!g_dolvm_gpr_span[i])
            continue;
        running_span += g_dolvm_gpr_span[i];
        fprintf(out, "  %2u gprs %6.2f%% cumulative %6.2f%%\n", i,
                100.0 * (double)g_dolvm_gpr_span[i] / (double)dispatches,
                100.0 * (double)running_span / (double)dispatches);
    }
}
#endif
