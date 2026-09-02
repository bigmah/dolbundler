// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>
#include <functional>
#include <future>

#include "Common/Functional.h"
#include "Common/SPSCQueue.h"
#include "Core/CPUThreadClock.h"

struct EfbPokeData;
class PointerWrap;

class AsyncRequests
{
public:
  AsyncRequests();

  // Called from the Video thread.
  // Returns whether anything was pulled: the GPU thread's busy accounting
  // wants to know, because the present arrives this way.
  bool PullEvents();
  bool IsQueueEmpty() const { return m_queue.Empty(); }

  // The following are called from the CPU thread.
  void WaitForEmptyQueue();

  template <std::invocable<> F>
  void PushEvent(F&& callback)
  {
    if (m_passthrough)
    {
      std::invoke(std::forward<F>(callback));
      return;
    }

    QueueEvent(Event{std::forward<F>(callback)});
  }

  template <std::invocable<> F>
  auto PushBlockingEvent(F&& callback) -> std::invoke_result_t<F>
  {
    if (m_passthrough)
      return std::invoke(std::forward<F>(callback));

    std::packaged_task task{std::forward<F>(callback)};
    QueueEvent(Event{[&] { task(); }});

    // An EFB peek from the CPU thread stops here until the GPU thread has
    // flushed its draws and read the pixel back, which on a phone is a
    // proxied readback through the browser's GPU process. Accounted as a
    // GPU wait so the perf line can say how much of the CPU thread it costs.
    const Core::CPUThreadClock::Scope clock_scope(Core::CPUThreadClock::gpu_wait_ns,
                                                  &Core::CPUThreadClock::gpu_waits);
    return task.get_future().get();
  }

  // Not thread-safe. Only set during initialization.
  void SetPassthrough(bool enable);

  static AsyncRequests* GetInstance() { return &s_singleton; }

private:
  using Event = Common::MoveOnlyFunction<void()>;

  void QueueEvent(Event&& event);

  static AsyncRequests s_singleton;

  Common::WaitableSPSCQueue<Event> m_queue;

  bool m_passthrough = true;
};
