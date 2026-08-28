// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#endif

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);
constexpr u32 ASYNC_EXCEPTION_MASK =
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR;
constexpr u32 MSR_EE = 0x00008000u;

struct FileCloser
{
  void operator()(std::FILE* file) const
  {
    if (file)
      std::fclose(file);
  }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// One line every 2^20 dispatches is right for a running game and useless for a
// boot that hangs after a few thousand. STATICRECOMP_TRACE_EVERY makes the
// interval a power of two of the caller's choosing, which is what turns the
// trace into a "where is it spinning" tool.
u64 DispatchTraceMask()
{
  const char* every = std::getenv("STATICRECOMP_TRACE_EVERY");
  if (!every || !*every)
    return 0xFFFFFu;
  const u64 value = std::strtoull(every, nullptr, 0);
  if (value < 2)
    return 0;
  return value - 1;
}

FilePtr OpenDispatchTrace()
{
  const char* path = std::getenv("STATICRECOMP_TRACE_FILE");
  if (!path || !*path)
    return {};

  FilePtr file(std::fopen(path, "w"));
  if (file)
  {
    std::fprintf(file.get(), "dispatch,pc,lr,ctr,cr,timebase,ppc_downcount\n");
    std::fflush(file.get());
  }
  return file;
}

#if defined(__APPLE__) && defined(__aarch64__)
std::atomic<bool> s_native_sampler_running{false};
std::thread s_native_sampler_thread;
mach_port_t s_native_sampled_thread = MACH_PORT_NULL;
std::unordered_map<uintptr_t, u64> s_native_pc_samples;

const char* NativeSampleCategory(const char* symbol)
{
  if (!symbol)
    return "unknown";
  if (std::strstr(symbol, "func_"))
    return "generated";
  if (std::strstr(symbol, "ppc_f") || std::strstr(symbol, "ppc_ps") ||
      std::strstr(symbol, "psq_") || std::strstr(symbol, "ni_"))
    return "exact-fp/psq";
  if (std::strstr(symbol, "StaticRecomp") || std::strstr(symbol, "staticrecomp") ||
      std::strstr(symbol, "chassis_") || std::strstr(symbol, "dolrecomp_native_gate"))
    return "chassis";
  if (std::strstr(symbol, "Video") || std::strstr(symbol, "Vertex") ||
      std::strstr(symbol, "Texture") || std::strstr(symbol, "Renderer") ||
      std::strstr(symbol, "Fifo") || std::strstr(symbol, "FIFO") ||
      std::strstr(symbol, "Metal") || std::strstr(symbol, "Decode"))
    return "video";
  if (std::strstr(symbol, "Audio") || std::strstr(symbol, "Mixer") ||
      std::strstr(symbol, "Sound") || std::strstr(symbol, "AX"))
    return "audio";
  if (std::strstr(symbol, "semwait") || std::strstr(symbol, "semaphore") ||
      std::strstr(symbol, "swtch") || std::strstr(symbol, "mach_msg") ||
      std::strstr(symbol, "kevent"))
    return "wait/kernel";
  return "other";
}

void NativeSamplerStart()
{
  const char* enabled = std::getenv("STATICRECOMP_NATIVE_SAMPLE");
  if (!enabled || enabled[0] == '0' || s_native_sampler_running.exchange(true))
    return;

  s_native_pc_samples.clear();
  s_native_sampled_thread = mach_thread_self();
  s_native_sampler_thread = std::thread([] {
    while (s_native_sampler_running.load(std::memory_order_relaxed))
    {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      if (thread_suspend(s_native_sampled_thread) != KERN_SUCCESS)
        continue;

      arm_thread_state64_t state{};
      mach_msg_type_number_t size = ARM_THREAD_STATE64_COUNT;
      const kern_return_t rc = thread_get_state(s_native_sampled_thread, ARM_THREAD_STATE64,
                                                reinterpret_cast<thread_state_t>(&state), &size);
      thread_resume(s_native_sampled_thread);
      if (rc == KERN_SUCCESS)
        ++s_native_pc_samples[static_cast<uintptr_t>(arm_thread_state64_get_pc(state))];
    }
  });
  std::fprintf(stderr, "[native-sample] started at 200us on the CPU-GPU thread\n");
}
#else
void NativeSamplerStart()
{
}
#endif
}

void StaticRecompNativeSamplerStopAndReport()
{
#if defined(__APPLE__) && defined(__aarch64__)
  if (!s_native_sampler_running.exchange(false))
    return;
  if (s_native_sampler_thread.joinable())
    s_native_sampler_thread.join();
  if (s_native_sampled_thread != MACH_PORT_NULL)
  {
    mach_port_deallocate(mach_task_self(), s_native_sampled_thread);
    s_native_sampled_thread = MACH_PORT_NULL;
  }

  u64 total = 0;
  std::unordered_map<std::string, u64> symbols;
  std::unordered_map<std::string, u64> categories;
  std::vector<std::pair<uintptr_t, u64>> pcs;
  pcs.reserve(s_native_pc_samples.size());
  for (const auto& [pc, count] : s_native_pc_samples)
  {
    Dl_info info{};
    const char* symbol = "?";
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname)
      symbol = info.dli_sname;
    total += count;
    symbols[symbol] += count;
    categories[NativeSampleCategory(symbol)] += count;
    pcs.emplace_back(pc, count);
  }
  if (total == 0)
    return;

  const auto hottest = [](const auto& left, const auto& right) {
    return left.second > right.second;
  };
  std::vector<std::pair<std::string, u64>> sorted_categories(categories.begin(), categories.end());
  std::vector<std::pair<std::string, u64>> sorted_symbols(symbols.begin(), symbols.end());
  std::sort(sorted_categories.begin(), sorted_categories.end(), hottest);
  std::sort(sorted_symbols.begin(), sorted_symbols.end(), hottest);
  std::sort(pcs.begin(), pcs.end(), hottest);

  std::fprintf(stderr, "[native-sample] %llu CPU-GPU-thread samples\n",
               static_cast<unsigned long long>(total));
  std::fprintf(stderr, "[native-sample] by category:\n");
  for (const auto& [category, count] : sorted_categories)
    std::fprintf(stderr, "  %6.2f%%  %s\n", 100.0 * static_cast<double>(count) / total,
                 category.c_str());
  std::fprintf(stderr, "[native-sample] by symbol:\n");
  for (std::size_t i = 0; i < std::min<std::size_t>(sorted_symbols.size(), 40); ++i)
    std::fprintf(stderr, "  %6.2f%%  %s\n",
                 100.0 * static_cast<double>(sorted_symbols[i].second) / total,
                 sorted_symbols[i].first.c_str());
  std::fprintf(stderr, "[native-sample] hottest program counters:\n");
  double cumulative = 0.0;
  for (std::size_t i = 0; i < std::min<std::size_t>(pcs.size(), 80); ++i)
  {
    const auto [pc, count] = pcs[i];
    Dl_info info{};
    const char* symbol = "?";
    uintptr_t symbol_offset = 0;
    uintptr_t image_offset = 0;
    if (dladdr(reinterpret_cast<void*>(pc), &info))
    {
      if (info.dli_sname)
        symbol = info.dli_sname;
      if (info.dli_saddr)
        symbol_offset = pc - reinterpret_cast<uintptr_t>(info.dli_saddr);
      if (info.dli_fbase)
        image_offset = pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
    const double share = 100.0 * static_cast<double>(count) / total;
    cumulative += share;
    std::fprintf(stderr, "  %5.2f%% (cum %5.2f%%) %s+0x%lx [image+0x%lx]\n", share,
                 cumulative, symbol, static_cast<unsigned long>(symbol_offset),
                 static_cast<unsigned long>(image_offset));
  }
  std::fflush(stderr);
  s_native_pc_samples.clear();
#endif
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();
  FilePtr dispatch_trace = OpenDispatchTrace();
  const u64 dispatch_trace_mask = DispatchTraceMask();
  NativeSamplerStart();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  // The locked cache is memory, and games use it as their fastest scratchpad,
  // so handing the guest a pointer to it takes every access out of the hook
  // path. MMU_Tables.cpp's WriteToHardware ends in exactly this store, past a
  // page split, an address translation, a gather-pipe test and an MMIO test.
  m_guest.l1cache = getenv("MODERNGEKKO_NO_L1") ? nullptr : memory.GetL1Cache();
  m_guest.l1cache_size = memory.GetL1CacheSize();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);
  const bool lockstep_enabled = m_lockstep_verifier->IsEnabled();
  const auto fast_dispatchable_at = [this](u32 address) {
    if (m_has_rel_modules || !m_forced_fallback_ranges.empty())
      return FastDispatchableAt(address);
    if (!m_module_active || m_chunk_lookup_table.empty())
      return false;

    int lookup_index = -1;
    if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    {
      lookup_index = static_cast<int>((address - 0x80000000u) >> 2);
    }
    else if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    {
      lookup_index = static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
    }
    if (lookup_index < 0 || lookup_index >= static_cast<int>(m_chunk_lookup_table.size()))
      return false;
    const int chunk = m_chunk_lookup_table[lookup_index];
    return chunk >= 0 && m_chunk_state[chunk] == CHUNK_VERIFIED;
  };

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit && !m_guest.host_call)
  {
    m_fallback_jit->Run();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc) &&
          !(m_guest.host_call && IsHostCallAddress(ppc.pc)))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          if (dispatch_trace && (m_native_dispatches & dispatch_trace_mask) == 0)
          {
            std::fprintf(dispatch_trace.get(), "%llu,%08x,%08x,%08x,%08x,%llu,%d\n",
                         static_cast<unsigned long long>(m_native_dispatches), m_guest.pc,
                         m_guest.lr, m_guest.ctr, m_guest.cr,
                         static_cast<unsigned long long>(m_guest.timebase), ppc.downcount);
            std::fflush(dispatch_trace.get());
          }
          const bool do_ls = lockstep_enabled && m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          if (m_collect_dispatch_samples && (m_native_dispatches & 4095u) == 0)
            ++m_dispatch_samples[m_guest.pc];
          const u32 runtime_dispatch_address = m_guest.pc;
          u32 linked_dispatch_address = runtime_dispatch_address;
          if (m_has_rel_modules)
            ResolveNativeAddress(runtime_dispatch_address, &linked_dispatch_address, nullptr);
          m_guest.pc = linked_dispatch_address;
          // A gated module runs through what is left of the slice before it
          // comes back here, flushing its charge from inside any hook it
          // calls; the flush below covers whatever it charged since.
          m_module->dispatch(&m_guest, linked_dispatch_address);
          if (m_has_rel_modules)
            m_guest.pc = TranslateRelAddress(m_guest.pc);
          ++m_native_dispatches;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
          ppc.downcount -= static_cast<int>(effective_charge);
          m_charged_cycles += effective_charge;
          AdvanceGuestTimebase(effective_charge);

          // Idle loop skipping for configured target loops (e.g. Wii Menu OSIdleThread)
          if (m_guest.pc == m_idle_pc && m_idle_pc != 0)
          {
            m_system.GetCoreTiming().Idle();
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
          if ((ppc.Exceptions & ASYNC_EXCEPTION_MASK) != 0 && (m_guest.msr & MSR_EE) != 0)
            break;  // rfi/mtmsr re-enabled interrupts while one was pending.
        } while (m_module_active && fast_dispatchable_at(m_guest.pc) &&
                 !(m_guest.host_call && IsHostCallAddress(m_guest.pc)) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
        else if ((ppc.Exceptions & ASYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExternalExceptions();
      }
      else
      {
        if (m_guest.host_call && IsHostCallAddress(ppc.pc))
        {
          SyncIn();
          bool handled = m_guest.host_call(&m_guest, m_guest.pc);
          if (!handled && m_guest.pc < m_guest.ram_size)
            handled = m_guest.host_call(&m_guest, m_guest.pc | 0x80000000u);
          if (m_fallback_jit && IsHostCallAddress(m_guest.lr))
            m_fallback_jit->GetBlockCache()->InvalidateICache(m_guest.lr, 4, true);
          if (handled)
          {
            const s64 charge = -m_guest.downcount;
            m_guest.downcount = 0;
            const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
            ppc.downcount -= static_cast<int>(effective_charge);
            AdvanceGuestTimebase(effective_charge);
            SyncOut();
            continue;
          }
          SyncOut();
          if (m_fallback_jit)
          {
            m_host_call_passthrough_pc = ppc.pc;
            m_host_call_passthrough = true;
          }
        }
        // A guest exception lands here with pc at a low-RAM vector the module
        // cannot cover. If the stub there proved out against the SDK template,
        // run it whole instead of one interpreted step at a time.
        if (m_module_active && m_vector_stubs_enabled && TryVectorStub(ppc))
          continue;
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (m_module_active && IsForcedFallbackAddress(ppc.pc))
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
        }
        else if (m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else
        {
          // Where the interpreter is entered from, when it is entered at all.
          // A run of interpreted instructions belongs to whatever address the
          // module could not dispatch, so counting the first pc of each run
          // names the code the module is missing.
          if (getenv("MODERNGEKKO_FALLBACK_TRACE"))
          {
            static std::map<u32, unsigned long long> sites;
            static unsigned long long runs = 0;
            sites[ppc.pc]++;
            if (++runs % 20000ull == 0)
            {
              std::vector<std::pair<u32, unsigned long long>> v(sites.begin(),
                                                                sites.end());
              std::sort(v.begin(), v.end(),
                        [](const auto& a, const auto& b) { return a.second > b.second; });
              std::fprintf(stderr, "[fallback] %llu runs, %zu distinct sites\n",
                           runs, v.size());
              for (size_t i = 0; i < v.size() && i < 14; i++)
                std::fprintf(stderr, "[fallback]   0x%08X  %14llu\n", v[i].first,
                             v[i].second);
            }
          }
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) &&
                   !IsHostCallAddress(ppc.pc) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
