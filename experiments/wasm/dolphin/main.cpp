// Dolphin in WebAssembly, running a statically recompiled game module.
//
// Two builds come out of one file. The node build is the measurement harness:
// boot a disc, run for a fixed budget, report emulation speed. The browser
// build is the real thing -- the same runtime with a canvas, a keyboard, and a
// disc mounted over HTTP.
//
// The number that decides the route is `speed` in the perf line: the fraction
// of real time the guest keeps up with. With no JIT anywhere in this build a
// guest range the module does not cover runs on the plain interpreter, so also
// watch native_dispatches / fallback_steps in the shutdown line. Coverage is a
// speed cliff here, not a rounding error.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>

#include "moderngekko/module_abi.h"
#include "moderngekko/runtime.hpp"

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/CPU.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/Touch/InputOverrider.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoConfig.h"

#include <emscripten/emscripten.h>

#ifdef DOLWEB_WEB
#include <emscripten/wasmfs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "DolphinNoGUI/Platform.h"
#endif

#ifdef DOLWEB_HAVE_NATIVE_MODULES
#include "dolweb_native_modules.inc"
#endif

namespace
{
constexpr int kPadIndex = 0;
std::atomic<bool> s_overrider_ready{false};
std::atomic<bool> s_running{false};

// The disc ID is the first six bytes of sys/boot.bin. Reading it here rather
// than taking it on the command line keeps the module choice tied to the disc
// actually being booted, which is what the runtime validates against.
std::string ReadDiscId(const std::string& game_root)
{
  const std::string path = game_root + "/sys/boot.bin";
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return {};
  char id[7] = {};
  const size_t read = std::fread(id, 1, 6, file);
  std::fclose(file);
  return read == 6 ? std::string(id, 6) : std::string{};
}

#ifdef DOLWEB_WEB
// WASMFS, not the JS filesystem, and the reason is threads: emscripten's JS FS
// lives in each worker's own JS state, so a file created on one thread is
// invisible to the thread Dolphin actually reads the disc on. WASMFS keeps its
// state in linear memory, which every thread shares.
//
// The fetch backend turns each file into HTTP range requests against the page's
// origin. That is what makes a 1.2 GB disc usable in a browser without
// downloading it: the boot touches a few tens of megabytes and the movies
// stream. It cannot list a directory, though -- a fetch directory only knows
// the children something inserted -- so the tree comes from a manifest the
// server writes next to the game.
bool MountFetchTree(const std::string& base_url, const std::string& mount_point)
{
  // The chunk size is the whole story for how a phone boots. Emscripten's
  // fetch backend downloads a file *in full* on first touch unless it is more
  // than twice the chunk size -- and the default chunk is 16 MB, so with a
  // 1.34 GB disc of mostly-small files, Dolphin building the directory FST
  // pulls the entire disc over Wi-Fi before the CPU starts: 189 files, ~1 GB,
  // sixty to a hundred seconds, and all of it resident in the JS heap where a
  // phone's jetsam limit is waiting. A small chunk turns the same walk into a
  // first-touch read per file: 24 MB at 128 KB, 48 MB at 256 KB.
  //
  // Smaller is not free -- a game that streams then pays a round trip per
  // chunk -- so it is a knob, and the default is the compromise measured on
  // this disc.
  uint32_t fetch_chunk = 256 * 1024;
  if (const char* env = std::getenv("DOLWEB_FETCH_CHUNK"))
  {
    const long kb = std::strtol(env, nullptr, 10);
    if (kb > 0)
      fetch_chunk = static_cast<uint32_t>(kb) * 1024;
  }
  std::printf("[dolweb] fetch chunk %u KB\n", fetch_chunk / 1024);
  backend_t backend = wasmfs_create_fetch_backend(base_url.c_str(), fetch_chunk);
  if (!backend)
  {
    std::printf("[dolweb] fetch backend failed for %s\n", base_url.c_str());
    return false;
  }
  if (wasmfs_create_directory(mount_point.c_str(), 0777, backend) != 0)
  {
    std::printf("[dolweb] cannot mount %s\n", mount_point.c_str());
    return false;
  }

  // The manifest arrives through the mount itself, so there is no second
  // transport to get wrong: it is the first file created in the tree it
  // describes.
  const std::string manifest_path = mount_point + "/.manifest";
  int fd = wasmfs_create_file(manifest_path.c_str(), 0444, backend);
  if (fd < 0)
  {
    std::printf("[dolweb] no manifest at %s/.manifest\n", base_url.c_str());
    return false;
  }
  std::string manifest;
  char chunk[8192];
  for (ssize_t got = read(fd, chunk, sizeof(chunk)); got > 0;
       got = read(fd, chunk, sizeof(chunk)))
    manifest.append(chunk, static_cast<size_t>(got));
  close(fd);
  if (manifest.empty())
  {
    std::printf("[dolweb] manifest at %s is empty\n", manifest_path.c_str());
    return false;
  }

  size_t created = 0;
  size_t start = 0;
  while (start < manifest.size())
  {
    size_t end = manifest.find('\n', start);
    if (end == std::string::npos)
      end = manifest.size();
    std::string rel = manifest.substr(start, end - start);
    start = end + 1;
    while (!rel.empty() && (rel.back() == '\r' || rel.back() == ' '))
      rel.pop_back();
    if (rel.empty() || rel[0] == '#')
      continue;

    const std::string full = mount_point + "/" + rel;
    // Parent directories first; mkdir inside a fetch directory makes another
    // fetch directory, so the whole tree stays backed by HTTP.
    for (size_t slash = full.find('/', 1); slash != std::string::npos;
         slash = full.find('/', slash + 1))
      mkdir(full.substr(0, slash).c_str(), 0777);

    fd = wasmfs_create_file(full.c_str(), 0444, backend);
    if (fd < 0)
    {
      std::printf("[dolweb] cannot create %s\n", full.c_str());
      return false;
    }
    close(fd);
    ++created;
  }
  std::printf("[dolweb] mounted %s from %s (%zu files)\n", mount_point.c_str(),
              base_url.c_str(), created);
  return true;
}
#endif

const ModernGekkoModuleDesc* FindModule(const std::string& disc_id)
{
#ifdef DOLWEB_HAVE_NATIVE_MODULES
  for (const auto& entry : kDolWebNativeModules)
  {
    if (disc_id == entry.game_id)
      return entry.get_module();
  }
#else
  (void)disc_id;
#endif
  return nullptr;
}

// The pads do not exist until the core has started, so the overrider attaches
// on the first state change that reports one. Same sequence as the iOS app, and
// for the same reason.
Common::EventHook AttachOverrider()
{
  return Core::AddOnStateChangedCallback([](Core::State state) {
    if (state == Core::State::Uninitialized || state == Core::State::Stopping)
      return;
    if (s_overrider_ready.load())
      return;
    if (Pad::GetConfig()->GetControllerCount() <= kPadIndex)
      return;
    ciface::Touch::RegisterGameCubeInputOverrider(kPadIndex);
    s_overrider_ready.store(true);
  });
}

// Dolphin's own log, enabled after UICommon::Init() has built the LogManager.
// Writing Logger.ini does not work: the config layers are assembled during
// Create() and the file is read too early to affect the boot being watched.
void EnableDolphinLog()
{
  const char* wanted = std::getenv("DOLWEB_DEBUG_LOG");
  if (!wanted || wanted[0] < '1' || wanted[0] > '9')
    return;
  using namespace Common::Log;
  LogManager* manager = LogManager::GetInstance();
  if (!manager)
    return;
  // 1 = NOTICE, which is the boot narrative. 3 = INFO, which is where the video
  // backend says what it found and what it is missing. 5 = DEBUG, a line per
  // DVD read.
  const int level = std::min(std::max(wanted[0] - '0', 1), 5);
  manager->SetConfigLogLevel(static_cast<LogLevel>(level));
  for (int i = 0; i < static_cast<int>(LogType::NUMBER_OF_LOGS); ++i)
    manager->SetEnable(static_cast<LogType>(i), true);
  manager->EnableListener(LogListener::CONSOLE_LISTENER, true);
}

// Read `speed`, not `fps`: a GameCube game's own frame rate varies by scene, so
// only speed says whether emulation is keeping up.
void StartPerfLog(double interval_seconds)
{
  if (interval_seconds <= 0.0)
    return;
  std::thread([interval_seconds] {
    {
      auto& system = Core::System::GetInstance();
      std::printf("[dolweb] guest clock %u Hz\n",
                  system.GetSystemTimers().GetTicksPerSecond());
      std::fflush(stdout);
    }
    while (s_running.load())
    {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<long long>(interval_seconds * 1000.0)));
      if (!s_running.load())
        break;
      auto& system = Core::System::GetInstance();
      const auto& metrics = system.GetPerfMetrics();
      // pc as well as speed: a boot that stops making progress reports 0%
      // exactly like a boot that never started, and the guest PC is the one
      // thing that tells the two apart without a debugger.
      // ticks is the guest's own clock, and it is here so that two runs can be
      // compared over the same guest interval rather than the same wall-clock
      // one. A GameCube game's attract loop is the same every boot, so equal
      // tick ranges mean equal scenes -- which is the only way to put Null next
      // to OpenGL honestly when savestates will not load on this target.
      std::printf("[perf] %.1f fps  %.0f%% speed  pc=0x%08x lr=0x%08x cpu=%d ticks=%llu\n",
                  metrics.GetFPS(), metrics.GetSpeed() * 100.0,
                  system.GetPPCState().pc, system.GetPPCState().spr[SPR_LR],
                  static_cast<int>(system.GetCPU().GetState()),
                  static_cast<unsigned long long>(system.GetCoreTiming().GetTicks()));
      std::fflush(stdout);
    }
  }).detach();
}
}  // namespace

// Called from the page. Dropped rather than queued before the overrider exists:
// a key that lands during boot is stale by the time the game reads it.
extern "C" EMSCRIPTEN_KEEPALIVE void dolweb_set_control(int control, double state)
{
  if (!s_overrider_ready.load())
    return;
  ciface::Touch::SetControlState(kPadIndex, static_cast<ciface::Touch::ControlID>(control),
                                 state);
}

// Read a file out of the emulator's filesystem and onto the page's console.
// In a browser the user directory lives in linear memory, so Dolphin's own
// diagnostics -- a shader it could not compile, most usefully -- are otherwise
// written somewhere nothing can read them.
extern "C" EMSCRIPTEN_KEEPALIVE void dolweb_cat(const char* path)
{
  FILE* file = std::fopen(path, "rb");
  if (!file)
  {
    std::printf("[cat] %s: not found\n", path);
    return;
  }
  std::printf("[cat] ---- %s ----\n", path);
  char line[4096];
  int number = 0;
  while (std::fgets(line, sizeof(line), file))
    std::printf("%d: %s", ++number, line);
  std::fclose(file);
  std::printf("[cat] ---- end %s ----\n", path);
  std::fflush(stdout);
}

extern "C" EMSCRIPTEN_KEEPALIVE void dolweb_clear_control(int control)
{
  if (!s_overrider_ready.load())
    return;
  ciface::Touch::ClearControlState(kPadIndex,
                                   static_cast<ciface::Touch::ControlID>(control));
}

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::printf("usage: dolweb <game-root> <user-dir> [backend] [seconds]\n");
    return 2;
  }

  std::string game_root = argv[1];
  const std::string user_dir = argv[2];
  const std::string backend = argc > 3 ? argv[3] : "Null";
  const double budget = argc > 4 ? std::atof(argv[4]) : 0.0;

  // Everything past the budget is KEY=VALUE. A browser has no environment to
  // export, and every knob in this tree -- Dolphin's, the chassis's and this
  // harness's -- is read with getenv(), so the page passes them as arguments and
  // they land in the environment before anything reads one.
  for (int i = 5; i < argc; ++i)
  {
    const char* eq = std::strchr(argv[i], '=');
    if (!eq || eq == argv[i])
      continue;
    const std::string key(argv[i], eq - argv[i]);
    setenv(key.c_str(), eq + 1, 1);
    std::printf("[dolweb] %s=%s\n", key.c_str(), eq + 1);
  }

#ifdef DOLWEB_WEB
  // argv[1] is a URL prefix in the browser, not a path. Mount it, then carry on
  // exactly as every other host does.
  if (!MountFetchTree(game_root, "/game"))
    return 1;
  game_root = "/game";
  Platform::SetEmscriptenCanvas(std::getenv("DOLWEB_CANVAS"));
#endif

  moderngekko::RuntimeConfig config;
  config.game_root = game_root;
  config.user_directory = user_dir;
  config.graphics.backend = backend;
  config.headless = backend == "Null";
  config.show_fps_in_title = true;
#ifdef DOLWEB_WEB
  // The browser build has a real backend; the measurement build deliberately
  // does not, so a speed figure is never a figure about the audio thread.
  config.audio.backend = std::getenv("DOLWEB_NO_AUDIO") ? "No Audio Output" : "WebAudio";
#else
  config.audio.backend = "No Audio Output";
#endif
  // The GPU has its own thread, and it is the single largest performance win
  // measured in this build. Gameplay, throttle off, frames actually rendered:
  //
  //   single core   45.8 fps median      dual core   67.3 fps median
  //
  // (Guest-seconds-per-wall-second reads 82.9% -> 100-115%, but with dual core
  // the CPU thread runs ahead of the GPU, so that metric flatters it. Frames
  // rendered is the honest one.)
  //
  // Why it is worth so much here and not on a desktop: every GL call is proxied
  // to the browser's main thread, so on one thread the emulated CPU blocks on a
  // cross-thread round trip per call -- a profile shows it waiting 38% of the
  // time while the main thread sits 93% idle. A second thread absorbs that
  // latency and the CPU runs ahead. It is the renderer's cost recovered, and it
  // is why the earlier "dual core is an 11% loss" reading did not survive: that
  // was measured in the menus, where the renderer is nearly free and the extra
  // thread only costs synchronisation.
  //
  // This was opt-in for a while on the belief that WebKit intermittently
  // rendered nothing with it -- "0 of 10 screenshots across 200 seconds". That
  // was a measurement artifact, not a defect. `simctl openurl` can time out
  // while the page loads and runs perfectly; sim-run.sh ran under `set -e`, so
  // the timeout aborted the run before it took a single screenshot, and the
  // detector scored zero screenshots as zero pictures. The run it "failed" on
  // went on to render for sixteen hours at 99% speed. No dual-core session in
  // the whole report log has ever shown the claimed signature of 0 fps at a
  // full-speed guest clock. sim-run.sh now tolerates the timeout and the
  // detector separates "never launched" from "launched and drew nothing".
  //
  // DOLWEB_CPU_THREAD=0 turns it off.
  config.cpu_thread = true;
  if (const char* cpu_thread = std::getenv("DOLWEB_CPU_THREAD"))
    config.cpu_thread = (cpu_thread[0] != '0' && cpu_thread[0] != '\0');
  // Two builds only line up if they are measured in the same scene, and a boot
  // is twelve seconds of logos followed by whatever the attract loop is doing
  // when the window opens. A savestate puts both in the same place; see
  // ios/PERFORMANCE.md, which learned this the hard way.
  if (const char* state = std::getenv("DOLWEB_STATE"))
    config.load_state_path = state;
  // Internal resolution, so a run can ask whether the renderer is fill-bound or
  // call-bound: if halving it does not move the number, the GPU is not the
  // thing that is busy.
  if (const char* scale = std::getenv("DOLWEB_EFB_SCALE"))
    config.graphics.internal_resolution_scale = std::atoi(scale);
  // A diagnostic more than a setting: Stretch takes the presenter's draw-rect
  // arithmetic out of the picture, which is how you tell "the image is being
  // placed wrong" from "the XFB only has content in part of it".
  // DOLWEB_GFX_HACK=name[,name] flips Dolphin's EFB hacks from here, so a
  // surface that renders black can be walked through the copy paths without a
  // rebuild each time. (Wireframe is not among them: it needs glPolygonMode,
  // which GLES and WebGL do not have.)
  if (const char* hacks = std::getenv("DOLWEB_GFX_HACK"))
  {
    const std::string list = std::string(",") + hacks + ",";
    const auto on = [&list](const char* name) {
      return list.find(std::string(",") + name + ",") != std::string::npos;
    };
    if (on("SkipEFBCopyToRam"))
      Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, true);
    if (on("DisableCopyToVRAM"))
      Config::SetBase(Config::GFX_HACK_DISABLE_COPY_TO_VRAM, true);
    if (on("NoEFBAccess"))
      Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE, false);
    if (on("NoCopyEFBScaled"))
      Config::SetBase(Config::GFX_HACK_COPY_EFB_SCALED, false);
    std::printf("[dolweb] gfx hacks: %s\n", hacks);
  }
  if (const char* aspect = std::getenv("DOLWEB_ASPECT"))
  {
    Config::SetBase(Config::GFX_ASPECT_RATIO,
                    std::string(aspect) == "stretch" ? AspectMode::Stretch :
                    std::string(aspect) == "raw"     ? AspectMode::Raw :
                                                       AspectMode::Auto);
    std::printf("[dolweb] aspect=%s\n", aspect);
  }

  const std::string disc_id = ReadDiscId(game_root);
  const ModernGekkoModuleDesc* module = FindModule(disc_id);
  if (module)
  {
    config.module = moderngekko::ModuleSource::AttachedDescriptor(module);
    // Deliberately strict: if the embedded module does not match this disc or
    // this CPUState layout, say so instead of quietly interpreting the whole
    // game and reporting the interpreter's speed as the module's.
    config.allow_interpreter = false;
    std::printf("[dolweb] disc %s: embedded module attached (%u code ranges)\n",
                disc_id.c_str(), module->num_code_ranges);
  }
  else
  {
    config.allow_interpreter = true;
    std::printf("[dolweb] disc %s: no embedded module; interpreter-only\n",
                disc_id.c_str());
  }
  std::printf("[dolweb] backend=%s budget=%.1fs\n", backend.c_str(), budget);
  std::fflush(stdout);

  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created.runtime)
  {
    std::printf("[dolweb] Create failed: %s\n",
                created.error ? created.error->message.c_str() : "(no message)");
    return 1;
  }
  EnableDolphinLog();
  const Common::EventHook state_hook = AttachOverrider();
  std::printf("[dolweb] runtime created; running\n");
  std::fflush(stdout);

  s_running.store(true);
  const char* interval = std::getenv("DOLWEB_PERF_INTERVAL");
  StartPerfLog(interval ? std::atof(interval) : 2.0);

  // Run() blocks until the guest stops, so the budget needs its own thread.
  std::thread stopper;
  if (budget > 0.0)
  {
    moderngekko::Runtime* runtime = created.runtime.get();
    stopper = std::thread([runtime, budget] {
      const auto deadline =
          std::chrono::steady_clock::now() +
          std::chrono::milliseconds(static_cast<long long>(budget * 1000.0));
      while (s_running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (s_running.load())
      {
        std::printf("[dolweb] budget reached; stopping\n");
        std::fflush(stdout);
        runtime->RequestStop();
      }
    });
  }

  const auto start = std::chrono::steady_clock::now();
  auto result = created.runtime->Run();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  s_running.store(false);
  if (stopper.joinable())
    stopper.join();

  std::printf("[dolweb] Run() returned reason=%d after %.2f s\n",
              static_cast<int>(result.reason), seconds);
  if (result.error)
    std::printf("[dolweb] error: %s\n", result.error->message.c_str());
  std::fflush(stdout);
  return 0;
}
