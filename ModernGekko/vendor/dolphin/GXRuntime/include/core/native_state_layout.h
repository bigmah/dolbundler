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

#include <stdbool.h>
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

// --- Target layout ------------------------------------------------------------
//
// Every field of CPUState up to and including `locked_cache_valid` is u32, u64,
// f64, u8 or bool, so the prefix lays out identically on every target this
// backend can reach. Everything after it is pointers and the few words wedged
// between them, and **that tail is the only thing a 32-bit target lays out
// differently** -- which is why generating a wasm32 module used to be blocked
// on "the layouts cannot match". They match everywhere except here.
//
// `StaticRecompDispatchGate` has the same shape and the same problem.
//
// Nothing below duplicates the prefix: it starts from the host's own
// `offsetof(locked_cache_valid)`, which is correct for both. And
// `dolnative_target_layout_matches_host()` checks the walker against the host
// compiler, so a mistake here is caught before it can reach a module.

typedef struct DolNativeTargetLayout
{
  uint32_t pointer_size;
  uint32_t external_read, external_write, ram, ram_size, downcount, exram,
      exram_size;
  uint32_t gate_chunk_open, gate_chunk_count, gate_budget, gate_pending,
      gate_pending_sync, gate_pending_async;
  uint32_t gate_size;
} DolNativeTargetLayout;

static inline uint32_t dolnative_place(uint32_t* at, uint32_t size, uint32_t align)
{
  *at = (*at + align - 1u) & ~(align - 1u);
  const uint32_t offset = *at;
  *at += size;
  return offset;
}

static inline DolNativeTargetLayout dolnative_target_layout(uint32_t pointer_size)
{
  DolNativeTargetLayout l;
  const uint32_t p = pointer_size;
  l.pointer_size = p;

  uint32_t at = (uint32_t)(offsetof(CPUState, locked_cache_valid) +
                           sizeof(((CPUState*)0)->locked_cache_valid));
  l.external_read = dolnative_place(&at, p, p);
  l.external_write = dolnative_place(&at, p, p);
  dolnative_place(&at, p, p); /* external_read32 */
  dolnative_place(&at, p, p); /* external_write32 */
  dolnative_place(&at, p, p); /* instruction_fallback */
  dolnative_place(&at, p, p); /* host_call */
  dolnative_place(&at, p, p); /* external_user_data */
  l.ram = dolnative_place(&at, p, p);
  l.ram_size = dolnative_place(&at, 4u, 4u);
  dolnative_place(&at, p, p); /* external_pointer */
  l.downcount = dolnative_place(&at, 8u, 8u);
  l.exram = dolnative_place(&at, p, p);
  l.exram_size = dolnative_place(&at, 4u, 4u);
  // **Stop here.** DOLNATIVE_LAYOUT_FIELDS ends at EXRAM_SIZE, and past it the
  // two CPUState headers this file is meant to work against genuinely diverge:
  // GXRuntime has spr_read/spr_write, DolRecomp has u32 spr[1024]. Walking
  // further would describe neither, and nothing past here is part of the ABI
  // the generated offsets carry.

  at = 0u;
  l.gate_chunk_open = dolnative_place(&at, p, p);
  l.gate_chunk_count = dolnative_place(&at, 4u, 4u);
  l.gate_budget = dolnative_place(&at, p, p);
  l.gate_pending = dolnative_place(&at, p, p);
  l.gate_pending_sync = dolnative_place(&at, 4u, 4u);
  l.gate_pending_async = dolnative_place(&at, 4u, 4u);
  l.gate_size = (at + p - 1u) & ~(p - 1u);
  return l;
}

// True when the walker reproduces the host compiler's own layout. Call it once
// before trusting the walker for any other pointer size: if this is false the
// tail above has drifted from cpu.h and every offset it produces is wrong.
static inline bool dolnative_target_layout_matches_host(void)
{
  const DolNativeTargetLayout l =
      dolnative_target_layout((uint32_t)sizeof(void*));
  return l.external_read == (uint32_t)offsetof(CPUState, external_read) &&
         l.external_write == (uint32_t)offsetof(CPUState, external_write) &&
         l.ram == (uint32_t)offsetof(CPUState, ram) &&
         l.ram_size == (uint32_t)offsetof(CPUState, ram_size) &&
         l.downcount == (uint32_t)offsetof(CPUState, downcount) &&
         l.exram == (uint32_t)offsetof(CPUState, exram) &&
         l.exram_size == (uint32_t)offsetof(CPUState, exram_size);
}

// A name per field, so a target layout can be indexed the same way the macro
// list walks it.
typedef enum DolNativeFieldId
{
#define DOLNATIVE_FIELD_ENUM(id, field, count) DOLNATIVE_FIELD_##id,
  DOLNATIVE_LAYOUT_FIELDS(DOLNATIVE_FIELD_ENUM)
#undef DOLNATIVE_FIELD_ENUM
      DOLNATIVE_FIELD_COUNT
} DolNativeFieldId;

// The target's offset for one field. Everything except the pointer-bearing tail
// is in the prefix, where every target agrees, so those come straight from the
// host's own offsetof.
static inline uint32_t dolnative_target_field_offset(
    const DolNativeTargetLayout* l, DolNativeFieldId id)
{
  switch (id)
  {
  case DOLNATIVE_FIELD_EXTERNAL_READ: return l->external_read;
  case DOLNATIVE_FIELD_EXTERNAL_WRITE: return l->external_write;
  case DOLNATIVE_FIELD_RAM: return l->ram;
  case DOLNATIVE_FIELD_RAM_SIZE: return l->ram_size;
  case DOLNATIVE_FIELD_DOWNCOUNT: return l->downcount;
  case DOLNATIVE_FIELD_EXRAM: return l->exram;
  case DOLNATIVE_FIELD_EXRAM_SIZE: return l->exram_size;
  default: break;
  }
#define DOLNATIVE_FIELD_OFFSET_CASE(fid, field, count)                           if (id == DOLNATIVE_FIELD_##fid)                                                 return (uint32_t)offsetof(CPUState, field);
  DOLNATIVE_LAYOUT_FIELDS(DOLNATIVE_FIELD_OFFSET_CASE)
#undef DOLNATIVE_FIELD_OFFSET_CASE
  return 0u;
}

// The target's size for one field: a pointer is the target's pointer, and
// everything else is the size the host sees.
static inline uint32_t dolnative_target_field_size(
    const DolNativeTargetLayout* l, DolNativeFieldId id)
{
  switch (id)
  {
  case DOLNATIVE_FIELD_EXTERNAL_READ:
  case DOLNATIVE_FIELD_EXTERNAL_WRITE:
  case DOLNATIVE_FIELD_RAM:
  case DOLNATIVE_FIELD_EXRAM:
    return l->pointer_size;
  default: break;
  }
#define DOLNATIVE_FIELD_SIZE_CASE(fid, field, count)                             if (id == DOLNATIVE_FIELD_##fid)                                                 return (uint32_t)sizeof(((CPUState*)0)->field);
  DOLNATIVE_LAYOUT_FIELDS(DOLNATIVE_FIELD_SIZE_CASE)
#undef DOLNATIVE_FIELD_SIZE_CASE
  return 0u;
}

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

// The same hash, over a target's layout rather than the host's. A module built
// for one pointer size must not be mistaken for one built for another, and the
// hash is what the chassis compares.
static inline uint64_t dolnative_state_layout_hash_for(
    const DolNativeTargetLayout* target)
{
  uint64_t hash = UINT64_C(1469598103934665603);
#define DOLNATIVE_HASH_TARGET_FIELD(id, field, count)                          \
  do                                                                           \
  {                                                                            \
    hash = dolnative_layout_hash_u32(                                          \
        hash, dolnative_target_field_offset(target, DOLNATIVE_FIELD_##id));    \
    hash = dolnative_layout_hash_u32(                                          \
        hash, dolnative_target_field_size(target, DOLNATIVE_FIELD_##id));      \
    hash = dolnative_layout_hash_u32(hash, (uint32_t)(count));                 \
  } while (0);
  DOLNATIVE_LAYOUT_FIELDS(DOLNATIVE_HASH_TARGET_FIELD)
#undef DOLNATIVE_HASH_TARGET_FIELD
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
