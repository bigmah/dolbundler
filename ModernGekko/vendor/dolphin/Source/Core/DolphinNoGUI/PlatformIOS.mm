// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The iOS counterpart to PlatformMacos.mm, and deliberately much smaller.
//
// On macOS the runtime owns the window: it creates an NSWindow, runs an
// NSApplication event loop, and hands its content view to the video backend.
// None of that is possible on iOS. UIKit owns the run loop, a UIWindow can
// only be created on the main thread, and the runtime's MainLoop() is called
// on a worker thread so the UI stays responsive.
//
// So the ownership is inverted: the host app creates a UIView backed by a
// CAMetalLayer, hands the layer over with SetIOSRenderLayer(), and this
// platform only reports it and pumps Dolphin's host job queue.

#include "DolphinNoGUI/Platform.h"

#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/Present.h"

#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
// Set once by the app before the runtime starts. Retained deliberately: the
// layer has to outlive the emulation thread, which does not end when the view
// controller is dismissed.
CAMetalLayer* s_render_layer = nil;
// Retina scale of the layer. Dolphin reads the drawable size off the layer
// itself, so this is the only geometry the platform has to report.
std::atomic<float> s_layer_scale{1.0f};

class PlatformIOS : public Platform
{
public:
  bool Init() override;
  void SetTitle(const std::string& title) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;
};

bool PlatformIOS::Init()
{
  if (s_render_layer == nil)
    return false;

  // There is no window to focus or unfocus: while the emulation thread runs,
  // the app is by definition in the foreground, and iOS suspends it otherwise.
  m_window_focus = true;
  m_window_fullscreen = true;
  return true;
}

void PlatformIOS::SetTitle(const std::string& title)
{
  // Nothing on iOS displays a window title. The app shows the game name in its
  // own chrome, from the library entry, before the runtime ever starts.
}

void PlatformIOS::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());

    // Emulation runs on Dolphin's own CPU thread and drawing is driven by the
    // layer, so this loop exists only to service host jobs. Sleeping keeps it
    // off a core that the interpreter needs; on a phone that is the difference
    // between a stable frame rate and a thermal cliff.
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
}

WindowSystemInfo PlatformIOS::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::iOS;
  // The Metal backend's PrepareWindow() is compiled out on iOS, so unlike the
  // macOS path this has to already be the layer rather than a view to wrap.
  wsi.render_window = (__bridge void*)s_render_layer;
  wsi.render_surface = wsi.render_window;
  wsi.render_surface_scale = s_layer_scale.load(std::memory_order_relaxed);
  return wsi;
}
}  // namespace

void Platform::SetIOSRenderLayer(void* ca_metal_layer, float scale)
{
  s_render_layer = (__bridge CAMetalLayer*)ca_metal_layer;
  s_layer_scale.store(scale, std::memory_order_relaxed);
}

std::unique_ptr<Platform> Platform::CreateIOSPlatform()
{
  return std::make_unique<PlatformIOS>();
}
