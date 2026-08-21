// SPDX-License-Identifier: GPL-3.0-or-later

#include "dolbundler_run.h"

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "DolphinNoGUI/Platform.h"
#include "InputCommon/ControllerInterface/Touch/InputOverrider.h"
#include "InputCommon/InputConfig.h"
#include "moderngekko/runtime.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mach/mach.h>
#include <sys/stat.h>
#include <memory>
#include <mutex>
#include <string>

namespace
{
// A breadcrumb trail written straight to disk and flushed every line.
//
// iOS kills an app with SIGKILL for jetsam and watchdog timeouts, which no
// handler can catch and which leaves no crash report worth reading. stdout does
// not survive either. A file in the container is the only thing that is still
// there afterwards, so every step of the boot writes one line plus the process
// footprint at that moment -- if the kill is jetsam, the trail shows it
// climbing.
std::string s_log_path;

void run_log(const char* fmt, ...)
{
  if (s_log_path.empty())
    return;
  FILE* f = fopen(s_log_path.c_str(), "a");
  if (!f)
    return;

  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  double footprint_mb = 0.0;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
    footprint_mb = (double)info.phys_footprint / (1024.0 * 1024.0);

  char stamp[32];
  const time_t now = time(nullptr);
  strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&now));
  fprintf(f, "%s [%7.1f MB] ", stamp, footprint_mb);

  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);

  fputc('\n', f);
  fflush(f);
  fclose(f);
}

// Guards m_runtime against a stop arriving from the UI thread while the
// emulation thread is still inside Create().
std::mutex s_runtime_mutex;
std::unique_ptr<moderngekko::Runtime> s_runtime;
std::atomic<bool> s_running{false};

// Pad 0 is the only one the on-screen controls drive. A physical controller
// paired over Bluetooth arrives through the SDL backend as its own device and
// does not need an overrider.
constexpr int kTouchPadIndex = 0;

// The overrider can only be attached once the emulated pads exist, which does
// not happen until Pad::LoadConfig() runs inside BootCore(). Attaching earlier
// indexes an empty vector through InputConfig::GetController()'s .at(), which
// is a hard crash rather than an error. So registration waits for the core to
// report Running, and every touch is dropped until then.
std::atomic<bool> s_overrider_ready{false};
Common::EventHook s_state_hook;

void AttachOverriderWhenPadsExist(Core::State state)
{
  run_log("core state -> %d (pads: %d)", (int)state, Pad::GetConfig()->GetControllerCount());
  if (state == Core::State::Uninitialized || state == Core::State::Stopping)
    return;
  if (s_overrider_ready.load())
    return;
  if (Pad::GetConfig()->GetControllerCount() <= kTouchPadIndex)
    return;

  ciface::Touch::RegisterGameCubeInputOverrider(kTouchPadIndex);
  s_overrider_ready.store(true);
}

// Turn Dolphin's file log on directly, after UICommon::Init() has built the
// LogManager. Writing Logger.ini instead does not work: the config layers are
// assembled during Create(), and the file is read too early in that sequence
// to take effect for the boot we care about.
//
// This exists because a boot failure on iOS is a SIGKILL -- no crash report,
// no stdout, nothing. Dolphin's own log is the only account of what the video
// backend and the core were doing.
void ForceDolphinFileLog()
{
  // Off unless asked for. At LDEBUG Dolphin logs a line per DVD read, which is
  // a real drag on frame rate and fills the container -- fine while hunting a
  // boot failure, not something to ship enabled.
  const char* wanted = getenv("DOLBUNDLER_DEBUG_LOG");
  if (!wanted || wanted[0] != '1')
    return;

  using namespace Common::Log;
  LogManager* manager = LogManager::GetInstance();
  if (!manager)
  {
    run_log("no LogManager, cannot enable dolphin.log");
    return;
  }

  manager->SetConfigLogLevel(LogLevel::LDEBUG);
  for (int i = 0; i < static_cast<int>(LogType::NUMBER_OF_LOGS); ++i)
    manager->SetEnable(static_cast<LogType>(i), true);
  manager->EnableListener(LogListener::FILE_LISTENER, true);
  run_log("dolphin.log enabled at LDEBUG");
}

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

  s_log_path = std::string(user_dir) + "/dolbundler-run.log";
  run_log("---- run: %s", title ? title : "(untitled)");
  run_log("game_root:  %s", game_root);
  run_log("module:     %s", module_path);
  mkdir((std::string(user_dir) + "/Logs").c_str(), 0755);
  Platform::SetIOSDiagnosticLog(s_log_path.c_str());

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

  run_log("Runtime::Create ...");
  auto created = moderngekko::Runtime::Create(std::move(config));
  run_log("Runtime::Create returned");
  ForceDolphinFileLog();
  if (!created)
  {
    run_log("Runtime::Create FAILED: %s",
            created.error ? created.error->message.c_str() : "(no message)");
    set_err(err, err_size,
            created.error ? created.error->message : std::string("the runtime failed to start"));
    s_running.store(false);
    return 0;
  }

  {
    std::lock_guard<std::mutex> lock(s_runtime_mutex);
    s_runtime = std::move(created.runtime);
  }

  // Installed before Run() so no boot state transition is missed, but it only
  // attaches the overrider once the pads actually exist.
  s_overrider_ready.store(false);
  s_state_hook = Core::AddOnStateChangedCallback(&AttachOverriderWhenPadsExist);

  run_log("Runtime::Run ... (blocks until the game stops)");
  const moderngekko::RuntimeRunResult result = s_runtime->Run();
  run_log("Runtime::Run returned: reason=%d %s", (int)result.reason,
          result.error ? result.error->message.c_str() : "");

  if (s_overrider_ready.exchange(false))
    ciface::Touch::UnregisterGameCubeInputOverrider(kTouchPadIndex);
  s_state_hook.reset();

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
  // Dropped rather than queued: a touch that lands during boot is stale by the
  // time the game reads it.
  if (!s_overrider_ready.load())
    return;
  ciface::Touch::SetControlState(kTouchPadIndex, static_cast<ciface::Touch::ControlID>(control),
                                 state);
}

void db_clear_control(DBPadControl control)
{
  if (!s_overrider_ready.load())
    return;
  ciface::Touch::ClearControlState(kTouchPadIndex,
                                   static_cast<ciface::Touch::ControlID>(control));
}
