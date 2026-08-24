// SPDX-License-Identifier: GPL-3.0-or-later

#include "dolbundler_run.h"

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/Config/MainSettings.h"  // BACKEND_NULLSOUND
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "InputCommon/ControllerInterface/Touch/InputOverrider.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "moderngekko/runtime.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

// Sample emulation speed into the run log. Off unless asked for.
//
// The on-screen overlay shows the same two numbers live, but a device with no
// console attached cannot be read that way, and a phone in your hand cannot be
// watched and benchmarked at once. This leaves a trace to pull off afterwards.
//
// Read `speed`, not `fps`: a GameCube game's own frame rate varies by scene --
// 30 in one, 60 in another -- so only speed says whether emulation is keeping
// up. See PERFORMANCE.md.
void StartPerfLog(std::atomic<bool>* running)
{
  const char* wanted = getenv("DOLBUNDLER_PERF_LOG");
  if (!wanted || wanted[0] != '1')
    return;

  std::thread([running] {
    while (running->load())
    {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      if (!running->load())
        break;
      const auto& metrics = Core::System::GetInstance().GetPerfMetrics();
      run_log("perf: %.1f fps  %.0f%% speed  peak %.0f/%.0fms",
              metrics.GetFPS(), metrics.GetSpeed() * 100.0,
              DT_ms(metrics.TakeFramePeak()).count(),
              DT_ms(metrics.TakeVBlankPeak()).count());
    }
  }).detach();
}

// Stop the game by itself after N seconds. Without this a scripted run can only
// be killed, and a kill skips Core shutdown -- which is where StaticRecomp
// prints how much of the guest ran natively versus through the chassis:
//
//   [staticrecomp] shutdown: native=... fallback=... hook_fb=... smc_failed=...
//
// Those counters are a property of the module and the game rather than of the
// host, so they are the one performance question the simulator can answer.
void StartRunTimer(std::atomic<bool>* running)
{
  const char* seconds = getenv("DOLBUNDLER_RUN_SECONDS");
  if (!seconds)
    return;
  const int wanted = atoi(seconds);
  if (wanted <= 0)
    return;

  // Optionally take a screenshot part way through. There is no way to capture a
  // device's screen from a Mac the way `simctl io screenshot` captures the
  // simulator's, and the simulator renders this class of game black regardless
  // -- so without this, a rendering bug on a phone can only be described, not
  // seen. Dolphin writes into its own screenshots directory, which is inside
  // the app's Documents and therefore reachable with `devicectl copy from`.
  const char* shot_at = getenv("DOLBUNDLER_SCREENSHOT_AFTER");
  const int shot_seconds = shot_at ? atoi(shot_at) : 0;

  std::thread([running, wanted, shot_seconds] {
    for (int i = 0; i < wanted && running->load(); ++i)
    {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (shot_seconds > 0 && i + 1 == shot_seconds && running->load())
      {
        run_log("screenshot requested at %ds", shot_seconds);
        Core::SaveScreenShot();
      }
    }
    if (running->load())
    {
      run_log("run timer expired after %ds, stopping", wanted);
      db_request_stop();
    }
  }).detach();
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
  // Anything the runtime writes to stderr -- the DolVM sampler's report, most
  // of all -- goes into the same file, because stderr on a device goes nowhere
  // a `devicectl copy from` can reach. Only when the perf log is on, so a
  // normal run is unaffected.
  if (const char* perf = getenv("DOLBUNDLER_PERF_LOG"))
    if (perf[0] == '1')
    {
      freopen(s_log_path.c_str(), "a", stderr);
      // stderr is unbuffered only while it is a terminal; redirected to a file
      // it becomes fully buffered, and the last few kilobytes of a run then
      // sit in that buffer until the process exits. Pulling the log off the
      // device mid-run then reads a truncated one, which looks exactly like a
      // run that stopped early -- and cost an afternoon reading a 150-second
      // measurement as a 30-second crash.
      setvbuf(stderr, nullptr, _IOLBF, 0);
    }

  moderngekko::RuntimeConfig config;
  config.game_root = game_root;
  config.user_directory = user_dir;
  config.module = moderngekko::ModuleSource::BytecodePath(module_path);
  config.graphics.backend = "Metal";
  // AudioCommon's own default resolves to this on iOS too, but naming it means
  // a silent game is a wrong-backend bug rather than an unnoticed fallback.
  //
  // Except on the simulator, which plays through the Mac's speakers while the
  // development loop is launch-run-relaunch. Silent by default there;
  // DOLBUNDLER_SIM_AUDIO=1 turns it back on when audio is the thing being
  // worked on. Device builds are unaffected -- TARGET_OS_SIMULATOR is a
  // compile-time property of a separate binary.
#if TARGET_OS_SIMULATOR
  const char* sim_audio = getenv("DOLBUNDLER_SIM_AUDIO");
  config.audio.backend = (sim_audio && sim_audio[0] == '1') ? "AudioUnit" : BACKEND_NULLSOUND;
#else
  config.audio.backend = "AudioUnit";
#endif
  // Two switches for isolating what the emulation thread waits on. Profiling
  // the phone put a third of that thread's samples in __semwait_signal and
  // swtch_pri -- blocked, not computing -- and in single-core mode the video
  // work runs on the same thread, so audio and presentation are both
  // candidates and neither can be told apart from the outside. Turning each
  // off in turn on the device answers it in two runs. Off unless set, so a
  // normal build is untouched.
  if (const char* quiet = getenv("DOLBUNDLER_NULL_AUDIO"))
    if (quiet[0] == '1')
      config.audio.backend = BACKEND_NULLSOUND;
  if (const char* blind = getenv("DOLBUNDLER_NULL_VIDEO"))
    if (blind[0] == '1')
      config.graphics.backend = "Null";

  // The emulated GPU stays on the emulation thread. Giving it its own is worth
  // +19% on the desktop against a real graphics backend, and **-15% on the
  // phone**: measured in Olliewood on an iPhone 15 Pro Max, interleaved,
  // single core holds a median 72-77% and dual core 60-62%. That agrees with
  // the reading this project took in 2026-08-21 and disagrees with every
  // desktop measurement, so the desktop does not predict a phone here.
  //
  // DOLBUNDLER_DUAL_CORE=1 turns it on, because the *why* is still open --
  // most likely the GPU thread landing on an efficiency core while the
  // emulation thread waits on it.
  config.cpu_thread = false;
  if (const char* dual = getenv("DOLBUNDLER_DUAL_CORE"))
    if (dual[0] == '1')
      config.cpu_thread = true;

  config.window_title = title ? std::string(title) : std::string();
  config.fullscreen = true;
  // A .dvm covers the whole title; letting the chassis fall back to its own
  // interpreter would silently mask a module that failed to load.
  config.allow_interpreter = false;
  config.show_fps_in_title = false;

  // Test hook, off unless asked for: boot straight into a savestate.
  //
  // Measuring emulation speed on a phone means measuring the same scene twice,
  // and the scenes that matter are minutes into an autoplay run -- Disney
  // skate's attract demo is eight minutes in at the speed the phone manages,
  // and its menus, where the speed is quite different, are everything before
  // it. The desktop bench solved this by pinning a savestate; this is the same
  // savestate, so the two are directly comparable.
  //
  // Relative paths resolve inside the app's Documents directory, which is what
  // a state pushed over devicectl lands in.
  if (const char* state = getenv("DOLBUNDLER_LOAD_STATE"))
  {
    if (state[0] != '\0')
    {
      std::string path = state;
      if (path.front() != '/')
        path = std::string(user_dir) + "/../" + path;
      config.load_state_path = path;
      run_log("load-state: %s", path.c_str());
    }
  }

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

  StartPerfLog(&s_running);
  StartRunTimer(&s_running);

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

void db_get_performance(double* fps, double* speed)
{
  const bool running = s_running.load();
  if (!running)
  {
    if (fps)
      *fps = 0.0;
    if (speed)
      *speed = 0.0;
    return;
  }

  const auto& metrics = Core::System::GetInstance().GetPerfMetrics();
  if (fps)
    *fps = metrics.GetFPS();
  if (speed)
    *speed = metrics.GetSpeed();
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
