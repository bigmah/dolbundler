// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>

#include "Common/CommonTypes.h"

namespace Core
{
// Where the CPU thread's wall time goes, for the [perf] line.
//
// A speed figure says the thread is not keeping up; it does not say whether
// the thread is running recompiled code, servicing the scheduled events
// (CoreTiming::Advance -- DSP HLE, VI, SI, the audio DMA, and the throttle
// sleep), or standing still waiting for the GPU thread to answer an EFB peek.
// On a phone nothing outside the emulator can profile that thread, so it
// accounts for itself: three buckets, one clock read per slice, opt-in
// (DOLWEB_CPU_TIME=1) because on the phone a clock read is not free.
// Whatever is not in a bucket is the recompiled code and the chassis.
struct CPUThreadClock
{
  static inline bool enabled = false;
  static inline std::atomic<u64> events_ns{0};    // CoreTiming::Advance, throttle included
  static inline std::atomic<u64> throttle_ns{0};  // the sleep inside it
  static inline std::atomic<u64> gpu_wait_ns{0};  // blocking waits on the GPU thread
  static inline std::atomic<u64> gpu_waits{0};
  // How often the recompiled code leaves for a hardware access: every one is
  // a hook call from the memory slow path. The gather pipe is counted on its
  // own because a GX vertex component is one of these each.
  static inline std::atomic<u64> ext_reads{0};
  static inline std::atomic<u64> ext_writes{0};
  static inline std::atomic<u64> gather_writes{0};

  class Scope
  {
  public:
    explicit Scope(std::atomic<u64>& bucket, std::atomic<u64>* count = nullptr)
        : m_bucket(enabled ? &bucket : nullptr), m_count(count)
    {
      if (m_bucket)
        m_start = std::chrono::steady_clock::now();
    }
    ~Scope()
    {
      if (!m_bucket)
        return;
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - m_start)
                          .count();
      m_bucket->fetch_add(static_cast<u64>(ns), std::memory_order_relaxed);
      if (m_count)
        m_count->fetch_add(1, std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    std::atomic<u64>* m_bucket;
    std::atomic<u64>* m_count;
    std::chrono::steady_clock::time_point m_start;
  };
};
}  // namespace Core
