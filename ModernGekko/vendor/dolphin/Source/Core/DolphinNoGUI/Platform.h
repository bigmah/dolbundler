// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "Common/Flag.h"
#include "Common/WindowSystemInfo.h"

class Platform
{
public:
  virtual ~Platform();

  bool IsRunning() const { return m_running.IsSet(); }
  bool IsWindowFocused() const { return m_window_focus; }
  bool IsWindowFullscreen() const { return m_window_fullscreen; }

  virtual bool Init();
  virtual void SetTitle(const std::string& title);
  virtual void MainLoop() = 0;
  virtual void SaveWindowGeometry() {}

  virtual WindowSystemInfo GetWindowSystemInfo() const = 0;

  // Requests a graceful shutdown, from SIGINT/SIGTERM.
  void RequestShutdown();

  // Requests a slot-1 savestate save/load, from SIGUSR1/SIGUSR2 (nogui has no
  // hotkey scheduler, so signals are the external savestate trigger).
  void RequestSaveState();
  void RequestLoadState();

  // Request an immediate shutdown.
  void Stop();

  static std::unique_ptr<Platform> CreateHeadlessPlatform();
#ifdef HAVE_WAYLAND
  static std::unique_ptr<Platform> CreateWaylandPlatform();
#endif
#ifdef HAVE_X11
  static std::unique_ptr<Platform> CreateX11Platform();
#endif

#ifdef __linux__
  static std::unique_ptr<Platform> CreateFBDevPlatform();
#endif

#ifdef _WIN32
  static std::unique_ptr<Platform> CreateWin32Platform();
#endif

#ifdef __APPLE__
  static std::unique_ptr<Platform> CreateMacOSPlatform();
#endif

#ifdef MODERNGEKKO_HAVE_UIKIT
  // The layer is created and owned by the host app; the runtime only draws
  // into it. Must be set before Init().
  static void SetIOSRenderLayer(void* ca_metal_layer, float scale);
  // Optional trace file. Boot failures on iOS are SIGKILLs with no crash
  // report, so the platform writes its own breadcrumbs when this is set.
  static void SetIOSDiagnosticLog(const char* path);
  // Writes one line to that file, flushed. Safe to call from any thread and
  // before/after the platform exists. Used to trace the boot on iOS, where a
  // failure is a SIGKILL and Dolphin's own log stops being written.
  static void IOSLog(const char* message);
  static std::unique_ptr<Platform> CreateIOSPlatform();
#endif

protected:
  void UpdateRunningFlag();

  Common::Flag m_running{true};
  Common::Flag m_shutdown_requested{false};
  Common::Flag m_tried_graceful_shutdown{false};
  Common::Flag m_save_state_requested{false};
  Common::Flag m_load_state_requested{false};

  bool m_window_focus = true;  // Should be made atomic if actually implemented
  bool m_window_fullscreen = false;
};
