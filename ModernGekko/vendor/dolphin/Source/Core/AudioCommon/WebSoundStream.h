// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "AudioCommon/SoundStream.h"

#ifdef __EMSCRIPTEN__
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#endif

// Output through the browser's audio clock.
//
// Every other backend here is handed a callback on an audio thread and fills it
// from the mixer. A browser will not do that: the thread that owns the audio
// clock is an AudioWorklet, which is JavaScript, on the page's main thread's
// realm -- not somewhere this code can be called from. So the direction is
// inverted. This fills a ring buffer in linear memory, the worklet reads it, and
// the two agree through the indices at the top of that buffer.
//
// Everything about the ring is deliberately simple enough to reimplement in
// twenty lines of JavaScript, because it has to be: the reader is written twice,
// once here and once in dolweb-audio.js, and the two must not drift.
class WebSoundStream final : public SoundStream
{
#ifdef __EMSCRIPTEN__
public:
  ~WebSoundStream() override;
  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;
  static bool IsValid() { return true; }

  // Interleaved stereo s16 at the mixer's rate. Frames, not samples.
  static constexpr uint32_t RING_FRAMES = 1u << 14;  // 16384, ~340 ms at 48 kHz

  // Addresses, because the reader is JavaScript and builds typed-array views
  // over this module's memory rather than calling into it.
  uintptr_t RingAddress() const { return reinterpret_cast<uintptr_t>(m_ring.data()); }
  uintptr_t ReadIndexAddress() { return reinterpret_cast<uintptr_t>(&m_read); }
  uintptr_t WriteIndexAddress() { return reinterpret_cast<uintptr_t>(&m_write); }

private:
  void Produce();

  std::vector<int16_t> m_ring;
  // Shared with the worklet, which reads them with Atomics on the same memory.
  std::atomic<uint32_t> m_read{0};
  std::atomic<uint32_t> m_write{0};
  std::atomic<int> m_volume{100};
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stop{false};
  std::thread m_producer;
#endif
};
