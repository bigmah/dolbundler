// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The browser owns the canvas; the runtime only draws into it. That is the
// same inversion PlatformIOS.mm describes -- the host creates the surface and
// hands it over before Init() -- and for the same reason: there is no window
// system here for Dolphin to open a window with.

#include <chrono>
#include <string>
#include <thread>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"

namespace
{
// A CSS selector, not a handle. Set before Init(); "#canvas" is emscripten's
// own default and is what the page uses unless it says otherwise.
std::string s_canvas_selector = "#canvas";

class PlatformEmscripten final : public Platform
{
public:
  bool Init() override;
  void SetTitle(const std::string& title) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;
};

bool PlatformEmscripten::Init()
{
  return true;
}

void PlatformEmscripten::SetTitle(const std::string& title)
{
  // The page decides what to do with it; document.title is the obvious choice
  // but a canvas embedded in a larger UI may want it elsewhere.
  EM_ASM({ if (Module['onTitle']) Module['onTitle'](UTF8ToString($0)); }, title.c_str());
}

void PlatformEmscripten::MainLoop()
{
  // main() lives on a proxied pthread, so blocking here is allowed and is what
  // every other platform does. The browser's own event loop is unaffected.
  while (m_running.IsSet())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

WindowSystemInfo PlatformEmscripten::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Emscripten;
  wsi.display_connection = nullptr;
  wsi.render_window = const_cast<char*>(s_canvas_selector.c_str());
  wsi.render_surface = const_cast<char*>(s_canvas_selector.c_str());
  double width = 0.0;
  double height = 0.0;
  if (emscripten_get_element_css_size(s_canvas_selector.c_str(), &width, &height) ==
      EMSCRIPTEN_RESULT_SUCCESS)
  {
    wsi.render_surface_scale = static_cast<float>(emscripten_get_device_pixel_ratio());
  }
  return wsi;
}
}  // namespace

void Platform::SetEmscriptenCanvas(const char* css_selector)
{
  if (css_selector && css_selector[0])
    s_canvas_selector = css_selector;
}

std::unique_ptr<Platform> Platform::CreateEmscriptenPlatform()
{
  return std::make_unique<PlatformEmscripten>();
}
