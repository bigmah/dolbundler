#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBACore.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoConfig.h"
#include "dolphin_runtime_internal.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/dolvm_module.hpp"
#include "moderngekko/mod_loader.hpp"
#include "moderngekko/module_loader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// The interpreter's bench raises this once it has charged past
// DOLVM_BENCH_SKIP. Declared rather than included: dolvm_bridge.h is the one
// header in the tree that speaks DolRecomp's CPUState, and this translation
// unit already has the chassis's.
extern "C" int dolvm_bridge_bench_at_skip(void);

namespace {
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;
u64 s_previous_net_wait_ns = 0;
double s_net_wait_ms_per_second = 0.0;
std::chrono::steady_clock::time_point s_previous_net_wait_sample;

std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  const auto now = std::chrono::steady_clock::now();
  std::string formatted_title = fmt::format("{} | {:.1f} FPS", title, fps);
  const NetPlay::InputWaitTelemetry telemetry =
      NetPlay::NetPlayClient::GetInputWaitTelemetry();
  if (!telemetry.active) {
    s_previous_net_wait_ns = 0;
    s_net_wait_ms_per_second = 0.0;
    s_previous_net_wait_sample = {};
    return formatted_title;
  }
  if (s_previous_net_wait_sample.time_since_epoch().count() == 0) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  } else if (telemetry.total_wait_ns < s_previous_net_wait_ns) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
    s_net_wait_ms_per_second = 0.0;
  } else if (now - s_previous_net_wait_sample >=
             std::chrono::milliseconds(500)) {
    const double seconds =
        std::chrono::duration<double>(now - s_previous_net_wait_sample).count();
    s_net_wait_ms_per_second =
        static_cast<double>(telemetry.total_wait_ns - s_previous_net_wait_ns) /
        1000000.0 / seconds;
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  }
  return fmt::format("{} | Net wait {:.1f} ms/s | Buffer {}", formatted_title,
                     s_net_wait_ms_per_second, telemetry.buffer_size);
}
} // namespace

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  if (!s_platform)
    return;

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(
        title, Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
struct Runtime::Impl {
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  std::unique_ptr<ModManager> mods;
  Common::EventHook state_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  std::atomic<bool> running{false};
};

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

ModuleSource ModuleSource::BytecodePath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::BytecodePath;
  source.path = std::move(path);
  return source;
}

bool ModuleSource::IsBytecodePath(const std::filesystem::path &path) {
  return DolVMModule::IsBytecodePath(path);
}

ModuleSource ModuleSource::ForPath(std::filesystem::path path) {
  return IsBytecodePath(path) ? BytecodePath(std::move(path))
                              : DynamicPath(std::move(path));
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  // Skipping the assets hash is most of a phone's launch time; the runtime
  // never reads it.
  GameInspectResult inspected = InspectGame(config.game_root, /*hash_assets=*/false);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  std::string bytecode_error;
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (config.module.kind == ModuleSource::Kind::BytecodePath) {
    // A bytecode module is loaded here rather than deeper in the chassis
    // because it becomes an ordinary attached descriptor the moment it is
    // open: from the CPU core's side there is nothing left to distinguish it.
    if (DolVMModule::Open(config.module.path, inspected.metadata->disc_id,
                          &bytecode_error))
      module_result =
          validation_library.Attach(DolVMModule::Descriptor(), requirements);
  } else if (!config.allow_interpreter)
    return {
        {},
        RuntimeError{
            RuntimeErrorCode::ModuleRequired,
            "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      if (!bytecode_error.empty())
        message = "bytecode module was rejected: " + bytecode_error;
      else if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      DolVMModule::Close();
      return {
          {},
          RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    DolVMModule::Close();
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      "ModernGekko - " + impl->metadata.game_name + " [" +
      impl->metadata.disc_id + "]");
  impl->mods = std::make_unique<ModManager>();
  const ModLoadReport mod_report = impl->mods->LoadDirectories(
      impl->config.mod_directories, impl->metadata.disc_id);
  for (const ModLoadIssue &issue : mod_report.issues)
    std::fprintf(stderr, "mod rejected: %s: %s\n", issue.source.c_str(),
                 issue.message.c_str());
  for (const LoadedModInfo &mod : mod_report.loaded)
    std::fprintf(stderr, "mod loaded: %s %s\n", mod.id.c_str(),
                 mod.version.c_str());

  if (!s_external_ui_common) {
    UICommon::SetUserDirectory(impl->config.user_directory.string());
    UICommon::Init();
    impl->ui_initialized = true;
  }
  Config::SetBase(Config::MAIN_FULLSCREEN, impl->config.fullscreen);

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
#ifdef MODERNGEKKO_HAVE_COCOA
  else impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef MODERNGEKKO_HAVE_UIKIT
  // Fails Init() if the app has not handed over a render layer yet, which
  // surfaces as PlatformUnavailable rather than a crash in the video backend.
  else impl->platform = Platform::CreateIOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland) impl->platform =
      Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11) impl->platform =
      Platform::CreateWaylandPlatform();
#endif
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                         "the requested Dolphin host platform is unavailable"}};
  }

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);

  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  if (impl->config.cpu_thread)
    Config::SetBase(Config::MAIN_CPU_THREAD, *impl->config.cpu_thread);
  // Two builds of the same scene only line up if the guest clock does. With the
  // limiter off, a faster build is somewhere else in an attract loop by the time
  // a screenshot lands, and the two pictures are not of the same thing.
  if (const char* speed = std::getenv("MODERNGEKKO_EMULATION_SPEED"))
    Config::SetBase(Config::MAIN_EMULATION_SPEED, (float)std::atof(speed));
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE,
                    *impl->config.graphics.internal_resolution_scale);
  Config::SetBase(Config::GFX_SHADER_CACHE, true);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  const std::vector<std::string> audio_backends =
      AudioCommon::GetSoundBackends();
  if (impl->config.headless) {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  } else if (impl->config.audio.backend.empty() ||
             !std::ranges::contains(audio_backends,
                                    impl->config.audio.backend)) {
    constexpr std::array preferred_backends = {
        BACKEND_CUBEB, BACKEND_PULSEAUDIO, BACKEND_ALSA};
    const auto preferred =
        std::ranges::find_if(preferred_backends, [&](const char *backend) {
          return std::ranges::contains(audio_backends, backend);
        });
    impl->config.audio.backend =
        preferred != preferred_backends.end() ? *preferred : BACKEND_NULLSOUND;
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT,
                  impl->config.input.background_input);

  auto &jit = Core::System::GetInstance().GetJitInterface();
  StaticRecompModuleSource recomp_source;
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    recomp_source =
        StaticRecompModuleSource::Dynamic(impl->config.module.path.string());
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    recomp_source = StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            impl->config.module.descriptor));
  else if (impl->config.module.kind == ModuleSource::Kind::BytecodePath) {
    recomp_source = StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            DolVMModule::Descriptor()));
    // The core logs the module source by path, and an attached descriptor
    // normally has none. This one does, and it is the useful thing to see.
    recomp_source.path = impl->config.module.path.string();
    // The interpreter can follow calls and returns itself, given the core's
    // word on which chunks it would dispatch into; a native module has no
    // such hook and keeps returning for every one.
    recomp_source.publish_gate = &DolVMModule::PublishGate;
  }
  if (!impl->mods->Empty()) {
    recomp_source.host_call = &ModManager::HostCall;
    recomp_source.host_call_contains = &ModManager::HostCallContains;
    recomp_source.host_call_range_contains =
        &ModManager::HostCallRangeContains;
    recomp_source.host_call_user = impl->mods.get();
  }
  jit.SetStaticRecompModuleSource(std::move(recomp_source));

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  RequestStop();
  if (m_impl->booted) {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  DolVMModule::Close();
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_show_fps_in_title = true;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  std::unique_ptr<BootParameters> boot;
  {
    std::lock_guard lock(s_runtime_mutex);
    if (s_boot_session_data)
      boot = BootParameters::GenerateFromFile(
          PathToString(m_impl->metadata.main_dol), std::move(*s_boot_session_data));
    else if (m_impl->config.load_state_path)
      // DeleteSavestateAfterBoot::No: the state is the player's, not a
      // scratch file this run owns.
      boot = BootParameters::GenerateFromFile(
          PathToString(m_impl->metadata.main_dol),
          BootSessionData(PathToString(*m_impl->config.load_state_path),
                          DeleteSavestateAfterBoot::No));
    else
      boot =
          BootParameters::GenerateFromFile(PathToString(m_impl->metadata.main_dol));
    s_boot_session_data.reset();
  }
  if (!boot) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
  }
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  // Comparing two builds means measuring the same scene in both, and a game
  // does not stand still: an attract loop that alternates menus and gameplay
  // differs by 30% in interpretation cost between the two. The interpreter's
  // bench raises a flag once it has charged past DOLVM_BENCH_SKIP, so a state
  // written at that moment is a fixed guest instant rather than a wall one --
  // which turns a two-minute warm-up per measurement into a load. Off unless
  // MODERNGEKKO_SAVE_STATE_AT_SKIP names a file to write.
  std::jthread bench_state_thread;
  if (const char* bench_state = std::getenv("MODERNGEKKO_SAVE_STATE_AT_SKIP")) {
    bench_state_thread =
        std::jthread([path = std::string(bench_state)](std::stop_token stop) {
          while (!stop.stop_requested()) {
            if (dolvm_bridge_bench_at_skip()) {
              State::SaveAs(Core::System::GetInstance(), path);
              std::fprintf(stderr, "[dolvm-bench] savestate written to %s\n",
                           path.c_str());
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
          }
        });
  }
  // Wall-clock sibling of the above, for scenes that no bench window names:
  // "boot, wait, and write a state" is how a menu or an attract-mode scene
  // becomes reloadable. Chasing a miscompile means running the same scene
  // dozens of times, and a two-minute boot per attempt is the difference
  // between bisecting a function and giving up on it.
  // MODERNGEKKO_SAVE_STATE_AFTER=<seconds>:<path>.
  std::jthread timed_state_thread;
  if (const char* timed = std::getenv("MODERNGEKKO_SAVE_STATE_AFTER")) {
    const std::string spec(timed);
    const size_t colon = spec.find(':');
    if (colon != std::string::npos) {
      const int delay = std::atoi(spec.substr(0, colon).c_str());
      std::string path = spec.substr(colon + 1);
      if (delay > 0 && !path.empty()) {
        timed_state_thread = std::jthread(
            [delay, path = std::move(path)](std::stop_token stop) {
              for (int i = 0; i < delay * 10 && !stop.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              if (stop.stop_requested())
                return;
              State::SaveAs(Core::System::GetInstance(), path);
              std::fprintf(stderr, "[state] savestate written to %s\n",
                           path.c_str());
            });
      }
    }
  }
  std::jthread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title) {
    title_thread = std::jthread([](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        Host_UpdateTitle({});
        for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  m_impl->platform->MainLoop();
  bench_state_thread.request_stop();
  timed_state_thread.request_stop();
  title_thread.request_stop();
  if (title_thread.joinable())
    title_thread.join();
  m_impl->platform->SaveWindowGeometry();
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  return {};
}

void Runtime::RequestStop() {
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }
} // namespace moderngekko
