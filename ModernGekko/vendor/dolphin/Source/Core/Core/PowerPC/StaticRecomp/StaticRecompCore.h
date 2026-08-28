// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

namespace StaticRecompLockstep
{
class StaticRecompLockstepVerifier;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module; falls back to Dolphin's interpreter for everything else.
// With no module loaded this core is exactly an interpreter loop.
class StaticRecompCore : public JitBase
{
public:
  friend class StaticRecompLockstep::StaticRecompLockstepVerifier;

  explicit StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;
  bool IsModuleActive() const;
  bool DispatchableAt(u32 address);
  bool FastDispatchableAt(u32 address);
  bool IsHostCallAddress(u32 address) const;
  bool ShouldYieldAt(u32 address);

  void ClearCache() override;
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();

  // D4 SMC guard, verify-on-entry model. Every chunk starts Unverified; the
  // first native dispatch into it hashes its guest RAM against the module's
  // recorded hash of the original text. An icache invalidation touching a
  // chunk resets it to Unverified (Dolphin invalidates while *loading* code,
  // so invalidation alone must not retire coverage); a hash mismatch (real
  // SMC) marks it Failed, interpreter-only until the next invalidation.
  enum ChunkState : u8
  {
    CHUNK_UNVERIFIED = 0,
    CHUNK_VERIFIED = 1,
    CHUNK_FAILED = 2,
  };

  void OnICacheInvalidate(u32 address, u32 length);
  // The dispatch gate: one byte per chunk saying whether a dispatch into it
  // would go native right now, kept in step with every chunk state change so
  // the module can resolve its own calls and returns without asking.
  void PublishGate(bool publish);
  void RefreshChunkOpen();
  void RefreshChunkOpen(u32 index);
  int ChunkIndexOf(u32 address);
  bool IsForcedFallbackAddress(u32 address) const;
  bool ChunkContainsHostCall(u32 index) const;
  void VerifyChunk(u32 index);
  bool ResolveNativeAddress(u32 runtime_address, u32* linked_address, u32* rel_section_index);
  bool ResolveRuntimeAddress(u32 linked_address, u32* runtime_address) const;
  u32 TranslateRelAddress(u32 linked_address);
  void RefreshRelSections();

  // OS exception-vector stand-ins: run a low-RAM stub whose every word proved
  // out against the SDK template instead of single-stepping it. See
  // StaticRecompCore_Vectors.cpp for the pattern and the argument.
  bool TryVectorStub(PowerPC::PowerPCState& ppc);
  void VerifyVectorStub(u32 slot);
  void ResetVectorStubs();

  static void SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState
  void AdvanceGuestTimebase(u64 cpu_cycles);
  // Moves what the module has charged so far into Dolphin's downcount and the
  // guest timebase. Every hook calls it first, so that anything the hook
  // reads or schedules against CoreTiming's clock sees the exact moment the
  // guest is at rather than the start of the dispatch.
  void FlushGuestCharge();

  // Some SDK waits hide their hardware poll behind a call, so the frontend's
  // ordinary idle-loop analysis cannot see that the back edge has no way to
  // change the value being tested. Remember repeated identical chassis reads
  // and end the current timing slice after the wait has proved itself. The
  // generated loop guard then leaves at the loop head and polls again next
  // slice, matching DolVM's conservative poll-spin handling.
  void TrackExternalRead(u32 pc, u32 address, u64 value);
  void ResetExternalPollRun();
  void YieldToLoopGuard();

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static u32 HookSPRRead(CPUState* cpu, u16 spr, u32 cia);
  static void HookSPRWrite(CPUState* cpu, u16 spr, u32 value, u32 cia);
  static void HookCacheControl(CPUState* cpu, u8 operation, u32 ea, u32 cia);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);
  static bool HookHostCall(CPUState* cpu, u32 address);

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();

  std::unique_ptr<StaticRecompLockstep::StaticRecompLockstepVerifier> m_lockstep_verifier;

  EmptyBlockCache m_block_cache{*this};

  CPUState m_guest{};
  Common::DynamicLibrary m_library;
  StaticRecompModuleSource m_module_source;
  const StaticRecompModuleDesc* m_module = nullptr;
  bool m_module_active = false;
  u32 m_host_call_passthrough_pc = 0;
  bool m_host_call_passthrough = false;
  std::unique_ptr<JitBase> m_fallback_jit;

  u64 m_native_dispatches = 0;
  u64 m_fallback_steps = 0;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_timebase_cycle_remainder = 0;
  std::unordered_map<u32, u64> m_dispatch_samples;
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)

  static constexpr u32 POLL_SPIN_READS = 16;
  static constexpr u32 POLL_SITE_COUNT = 8;
  struct PollSite
  {
    u32 pc = 0;
    u32 address = 0;
    u64 value = 0;
    bool live = false;
  };
  PollSite m_poll_sites[POLL_SITE_COUNT]{};
  u32 m_poll_next_site = 0;
  u32 m_poll_pc = 0;
  u32 m_poll_address = 0;
  u64 m_poll_value = 0;
  u32 m_poll_run = 0;
  // Consecutive reads of the same register from the same instruction, whatever
  // they returned. The run above needs a *stable* value; this one does not, and
  // that is the difference between a heuristic and a liveness guarantee.
  static constexpr u32 POLL_SITE_SPIN_READS = 256;
  // Comfortably above DOLRECOMP_C_LOOP_CYCLE_BUDGET (256), which is the budget
  // the generated loop guards test against and which the chassis has no way to
  // read out of a module. Overshooting it only costs a spin loop a few cycles of
  // guest time it was not going to use.
  static constexpr s64 LOOP_GUARD_YIELD_CYCLES = 4096;
  u32 m_poll_site_run = 0;
  bool m_poll_run_live = false;
  bool m_poll_skip_enabled = true;
  u64 m_poll_reads = 0;
  u64 m_poll_yields = 0;
  // primary<<16 | extended, for the four primaries that have an extended field.
  bool m_fallback_histogram_enabled = false;
  std::map<u32, u64> m_fallback_histogram;

  // D4 guard state: parallel to m_module->chunk_ranges.
  std::vector<u8> m_chunk_state;
  std::vector<u8> m_chunk_open;
  StaticRecompDispatchGate m_gate{};
  bool m_gate_published = false;
  mutable std::vector<u8> m_chunk_host_call_state;
  std::vector<StaticRecompRange> m_forced_fallback_ranges;
  struct ActiveRelSection
  {
    u32 module_id;
    u32 section_index;
    u32 linked_start;
    u32 runtime_start;
    u32 size;
  };
  std::vector<ActiveRelSection> m_active_rel_sections;
  std::vector<int> m_chunk_rel_sections;
  std::vector<u64> m_effective_chunk_hashes;
  // The last range the guest invalidated inside each chunk. A chunk that fails
  // verification failed because the guest rewrote an instruction in it, and the
  // guest has to invalidate what it rewrote or the CPU would not see it -- so
  // this names the patched address, which is otherwise a day's work to find.
  struct LastInvalidation
  {
    u32 address = 0;
    u32 length = 0;
  };
  std::vector<LastInvalidation> m_chunk_last_invalidate;
  // One report per chunk. A failing chunk is re-verified on every invalidation
  // that touches it, and a game that invalidates in a loop would otherwise
  // print thousands of identical lines.
  std::vector<u8> m_chunk_reported;
  u64 m_smc_lost_bytes = 0;  // guest code the module stopped covering
  u64 m_rel_mapping_generation = 0;
  u32 m_failed_chunks = 0;    // chunks currently failing verification (real SMC)
  u64 m_verifications = 0;    // chunk hash checks performed
  u64 m_reverify_events = 0;  // invalidations that reset a chunk to Unverified

  // Lookup table optimization for O(1) chunk searches
  std::vector<int> m_chunk_lookup_table;
  u32 m_lookup_ram_size = 0;
  u32 m_lookup_exram_size = 0;
  int GetAddressLookupIndex(u32 address) const;
  void InitLookupTable(u32 ram_size, u32 exram_size);

  // Dispatch locality: most control transfers stay inside one chunk, so the
  // last hit short-circuits the chunk binary search on the hot path.
  mutable u32 m_last_chunk_index = 0;

  bool m_collect_dispatch_samples = false;
  bool m_has_rel_modules = false;
  u32 m_idle_pc = 0;

  // Vector stand-in state, one slot per 0x100 of low RAM. Verification is
  // cached until an icache invalidation over low RAM or a ClearCache (which
  // savestate loads pass through) drops it.
  enum VectorStubState : u8
  {
    VECTOR_STUB_UNKNOWN = 0,
    VECTOR_STUB_VERIFIED = 1,
    VECTOR_STUB_MISMATCH = 2,
  };
  struct VectorStubCharge
  {
    u32 cycles = 0;
    u32 load_stores = 0;
  };
  struct VectorStubSlot
  {
    bool syscall = false;
    u32 debugger_target = 0;
    VectorStubCharge charge_taken;
    VectorStubCharge charge_fallthrough;
  };
  u8 m_vector_stub_state[0x18] = {};
  VectorStubSlot m_vector_stub_slots[0x18];
  bool m_vector_stubs_enabled = true;
  u64 m_vector_stub_hits = 0;
  u64 m_vector_stub_verifies = 0;
};

extern StaticRecompCore* g_static_recomp_core;
u32 StaticRecompShouldYieldAt(u32 address);
