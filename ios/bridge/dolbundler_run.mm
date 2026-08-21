// SPDX-License-Identifier: GPL-3.0-or-later

#include "dolbundler_run.h"

#include "DolphinNoGUI/Platform.h"
#include "InputCommon/ControllerInterface/Touch/InputOverrider.h"
#include "moderngekko/runtime.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace
{
// Guards m_runtime against a stop arriving from the UI thread while the
// emulation thread is still inside Create().
std::mutex s_runtime_mutex;
std::unique_ptr<moderngekko::Runtime> s_runtime;
std::atomic<bool> s_running{false};

// Pad 0 is the only one the on-screen controls drive. A physical controller
// paired over Bluetooth arrives through the SDL backend as its own device and
// does not need an overrider.
constexpr int kTouchPadIndex = 0;

void set_err(char* err, size_t err_size, const std::string& message)
{
  if (err && err_size)
    snprintf(err, err_size, "%s", message.c_str());
}
}  // namespace

void db_set_render_layer(void* ca_metal_layer, float scale)
{
  Platform::SetIOSRenderLayer(ca_metal_layer, scale);
}

int db_run_game(const char* game_root, const char* module_path, const char* user_dir,
                const char* title, char* err, size_t err_size)
{
  if (s_running.exchange(true))
  {
    set_err(err, err_size, "A game is already running.");
    return 0;
  }

  moderngekko::RuntimeConfig config;
  config.game_root = game_root;
  config.user_directory = user_dir;
  config.module = moderngekko::ModuleSource::BytecodePath(module_path);
  config.graphics.backend = "Metal";
  // No cubeb on iOS, so there is no device-backed sound stream to ask for.
  // AudioCommon falls back to its null stream when the backend is unknown.
  config.audio.backend = "";
  config.window_title = title ? std::string(title) : std::string();
  config.fullscreen = true;
  // A .dvm covers the whole title; letting the chassis fall back to its own
  // interpreter would silently mask a module that failed to load.
  config.allow_interpreter = false;
  config.show_fps_in_title = false;

  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created)
  {
    set_err(err, err_size,
            created.error ? created.error->message : std::string("the runtime failed to start"));
    s_running.store(false);
    return 0;
  }

  {
    std::lock_guard<std::mutex> lock(s_runtime_mutex);
    s_runtime = std::move(created.runtime);
  }

  // Registered after the core exists, because it reaches into the emulated
  // pad, and unregistered before the runtime is torn down.
  ciface::Touch::RegisterGameCubeInputOverrider(kTouchPadIndex);

  const moderngekko::RuntimeRunResult result = s_runtime->Run();

  ciface::Touch::UnregisterGameCubeInputOverrider(kTouchPadIndex);

  {
    std::lock_guard<std::mutex> lock(s_runtime_mutex);
    s_runtime.reset();
  }
  s_running.store(false);

  if (result.reason == moderngekko::RuntimeExitReason::BootFailed)
  {
    set_err(err, err_size,
            result.error ? result.error->message : std::string("the game failed to boot"));
    return 0;
  }
  return 1;
}

void db_request_stop(void)
{
  std::lock_guard<std::mutex> lock(s_runtime_mutex);
  if (s_runtime)
    s_runtime->RequestStop();
}

int db_is_running(void)
{
  return s_running.load() ? 1 : 0;
}

void db_set_control(DBPadControl control, double state)
{
  ciface::Touch::SetControlState(kTouchPadIndex, static_cast<ciface::Touch::ControlID>(control),
                                 state);
}

void db_clear_control(DBPadControl control)
{
  ciface::Touch::ClearControlState(kTouchPadIndex,
                                   static_cast<ciface::Touch::ControlID>(control));
}
