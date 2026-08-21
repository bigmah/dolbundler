// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/IOSSoundStream.h"

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <cstring>

#include "Common/Logging/Log.h"

namespace
{
constexpr int SAMPLE_RATE = 48000;  // what Mixer is constructed with
constexpr int CHANNELS = 2;
}  // namespace

OSStatus IOSSoundStream::RenderCallback(void* context, AudioUnitRenderActionFlags* flags,
                                        const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                                        AudioBufferList* data)
{
  auto* self = static_cast<IOSSoundStream*>(context);
  auto* out = static_cast<s16*>(data->mBuffers[0].mData);

  // Mix() fills interleaved stereo and returns how many frames it actually
  // had. Anything it could not fill has to be silenced rather than left as
  // whatever the buffer held, or the shortfall is audible as a buzz.
  const std::size_t filled = self->m_mixer->Mix(out, frames);
  if (filled < frames)
    std::memset(out + filled * CHANNELS, 0, (frames - filled) * CHANNELS * sizeof(s16));

  // Applied here rather than through a mixer unit: RemoteIO has no volume
  // parameter of its own, and adding an AUGraph for one control is not worth
  // the extra node.
  const int volume = self->m_volume.load(std::memory_order_relaxed);
  if (volume < 100)
  {
    for (UInt32 i = 0; i < frames * CHANNELS; ++i)
      out[i] = static_cast<s16>(out[i] * volume / 100);
  }

  data->mBuffers[0].mDataByteSize = frames * CHANNELS * sizeof(s16);
  return noErr;
}

bool IOSSoundStream::Init()
{
  // Playback, not ambient: a game's audio should keep going with the ringer
  // switched off, which is what the silent switch would otherwise do.
  NSError* session_error = nil;
  AVAudioSession* session = AVAudioSession.sharedInstance;
  if (![session setCategory:AVAudioSessionCategoryPlayback error:&session_error])
  {
    ERROR_LOG_FMT(AUDIO, "AVAudioSession setCategory failed: {}",
                  session_error.localizedDescription.UTF8String);
    return false;
  }
  if (![session setActive:YES error:&session_error])
  {
    ERROR_LOG_FMT(AUDIO, "AVAudioSession setActive failed: {}",
                  session_error.localizedDescription.UTF8String);
    return false;
  }

  AudioComponentDescription desc = {};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_RemoteIO;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent component = AudioComponentFindNext(nullptr, &desc);
  if (!component)
  {
    ERROR_LOG_FMT(AUDIO, "No RemoteIO audio component");
    return false;
  }
  if (AudioComponentInstanceNew(component, &m_unit) != noErr || !m_unit)
  {
    ERROR_LOG_FMT(AUDIO, "Could not instantiate the RemoteIO audio unit");
    return false;
  }

  AudioStreamBasicDescription format = {};
  format.mSampleRate = SAMPLE_RATE;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  format.mChannelsPerFrame = CHANNELS;
  format.mBitsPerChannel = 16;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = CHANNELS * sizeof(s16);
  format.mBytesPerPacket = format.mBytesPerFrame;

  // Bus 0 is the one that reaches the speaker; its input scope is what this
  // app writes into.
  if (AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                           &format, sizeof(format)) != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "Could not set the RemoteIO stream format");
    return false;
  }

  AURenderCallbackStruct callback = {};
  callback.inputProc = &IOSSoundStream::RenderCallback;
  callback.inputProcRefCon = this;
  if (AudioUnitSetProperty(m_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                           &callback, sizeof(callback)) != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "Could not install the RemoteIO render callback");
    return false;
  }

  if (AudioUnitInitialize(m_unit) != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "AudioUnitInitialize failed");
    return false;
  }

  INFO_LOG_FMT(AUDIO, "RemoteIO output ready at {} Hz", SAMPLE_RATE);
  return true;
}

bool IOSSoundStream::SetRunning(bool running)
{
  if (!m_unit || running == m_running)
    return true;

  const OSStatus result = running ? AudioOutputUnitStart(m_unit) : AudioOutputUnitStop(m_unit);
  if (result != noErr)
  {
    ERROR_LOG_FMT(AUDIO, "AudioOutputUnit{} failed: {}", running ? "Start" : "Stop",
                  static_cast<int>(result));
    return false;
  }

  m_running = running;
  return true;
}

void IOSSoundStream::SetVolume(int volume)
{
  m_volume.store(std::clamp(volume, 0, 100), std::memory_order_relaxed);
}

IOSSoundStream::~IOSSoundStream()
{
  if (!m_unit)
    return;

  SetRunning(false);
  AudioUnitUninitialize(m_unit);
  AudioComponentInstanceDispose(m_unit);
  m_unit = nullptr;

  // Handing the session back matters: leaving it active keeps other audio
  // ducked after the game stops.
  [AVAudioSession.sharedInstance setActive:NO
                               withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                     error:nil];
}

#endif  // TARGET_OS_IPHONE
