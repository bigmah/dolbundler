// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <optional>

#include "Common/BlockingLoop.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/Flag.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}

namespace Fifo
{
// Used for diagnostics.
enum class SyncGPUReason
{
  Other,
  Wraparound,
  EFBPoke,
  PerfQuery,
  BBox,
  Swap,
  AuxSpace,
};

class FifoManager final
{
public:
  explicit FifoManager(Core::System& system);
  FifoManager(const FifoManager& other) = delete;
  FifoManager(FifoManager&& other) = delete;
  FifoManager& operator=(const FifoManager& other) = delete;
  FifoManager& operator=(FifoManager&& other) = delete;
  ~FifoManager();

  void Init();
  void Shutdown();
  void Prepare();  // Must be called from the CPU thread.
  void DoState(PointerWrap& f);
  void PauseAndLock();
  void RestoreState(bool was_running);
  void UpdateWantDeterminism(bool want);
  bool UseDeterministicGPUThread() const { return m_use_deterministic_gpu_thread; }
  bool UseSyncGPU() const { return m_config_sync_gpu; }

  // In deterministic GPU thread mode this waits for the GPU to be done with pending work.
  void SyncGPU(SyncGPUReason reason, bool may_move_read_ptr = true);

  // In single core mode, this runs the GPU for a single slice.
  // In dual core mode, this synchronizes with the GPU thread.
  void SyncGPUForRegisterAccess();

  void PushFifoAuxBuffer(const void* ptr, size_t size);
  void* PopFifoAuxBuffer(size_t size);

  void FlushGpu();
  void RunGpu();
  void GpuMaySleep();
  void RunGpuLoop();
  void ExitGpuLoop();
  void EmulatorState(bool running);
  // Nanoseconds the GPU thread has spent in its loop body doing work, in
  // total. A reader differences two samples against wall time to get the
  // thread's busy fraction -- the number that says whether the frame rate is
  // waiting on the CPU thread or on this one, which no speed figure can.
  u64 GetGpuBusyNs() const { return m_gpu_busy_ns.load(std::memory_order_relaxed); }
  void ResetVideoBuffer();

private:
  void RefreshConfig();
  // size defaults to one gather-pipe block (GPFifo::GATHER_PIPE_SIZE, not
  // visible here); the GPU thread passes a batch of them.
  void ReadDataFromFifo(u32 read_ptr, u32 size = 32);
  u32 BatchableFifoBlocks(u32 read_ptr) const;
  void ReadDataFromFifoOnCPU(u32 read_ptr);
  int RunGpuOnCpu(int ticks);
  int WaitForGpuThread(int ticks);
  static void SyncGPUCallback(Core::System& system, u64 ticks, s64 cyclesLate);

  static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;

  Common::BlockingLoop m_gpu_mainloop;
  std::atomic<u64> m_gpu_busy_ns{0};
  // How many empty payloads the GPU thread spins through before it lets
  // itself sleep (DOLWEB_GPU_IDLE_SPIN); negative keeps Dolphin's behaviour.
  int m_idle_spin_limit = -1;
  int m_idle_payloads = 0;

  Common::Flag m_emu_running_state;

  // Most of this array is unlikely to be faulted in...
  u8 m_fifo_aux_data[FIFO_SIZE]{};
  u8* m_fifo_aux_write_ptr = nullptr;
  u8* m_fifo_aux_read_ptr = nullptr;

  // This could be in SConfig, but it depends on multiple settings
  // and can change at runtime.
  bool m_use_deterministic_gpu_thread = false;

  CoreTiming::EventType* m_event_sync_gpu = nullptr;

  // STATE_TO_SAVE
  u8* m_video_buffer = nullptr;
  u8* m_video_buffer_read_ptr = nullptr;
  std::atomic<u8*> m_video_buffer_write_ptr = nullptr;
  std::atomic<u8*> m_video_buffer_seen_ptr = nullptr;
  u8* m_video_buffer_pp_read_ptr = nullptr;
  // The read_ptr is always owned by the GPU thread.  In normal mode, so is the
  // write_ptr, despite it being atomic.  In deterministic GPU thread mode,
  // things get a bit more complicated:
  // - The seen_ptr is written by the GPU thread, and points to what it's already
  // processed as much of as possible - in the case of a partial command which
  // caused it to stop, not the same as the read ptr.  It's written by the GPU,
  // under the lock, and updating the cond.
  // - The write_ptr is written by the CPU thread after it copies data from the
  // FIFO.  Maybe someday it will be under the lock.  For now, because RunGpuLoop
  // polls, it's just atomic.
  // - The pp_read_ptr is the CPU preprocessing version of the read_ptr.

  std::atomic<int> m_sync_ticks = 0;
  bool m_syncing_suspended = false;
  Common::Event m_sync_wakeup_event;

  std::optional<Config::ConfigChangedCallbackID> m_config_callback_id = std::nullopt;
  bool m_config_sync_gpu = false;
  int m_config_sync_gpu_max_distance = 0;
  int m_config_sync_gpu_min_distance = 0;
  float m_config_sync_gpu_overclock = 0.0f;

  Core::System& m_system;
};

bool AtBreakpoint(Core::System& system);
}  // namespace Fifo
