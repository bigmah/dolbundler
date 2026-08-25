// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPUState layout consumed by generated native code.
//
// Native objects contain byte offsets rather than C field references.  This
// list is therefore the ABI: it covers every architectural state array and
// scalar addressed by DolIR plus the memory, hook, exception, timing, and
// reservation fields addressed directly by the LLVM lowering.  The header is
// deliberately usable after either DolRecomp's CPU header or GXRuntime's CPU
// header has defined CPUState; their shared include guard makes the same code
// fingerprint both layouts.

#ifndef GXRUNTIME_CORE_NATIVE_STATE_LAYOUT_H
#define GXRUNTIME_CORE_NATIVE_STATE_LAYOUT_H

#include "core/cpu.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOLNATIVE_LAYOUT_FIELDS(X)                                             \
  X(GPR, gpr, 32u)                                                             \
  X(FPR, fpr, 32u)                                                             \
  X(PS1, ps1, 32u)                                                             \
  X(PC, pc, 1u)                                                               \
  X(LR, lr, 1u)                                                               \
  X(CTR, ctr, 1u)                                                             \
  X(CR, cr, 1u)                                                               \
  X(XER, xer, 1u)                                                             \
  X(FPSCR, fpscr, 1u)                                                         \
  X(MSR, msr, 1u)                                                             \
  X(SRR0, srr0, 1u)                                                           \
  X(SRR1, srr1, 1u)                                                           \
  X(DAR, dar, 1u)                                                             \
  X(DSISR, dsisr, 1u)                                                         \
  X(EAR, ear, 1u)                                                             \
  X(HID2, hid2, 1u)                                                           \
  X(TIMEBASE, timebase, 1u)                                                   \
  X(SR, sr, 16u)                                                              \
  X(GQR, gqr, 8u)                                                             \
  X(EXCEPTION, exception, 1u)                                                 \
  X(PROGRAM_EXCEPTION, program_exception, 1u)                                 \
  X(RESERVE_ADDR, reserve_addr, 1u)                                           \
  X(RESERVE_VALID, reserve_valid, 1u)                                         \
  X(EXTERNAL_READ, external_read, 1u)                                         \
  X(EXTERNAL_WRITE, external_write, 1u)                                       \
  X(RAM, ram, 1u)                                                             \
  X(RAM_SIZE, ram_size, 1u)                                                   \
  X(DOWNCOUNT, downcount, 1u)                                                 \
  X(EXRAM, exram, 1u)                                                         \
  X(EXRAM_SIZE, exram_size, 1u)

static inline uint64_t dolnative_layout_hash_u32(uint64_t hash, uint32_t value)
{
  for (uint32_t byte = 0; byte < 4u; ++byte)
  {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static inline uint64_t dolnative_state_layout_hash(void)
{
  uint64_t hash = UINT64_C(1469598103934665603);
#define DOLNATIVE_HASH_FIELD(id, field, count)                                 \
  do                                                                           \
  {                                                                            \
    hash = dolnative_layout_hash_u32(hash, (uint32_t)offsetof(CPUState, field));\
    hash = dolnative_layout_hash_u32(                                           \
        hash, (uint32_t)sizeof(((CPUState*)0)->field));                         \
    hash = dolnative_layout_hash_u32(hash, (uint32_t)(count));                  \
  } while (0);
  DOLNATIVE_LAYOUT_FIELDS(DOLNATIVE_HASH_FIELD)
#undef DOLNATIVE_HASH_FIELD
  return hash;
}

// Generated headers publish every base offset as well as the aggregate hash.
// Compiling module_export.c against GXRuntime therefore proves the iPhoneOS
// layout before the linker is allowed to consume a native object manifest.
#ifdef DOLRECOMP_NATIVE_STATE_LAYOUT_HASH
#ifdef __cplusplus
#define DOLNATIVE_STATIC_ASSERT static_assert
#else
#define DOLNATIVE_STATIC_ASSERT _Static_assert
#endif
#define DOLNATIVE_VERIFY_FIELD(id, field, count)                               \
  DOLNATIVE_STATIC_ASSERT(                                                     \
      DOLRECOMP_NATIVE_OFFSET_##id == offsetof(CPUState, field),               \
      "generated native CPUState offset mismatch: " #field);                  \
  DOLNATIVE_STATIC_ASSERT(                                                     \
      DOLRECOMP_NATIVE_SIZE_##id == sizeof(((CPUState*)0)->field),             \
      "generated native CPUState field-size mismatch: " #field);
DOLNATIVE_LAYOUT_FIELDS(DOLNATIVE_VERIFY_FIELD)
#undef DOLNATIVE_VERIFY_FIELD
#undef DOLNATIVE_STATIC_ASSERT
#endif

#ifdef DOLNATIVE_WITH_DOLIR
#include "ir/dolir.h"

static inline size_t dolnative_state_offset(DolIRStateSlot slot)
{
  if (slot >= DOLIR_STATE_GPR0 && slot <= DOLIR_STATE_GPR31)
    return offsetof(CPUState, gpr) + 4u * (slot - DOLIR_STATE_GPR0);
  if (slot >= DOLIR_STATE_FPR0 && slot <= DOLIR_STATE_FPR31)
    return offsetof(CPUState, fpr) + 8u * (slot - DOLIR_STATE_FPR0);
  if (slot >= DOLIR_STATE_PS1_0 && slot <= DOLIR_STATE_PS1_31)
    return offsetof(CPUState, ps1) + 8u * (slot - DOLIR_STATE_PS1_0);
  if (slot >= DOLIR_STATE_SR0 && slot <= DOLIR_STATE_SR15)
    return offsetof(CPUState, sr) + 4u * (slot - DOLIR_STATE_SR0);
  if (slot >= DOLIR_STATE_GQR0 && slot <= DOLIR_STATE_GQR7)
    return offsetof(CPUState, gqr) + 4u * (slot - DOLIR_STATE_GQR0);
  switch (slot)
  {
  case DOLIR_STATE_PC: return offsetof(CPUState, pc);
  case DOLIR_STATE_LR: return offsetof(CPUState, lr);
  case DOLIR_STATE_CTR: return offsetof(CPUState, ctr);
  case DOLIR_STATE_CR: return offsetof(CPUState, cr);
  case DOLIR_STATE_XER: return offsetof(CPUState, xer);
  case DOLIR_STATE_FPSCR: return offsetof(CPUState, fpscr);
  case DOLIR_STATE_MSR: return offsetof(CPUState, msr);
  case DOLIR_STATE_SRR0: return offsetof(CPUState, srr0);
  case DOLIR_STATE_SRR1: return offsetof(CPUState, srr1);
  case DOLIR_STATE_DAR: return offsetof(CPUState, dar);
  case DOLIR_STATE_DSISR: return offsetof(CPUState, dsisr);
  case DOLIR_STATE_EAR: return offsetof(CPUState, ear);
  case DOLIR_STATE_HID2: return offsetof(CPUState, hid2);
  case DOLIR_STATE_TIMEBASE: return offsetof(CPUState, timebase);
  case DOLIR_STATE_EXCEPTION: return offsetof(CPUState, exception);
  case DOLIR_STATE_PROGRAM_EXCEPTION: return offsetof(CPUState, program_exception);
  case DOLIR_STATE_RESERVE_ADDR: return offsetof(CPUState, reserve_addr);
  case DOLIR_STATE_RESERVE_VALID: return offsetof(CPUState, reserve_valid);
  case DOLIR_STATE_DOWNCOUNT: return offsetof(CPUState, downcount);
  default: return 0;
  }
}
#endif

#ifdef __cplusplus
}
#endif

#endif
