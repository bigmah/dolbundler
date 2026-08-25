// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dispatch gate: what a chassis knows that lets a module follow a control
// transfer in place instead of returning for every one.
//
// A chassis checks things on every dispatch -- that the chunk still matches
// guest RAM, that no mod has hooked the address, that its timing slice has
// something left -- and a module that resolved a call or a return by itself
// would skip all of them. So a module resolves nothing across chunks unless the
// chassis hands it this, and then resolves only what the gate says the chassis
// would have dispatched into anyway. Every field points into chassis-owned
// memory: the chassis updates it in place and the module reads it on the
// emulation thread, so no call crosses the boundary on the hot path.
//
// Shared by the chassis and whichever module runtime consumes it (today the
// DolVM bridge), which is why it is plain C with nothing but fixed-width types.

#ifndef GXRUNTIME_CORE_DISPATCH_GATE_H
#define GXRUNTIME_CORE_DISPATCH_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StaticRecompDispatchGate
{
  // One byte per chunk, parallel to the module descriptor's chunk_ranges:
  // nonzero while the chassis would itself dispatch into that chunk right now.
  const uint8_t* chunk_open;
  uint32_t chunk_count;
  // The chassis's live slice counter: guest cycles left before it needs
  // control back. A dispatch returns once what it has charged reaches this.
  // Live, not a snapshot, because the chassis shortens it from inside the
  // hooks a module calls -- an event scheduled by an MMIO write lands before
  // the slice was due to end -- and a module that kept running to the old end
  // would deliver every such event late.
  const int32_t* budget;
  // A word the chassis raises exceptions in, and the bits that mean "return
  // now": the synchronous set unconditionally, the asynchronous set only once
  // the guest has MSR[EE] set.
  const uint32_t* pending;
  uint32_t pending_sync;
  uint32_t pending_async;
} StaticRecompDispatchGate;

// Native modules publish the chassis-owned pointers here so generated loop
// guards can read them directly.  A zeroed value means no gate is available.
// Keeping the small descriptor in module-owned data avoids a helper call on
// every guest back edge while preserving the same live checks as DolVM.
extern StaticRecompDispatchGate dolrecomp_native_gate;

// True while the charge accumulated since the last chassis flush still fits
// in the live timing slice.  The accumulator is non-positive: generated code
// subtracts cycles from CPUState::downcount, and a hook resets it to zero after
// moving those cycles into *gate->budget.  Comparing a lifetime-wide counter
// here would count flushed cycles twice.
static inline bool staticrecomp_dispatch_budget_allows(
    const StaticRecompDispatchGate* gate, int64_t charge_accumulator)
{
  if (!gate || !gate->budget)
    return true;
  const int32_t budget = *gate->budget;
  return budget > 0 && charge_accumulator > -(int64_t)budget;
}

static inline bool staticrecomp_dispatch_has_pending(
    const StaticRecompDispatchGate* gate, uint32_t msr)
{
  if (!gate || !gate->pending)
    return false;
  const uint32_t pending = *gate->pending;
  return (pending & gate->pending_sync) != 0 ||
         ((msr & 0x00008000u) != 0 && (pending & gate->pending_async) != 0);
}

#ifdef __cplusplus
}
#endif

#endif
