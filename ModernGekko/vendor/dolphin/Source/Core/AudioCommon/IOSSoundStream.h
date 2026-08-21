// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "AudioCommon/SoundStream.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <AudioUnit/AudioUnit.h>
#include <atomic>
#endif

// iOS output through a RemoteIO audio unit.
//
// The vendored cubeb cannot build for iOS -- its AudioUnit backend declares
// macOS-only CoreAudio device-enumeration types and constants at file scope,
// and fixing that is a large fork of a third-party library. iOS needs none of
// that machinery anyway: there is one output device, the system owns it, and
// the app never enumerates or switches it. So this talks to RemoteIO directly,
// which is a few dozen lines and the same path cubeb would take underneath.
class IOSSoundStream final : public SoundStream
{
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
public:
  ~IOSSoundStream() override;
  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;
  static bool IsValid() { return true; }

private:
  static OSStatus RenderCallback(void* context, AudioUnitRenderActionFlags* flags,
                                 const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                                 AudioBufferList* data);

  AudioUnit m_unit = nullptr;
  bool m_running = false;
  // Read on the render thread, written from the emulation thread.
  std::atomic<int> m_volume{100};
#endif
};
