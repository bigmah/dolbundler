// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/WebSoundStream.h"

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <chrono>

#include <emscripten/emscripten.h>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"

namespace
{
WebSoundStream* s_instance = nullptr;
}  // namespace

// The page needs three numbers to build its views over this: where the ring is,
// how big it is, and where the two indices live. Exported rather than passed
// through a message because the worklet is created before the emulator is, and
// because a pointer that arrives by value cannot be wrong later.
extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t dolweb_audio_ring_ptr();
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t dolweb_audio_ring_frames();
extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t dolweb_audio_read_ptr();
extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t dolweb_audio_write_ptr();
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t dolweb_audio_sample_rate();

WebSoundStream::~WebSoundStream()
{
  m_stop.store(true, std::memory_order_relaxed);
  if (m_producer.joinable())
    m_producer.join();
  if (s_instance == this)
    s_instance = nullptr;
}

bool WebSoundStream::Init()
{
  m_ring.assign(static_cast<size_t>(RING_FRAMES) * 2, 0);
  m_read.store(0, std::memory_order_relaxed);
  m_write.store(0, std::memory_order_relaxed);
  s_instance = this;
  m_producer = std::thread([this] { Produce(); });
  NOTICE_LOG_FMT(AUDIO, "WebAudio: ring of {} frames at {} Hz", RING_FRAMES,
                 m_mixer->GetSampleRate());
  return true;
}

bool WebSoundStream::SetRunning(bool running)
{
  NOTICE_LOG_FMT(AUDIO, "WebAudio: running = {}", running);
  m_running.store(running, std::memory_order_relaxed);
  return true;
}

void WebSoundStream::SetVolume(int volume)
{
  m_volume.store(volume, std::memory_order_relaxed);
}

// A producer thread rather than a callback, and the pacing is the ring's own
// fill level: keeping it about half full is what absorbs the difference between
// the emulator's frame loop, which delivers audio in bursts, and the browser's
// audio clock, which consumes it evenly.
void WebSoundStream::Produce()
{
  Common::SetCurrentThreadName("WebAudio");
  constexpr uint32_t kChunkFrames = 512;
  std::vector<int16_t> chunk(kChunkFrames * 2);

  while (!m_stop.load(std::memory_order_relaxed))
  {
    if (!m_running.load(std::memory_order_relaxed))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      continue;
    }

    const uint32_t write = m_write.load(std::memory_order_relaxed);
    const uint32_t read = m_read.load(std::memory_order_acquire);
    const uint32_t queued = write - read;  // unsigned wraparound is the point
    if (queued >= RING_FRAMES / 2)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    const uint32_t want = std::min(kChunkFrames, RING_FRAMES / 2 - queued);
    const size_t filled = m_mixer->Mix(chunk.data(), want);
    // Two counters, because "no sound" has two very different causes and they
    // look identical from the page: a mixer with nothing in it, and a producer
    // that is never asked.
    static uint64_t s_empty = 0;
    static uint64_t s_frames = 0;
    if (filled == 0)
    {
      if ((++s_empty % 2000) == 0)
        NOTICE_LOG_FMT(AUDIO, "WebAudio: mixer empty {} times, {} frames so far", s_empty,
                       s_frames);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    s_frames += filled;

    const int volume = m_volume.load(std::memory_order_relaxed);
    if (volume < 100)
    {
      for (size_t i = 0; i < filled * 2; ++i)
        chunk[i] = static_cast<int16_t>(chunk[i] * volume / 100);
    }

    for (size_t frame = 0; frame < filled; ++frame)
    {
      const size_t slot = static_cast<size_t>((write + frame) % RING_FRAMES) * 2;
      m_ring[slot] = chunk[frame * 2];
      m_ring[slot + 1] = chunk[frame * 2 + 1];
    }
    m_write.store(write + static_cast<uint32_t>(filled), std::memory_order_release);
  }
}

extern "C" {
uintptr_t dolweb_audio_ring_ptr()
{
  return s_instance ? s_instance->RingAddress() : 0;
}
uint32_t dolweb_audio_ring_frames()
{
  return WebSoundStream::RING_FRAMES;
}
uintptr_t dolweb_audio_read_ptr()
{
  return s_instance ? s_instance->ReadIndexAddress() : 0;
}
uintptr_t dolweb_audio_write_ptr()
{
  return s_instance ? s_instance->WriteIndexAddress() : 0;
}
uint32_t dolweb_audio_sample_rate()
{
  return s_instance && s_instance->GetMixer() ? s_instance->GetMixer()->GetSampleRate() : 48000;
}
}

#endif  // __EMSCRIPTEN__
