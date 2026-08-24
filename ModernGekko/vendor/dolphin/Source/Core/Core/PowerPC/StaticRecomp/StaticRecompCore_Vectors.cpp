// RecompCore: StaticRecomp CPU core - OS exception-vector stand-ins.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Every Nintendo SDK title copies the same exception stub to each vector in
// low RAM at boot, which the module cannot cover: the code exists only at
// runtime, at a physical address. So every guest exception used to run its
// stub one instruction at a time through the plain interpreter -- Read_Opcode,
// MMU translate, GetOpInfo, HLE check per step -- which a desktop absorbs and
// a phone does not (~9% of the emulation thread on an iPhone 15 Pro Max,
// 164K exceptions per 24e9 cycles, ~21 interpreted instructions each).
//
// The stub is provable the way the DolVM SDK stand-ins are proved: every
// instruction word compared against a stored pattern. Across four titles
// spanning both SDK generations the 38-word template is bit-identical except
// exactly two kinds of word, both verified structurally and used as data:
//   - word 26, `li r3, <n>`: n must be the architectural exception number the
//     vector's own address implies, so it is checked, not masked;
//   - words 30/31, `lis r5, hi; addi r5, r5, lo`: the debugger jump target the
//     SDK links in. The pair's opcodes and registers are fixed; the immediate
//     is the one thing that varies per title, and the stand-in only ever uses
//     it as the value mtsrr0 would have received.
// The system-call vector instead holds a 7-word fast path (an HID0.ABE dance
// Dolphin ignores, then rfi), identical across every title checked.
//
// A verified vector executes as straight C against PowerPCState: the same
// register writes, the same context stores, the same CR0/SRR/MSR effects, the
// same cycle charge (summed from the same GetOpInfo the interpreter would have
// consulted, per branch path). Any byte that does not prove out -- or any
// entry with translation or user mode still on -- falls back to the
// interpreter exactly as before. An icache invalidation over low RAM, a
// ClearCache (which savestate loads go through), or a mismatch all drop the
// verification, so a guest that rewrites its vectors is never run stale.

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <cstring>

#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
u32 ReadBE32(const u8* p)
{
  return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
         (static_cast<u32>(p[2]) << 8) | p[3];
}

void WriteBE32(u8* p, u32 v)
{
  p[0] = static_cast<u8>(v >> 24);
  p[1] = static_cast<u8>(v >> 16);
  p[2] = static_cast<u8>(v >> 8);
  p[3] = static_cast<u8>(v);
}

u16 ReadBE16(const u8* p)
{
  return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
}

void WriteBE16(u8* p, u16 v)
{
  p[0] = static_cast<u8>(v >> 8);
  p[1] = static_cast<u8>(v);
}

// The generic OSExceptionVector stub, exactly as OSExceptionInit leaves it in
// low RAM: template words after the one-word `mtspr SPRG0, r4` patch. Word 26
// and words 30/31 are the parameterized forms described above; 0 here means
// "verified specially", every other word must match bit for bit.
constexpr u32 GENERIC_STUB[38] = {
    0x7C9043A6,  //  0: mtspr  SPRG0, r4
    0x808000C0,  //  1: lwz    r4, 0xC0(0)        physical &OSContext
    0x9064000C,  //  2: stw    r3, 0xC(r4)
    0x7C7042A6,  //  3: mfspr  r3, SPRG0
    0x90640010,  //  4: stw    r3, 0x10(r4)
    0x90A40014,  //  5: stw    r5, 0x14(r4)
    0xA06401A2,  //  6: lhz    r3, 0x1A2(r4)
    0x60630002,  //  7: ori    r3, r3, 2
    0xB06401A2,  //  8: sth    r3, 0x1A2(r4)
    0x7C600026,  //  9: mfcr   r3
    0x90640080,  // 10: stw    r3, 0x80(r4)
    0x7C6802A6,  // 11: mflr   r3
    0x90640084,  // 12: stw    r3, 0x84(r4)
    0x7C6902A6,  // 13: mfctr  r3
    0x90640088,  // 14: stw    r3, 0x88(r4)
    0x7C6102A6,  // 15: mfxer  r3
    0x9064008C,  // 16: stw    r3, 0x8C(r4)
    0x7C7A02A6,  // 17: mfsrr0 r3
    0x90640198,  // 18: stw    r3, 0x198(r4)
    0x7C7B02A6,  // 19: mfsrr1 r3
    0x9064019C,  // 20: stw    r3, 0x19C(r4)
    0x7C651B78,  // 21: mr     r5, r3
    0x60000000,  // 22: nop
    0x7C6000A6,  // 23: mfmsr  r3
    0x60630030,  // 24: ori    r3, r3, 0x30       IR|DR back on for the handler
    0x7C7B03A6,  // 25: mtsrr1 r3
    0,           // 26: li     r3, <vector number>
    0x808000D4,  // 27: lwz    r4, 0xD4(0)        virtual &OSContext
    0x54A507BD,  // 28: rlwinm. r5, r5, 0, 30, 30 test SRR1[RI]
    0x40820014,  // 29: bne    +0x14              recoverable: table dispatch
    0,           // 30: lis    r5, <debugger target hi>
    0,           // 31: addi   r5, r5, <debugger target lo>
    0x7CBA03A6,  // 32: mtsrr0 r5
    0x4C000064,  // 33: rfi
    0x546515BA,  // 34: rlwinm r5, r3, 2, 22, 29
    0x80A53000,  // 35: lwz    r5, 0x3000(r5)     handler table
    0x7CBA03A6,  // 36: mtsrr0 r5
    0x4C000064,  // 37: rfi
};

// __OSSystemCallVector: raise HID0.ABE around a sync, then return. Dolphin
// ignores ABE entirely, so only the register writes and the rfi are visible.
constexpr u32 SYSCALL_STUB[7] = {
    0x7D30FAA6,  // mfspr r9, HID0
    0x612A0008,  // ori   r10, r9, 8
    0x7D50FBA6,  // mtspr HID0, r10
    0x4C00012C,  // isync
    0x7C0004AC,  // sync
    0x7D30FBA6,  // mtspr HID0, r9
    0x4C000064,  // rfi
};

// Architectural exception number per vector, the value the stub's `li r3, n`
// must carry. -1 marks an address no Gekko exception vectors to.
constexpr int VECTOR_NUMBER[0x18] = {
    -1, 0,  1,  2,  3,  4,  5,  6,   // 0x000..0x700
    7,  8,  -1, -1, 9,  10, -1, 11,  // 0x800..0xF00
    -1, -1, -1, 12, 13, -1, -1, 14,  // 0x1000..0x1700
};

// One interpreted step's worth of accounting for each word a path executes,
// summed at verification time from the same tables SingleStepInner reads.
template <typename Charge>
void ChargeFor(const u32* words, const u32* indices, u32 count, u32 base_pc, Charge* charge)
{
  charge->cycles = 0;
  charge->load_stores = 0;
  for (u32 i = 0; i < count; ++i)
  {
    UGeckoInstruction inst;
    inst.hex = words[indices[i]];
    const GekkoOPInfo* info = PPCTables::GetOpInfo(inst, base_pc + indices[i] * 4);
    charge->cycles += info ? info->num_cycles : 1;
    if (info && (info->flags & FL_LOADSTORE))
      ++charge->load_stores;
  }
}
}  // namespace

void StaticRecompCore::ResetVectorStubs()
{
  std::memset(m_vector_stub_state, VECTOR_STUB_UNKNOWN, sizeof(m_vector_stub_state));
}

// Called with the pc the run loop could not dispatch. True means the stub ran
// and was charged; the caller just continues its loop.
bool StaticRecompCore::TryVectorStub(PowerPC::PowerPCState& ppc)
{
  const u32 pc = ppc.pc;
  if (pc >= 0x1800u || (pc & 0xFFu) != 0 || pc < 0x100u)
    return false;
  const u32 slot = pc >> 8;
  if (VECTOR_NUMBER[slot] < 0)
    return false;
  // The stub was verified against physical RAM and stores through physical
  // addresses; both are only what execution would do in the mode exception
  // delivery leaves the CPU in.
  if (ppc.msr.PR || ppc.msr.IR || ppc.msr.DR)
    return false;

  VectorStubSlot& stub = m_vector_stub_slots[slot];
  if (m_vector_stub_state[slot] == VECTOR_STUB_UNKNOWN)
    VerifyVectorStub(slot);
  if (m_vector_stub_state[slot] != VECTOR_STUB_VERIFIED)
    return false;

  auto& memory = m_system.GetMemory();
  u8* ram = memory.GetRAM();
  auto& power_pc = m_system.GetPowerPC();

  if (stub.syscall)
  {
    // r9 = HID0; r10 = r9 | 8; HID0 ends where it began; rfi.
    ppc.gpr[9] = ppc.spr[SPR_HID0];
    ppc.gpr[10] = ppc.gpr[9] | 8u;
    const u32 mask = 0x87C0FFFF;
    ppc.msr.Hex = (ppc.msr.Hex & ~mask) | (ppc.spr[SPR_SRR1] & mask);
    ppc.msr.Hex &= 0xFFFBFFFF;
    ppc.pc = ppc.npc = ppc.spr[SPR_SRR0];
    power_pc.MSRUpdated();
    PowerPC::UpdatePerformanceMonitor(stub.charge_taken.cycles, 0, 0, ppc);
    ppc.downcount -= static_cast<int>(stub.charge_taken.cycles);
    ++m_vector_stub_hits;
    return true;
  }

  // The context pointer the stub stores through. A garbage pointer would have
  // sent the interpreted stub to the MMU too; that case keeps the slow path.
  const u32 ctx = ReadBE32(ram + 0xC0);
  const u32 ram_size = memory.GetRamSizeReal();
  if (ctx > ram_size || ram_size - ctx < 0x1A4u)
    return false;
  u8* c = ram + ctx;

  const u32 old_r3 = ppc.gpr[3];
  const u32 old_r4 = ppc.gpr[4];
  const u32 old_r5 = ppc.gpr[5];
  const u32 old_srr0 = ppc.spr[SPR_SRR0];
  const u32 old_srr1 = ppc.spr[SPR_SRR1];
  const u32 old_msr = ppc.msr.Hex;
  const u32 xer = ppc.GetXER().Hex;

  ppc.spr[SPR_SPRG0] = old_r4;
  WriteBE32(c + 0x0C, old_r3);
  WriteBE32(c + 0x10, old_r4);
  WriteBE32(c + 0x14, old_r5);
  WriteBE16(c + 0x1A2, static_cast<u16>(ReadBE16(c + 0x1A2) | 2u));
  WriteBE32(c + 0x80, ppc.cr.Get());
  WriteBE32(c + 0x84, ppc.spr[SPR_LR]);
  WriteBE32(c + 0x88, ppc.spr[SPR_CTR]);
  WriteBE32(c + 0x8C, xer);
  WriteBE32(c + 0x198, old_srr0);
  WriteBE32(c + 0x19C, old_srr1);

  // rlwinm. leaves CR0 holding the SRR1[RI] test: the masked value is 0 or 2,
  // so EQ or GT, with SO carried over from XER.
  const bool recoverable = (old_srr1 & 2u) != 0;
  u32 cr0 = recoverable ? 0x4u : 0x2u;
  if (xer & 0x80000000u)
    cr0 |= 1u;
  ppc.cr.Set((ppc.cr.Get() & 0x0FFFFFFFu) | (cr0 << 28));

  const u32 target = recoverable ?
      ReadBE32(ram + 0x3000 + 4u * static_cast<u32>(VECTOR_NUMBER[slot])) :
      stub.debugger_target;

  ppc.gpr[3] = static_cast<u32>(VECTOR_NUMBER[slot]);
  ppc.gpr[4] = ReadBE32(ram + 0xD4);
  ppc.gpr[5] = target;
  ppc.spr[SPR_SRR0] = target;
  ppc.spr[SPR_SRR1] = old_msr | 0x30u;

  const u32 mask = 0x87C0FFFF;
  ppc.msr.Hex = (old_msr & ~mask) | (ppc.spr[SPR_SRR1] & mask);
  ppc.msr.Hex &= 0xFFFBFFFF;
  ppc.pc = ppc.npc = target;
  power_pc.MSRUpdated();

  const VectorStubCharge& charge = recoverable ? stub.charge_taken : stub.charge_fallthrough;
  PowerPC::UpdatePerformanceMonitor(charge.cycles, charge.load_stores, 0, ppc);
  ppc.downcount -= static_cast<int>(charge.cycles);
  ++m_vector_stub_hits;
  return true;
}

void StaticRecompCore::VerifyVectorStub(u32 slot)
{
  m_vector_stub_state[slot] = VECTOR_STUB_MISMATCH;
  ++m_vector_stub_verifies;

  auto& memory = m_system.GetMemory();
  const u8* ram = memory.GetRAM();
  const u32 base = slot << 8;
  VectorStubSlot& stub = m_vector_stub_slots[slot];

  u32 words[38];
  for (u32 i = 0; i < 38; ++i)
    words[i] = ReadBE32(ram + base + i * 4);

  if (base == 0xC00)
  {
    for (u32 i = 0; i < 7; ++i)
    {
      if (words[i] != SYSCALL_STUB[i])
        return;
    }
    stub.syscall = true;
    static constexpr u32 all[7] = {0, 1, 2, 3, 4, 5, 6};
    ChargeFor(words, all, 7, base, &stub.charge_taken);
    m_vector_stub_state[slot] = VECTOR_STUB_VERIFIED;
    return;
  }

  for (u32 i = 0; i < 38; ++i)
  {
    if (GENERIC_STUB[i] != 0 && words[i] != GENERIC_STUB[i])
      return;
  }
  if (words[26] != (0x38600000u | static_cast<u32>(VECTOR_NUMBER[slot])))
    return;
  if ((words[30] & 0xFFFF0000u) != 0x3CA00000u || (words[31] & 0xFFFF0000u) != 0x38A50000u)
    return;

  stub.syscall = false;
  const u32 hi = words[30] & 0xFFFFu;
  const s32 lo = static_cast<s16>(words[31] & 0xFFFFu);
  stub.debugger_target = static_cast<u32>(static_cast<s32>(hi << 16) + lo);

  static constexpr u32 taken[34] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                                    24, 25, 26, 27, 28, 29, 34, 35, 36, 37};
  static constexpr u32 fallthrough[34] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                          12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                                          24, 25, 26, 27, 28, 29, 30, 31, 32, 33};
  ChargeFor(words, taken, 34, base, &stub.charge_taken);
  ChargeFor(words, fallthrough, 34, base, &stub.charge_fallthrough);
  m_vector_stub_state[slot] = VECTOR_STUB_VERIFIED;
}
