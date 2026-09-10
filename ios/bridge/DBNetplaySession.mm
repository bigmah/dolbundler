// SPDX-License-Identifier: GPL-3.0-or-later
#import "DBNetplaySession.h"
#include "dolbundler_run.h"
#include "moderngekko/runtime.hpp"
#include "netplay_compatibility.hpp"
#include "runtime/dolphin_runtime_internal.hpp"
#include "Core/Boot/Boot.h"
#include "Core/Config/CheatSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "Core/PowerPC/PowerPC.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"
#include <atomic>
#include <mutex>

namespace {
std::recursive_mutex activeMutex;
NetPlay::NetPlayClient* activeClient = nullptr;
std::atomic<bool> sessionOpen{false};

class NearbyUI final : public NetPlay::NetPlayUI {
public:
  std::shared_ptr<UICommon::GameFile> game;
  bool hosting = false;
  std::atomic<bool> start{false}, ended{false};
  std::atomic<u32> buffer{NetPlay::GameCubeBufferPolicy::MIN_SIZE};
  std::mutex mutex;
  std::string error, status;
  std::unique_ptr<BootSessionData> boot;
  void Fail(const std::string& message) {
    { std::lock_guard lock(mutex); error = message; }
    ended = true;
    db_request_stop();
  }
  void BootGame(const std::string&, std::unique_ptr<BootSessionData> data) override {
    std::lock_guard lock(mutex); boot = std::move(data);
  }
  void StopGame() override { ended = true; db_request_stop(); }
  bool IsHosting() const override { return hosting; }
  void Update() override {}
  // The nearby UI has no chat pane. Transport diagnostics sent here (such as
  // QoS setup) must not replace the room name and ready instructions.
  void AppendChat(const std::string&) override {}
  void OnMsgChangeGame(const NetPlay::SyncIdentifier&, const std::string&) override {}
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig&) override {}
  void OnMsgStartGame() override { start = true; }
  void OnMsgStopGame() override { StopGame(); }
  void OnMsgPowerButton() override { StopGame(); }
  void OnPlayerConnect(const std::string&) override {}
  void OnPlayerDisconnect(const std::string&) override {}
  void OnPadBufferChanged(u32 value) override { buffer = value; }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32 frame, const std::string&) override {
    Fail("The phones lost synchronization at frame " + std::to_string(frame) + ". Start a new room.");
  }
  void OnConnectionLost() override { Fail("Connection to the host was lost."); }
  void OnConnectionError(const std::string& message) override { Fail(message); }
  void OnTraversalError(Common::TraversalClient::FailureReason) override { Fail("Could not connect."); }
  void OnTraversalStateChanged(Common::TraversalClient::State) override {}
  void OnGameStartAborted() override { Fail("A player disconnected before the game started."); }
  void OnGolferChanged(bool, const std::string&) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }
  std::shared_ptr<const UICommon::GameFile> FindGameFile(
      const NetPlay::SyncIdentifier& id, NetPlay::SyncIdentifierComparison* found) override {
    if (found) *found = game->CompareSyncIdentifier(id);
    return game;
  }
  std::string FindGBARomPath(const std::array<u8, 20>&, std::string_view, int) override { return {}; }
  void ShowGameDigestDialog(const std::string&) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string&) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}
  void ShowChunkedProgressDialog(const std::string&, u64, std::span<const int>) override {
    std::lock_guard lock(mutex); status = "Synchronizing the host's save…";
  }
  void HideChunkedProgressDialog() override { std::lock_guard lock(mutex); status.clear(); }
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}
};
}

int db_netplay_active(void) { return sessionOpen.load(); }
int db_netplay_can_boot(void) {
  std::lock_guard lock(activeMutex);
  return activeClient && activeClient->IsRunning();
}
void db_netplay_stop(void) {
  std::lock_guard lock(activeMutex);
  if (activeClient) activeClient->Stop(); // Also wakes blocked GameCube input polls.
}

@implementation DBNetplaySession {
  NSString* _root;
  NSString* _directory;
  std::unique_ptr<NearbyUI> _ui;
  std::unique_ptr<NetPlay::NetPlayServer> _server;
  std::unique_ptr<NetPlay::NetPlayClient> _client;
  BOOL _initialized;
  BOOL _bootTaken;
  BOOL _startRequested;
}
- (instancetype)initWithGameRoot:(NSString*)root userDirectory:(NSString*)directory {
  if ((self = [super init])) { _root = [root copy]; _directory = [directory copy]; }
  return self;
}
- (BOOL)openAsHost:(BOOL)host port:(uint16_t)port nickname:(NSString*)nickname
            error:(NSString**)error {
  if (db_is_running() || sessionOpen.exchange(true)) {
    if (error) *error = @"A game or nearby room is already open.";
    return NO;
  }
  _initialized = YES;
  _ui = std::make_unique<NearbyUI>();
  _ui->hosting = host;
  UICommon::SetUserDirectory(_directory.UTF8String);
  UICommon::Init();
  moderngekko::detail::SetExternalUICommon(true);
  auto fail = [&](NSString* message) {
    if (error) *error = message;
    [self close];
    return NO;
  };
  const auto inspected = moderngekko::InspectGame(_root.UTF8String);
  if (!inspected) return fail(@"Could not verify the game files. Import Mario Party 7 again.");
  const auto& game = *inspected.metadata;
  if (game.disc_id.size() != 6 || game.disc_id.substr(0, 3) != "GP7")
    return fail(@"Nearby multiplayer currently supports Mario Party 7.");
  const auto* descriptor = db_native_descriptor(game.disc_id.c_str());
  if (!descriptor || descriptor->num_rel_modules == 0)
    return fail(@"This build needs the Mario Party 7 native module and its minigame modules.");
  moderngekko::RuntimeConfig config;
  config.module = moderngekko::ModuleSource::AttachedDescriptor(descriptor);
  NetPlay::SetCompatibilityFingerprint(moderngekko::frontend::CompatibilityFingerprint(config, game));
  _ui->game = std::make_shared<UICommon::GameFile>((std::string(_root.UTF8String) + "/sys/main.dol"));
  if (!_ui->game->IsValid()) return fail(@"The game could not be opened.");

  Config::SetBase(Config::MAIN_CPU_THREAD, false);
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetBase(Config::MAIN_DSP_JIT, false);
  Config::SetBase(Config::MAIN_EMULATION_SPEED, 1.0f);
  Config::SetBase(Config::MAIN_ENABLE_CHEATS, false);
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, true);
  // Dolphin redirects guests to temporary cards. The host keeps normal game
  // saves; automated device checks leave the user's existing save untouched.
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, getenv("DOLBUNDLER_NEARBY_TEST") == nullptr);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, false);
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, true);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);
  if (host) {
    _server = std::make_unique<NetPlay::NetPlayServer>(0, false, _ui.get(),
        NetPlay::NetTraversalConfig{}, NetPlay::ControllerMode::GameCube, true, std::chrono::seconds(5));
    if (!_server->is_connected) return fail(@"Could not create the nearby room.");
    _server->SetHostInputAuthority(false);
    _server->AdjustPadBufferSize(NetPlay::GameCubeBufferPolicy::MIN_SIZE);
    _server->SetAdaptiveBuffer(true);
    _server->ChangeGame(_ui->game->GetSyncIdentifier(), game.game_name);
    port = _server->GetPort();
  }
  _client = std::make_unique<NetPlay::NetPlayClient>("127.0.0.1", port, _ui.get(),
      nickname.UTF8String, NetPlay::NetTraversalConfig{}, 1, NetPlay::ControllerMode::GameCube,
      std::chrono::seconds(5));
  if (!_client->IsConnected()) {
    std::string message;
    { std::lock_guard lock(_ui->mutex); message = _ui->error; }
    return fail(message.empty() ? @"Could not join this room." : @(message.c_str()));
  }
  { std::lock_guard lock(activeMutex); activeClient = _client.get(); }
  return YES;
}
- (uint16_t)port { return _server ? _server->GetPort() : 0; }
- (NSDictionary*)snapshot {
  if (!_ui || !_client) return @{};
  NSMutableArray* players = [NSMutableArray array];
  const auto mapping = _client->GetPadMappingSnapshot();
  for (const auto& player : _client->GetPlayersSnapshot()) {
    int slot = 0;
    for (int i = 0; i < 4; ++i) if (mapping[i] == player.pid) slot = i + 1;
    [players addObject:@{@"name": @(player.name.c_str()), @"slot": @(slot),
      @"local": @(_client->IsLocalPlayer(player.pid)), @"ready": @(player.ready),
      @"ping": @(player.ping),
      @"match": @(player.game_status == NetPlay::SyncIdentifierComparison::SameGame)}];
  }
  const BOOL canStart = _server && !_startRequested && !_ui->ended && _server->CanStart();
  std::lock_guard lock(_ui->mutex);
  const auto telemetry = NetPlay::NetPlayClient::GetInputWaitTelemetry();
  return @{@"players": players, @"canStart": @(canStart),
    @"buffer": @(_ui->buffer.load()), @"ended": @(_ui->ended.load()),
    @"error": @(_ui->error.c_str()), @"status": @(_ui->status.c_str()),
    @"waitMilliseconds": @(telemetry.total_wait_ns / 1000000),
    @"waitCount": @(telemetry.wait_count), @"longWaitCount": @(telemetry.long_wait_count),
    @"maximumWaitMilliseconds": @(telemetry.maximum_wait_ns / 1000000)};
}
- (void)setReady:(BOOL)ready { if (_client) _client->SetReady(ready); }
- (void)start {
  if (!_server || _startRequested || _ui->ended || !_server->CanStart()) return;
  _startRequested = YES;
  if (!_server->RequestStartGame())
    _ui->Fail("Could not synchronize the host's save and start the game. Create a new room to retry.");
}
- (BOOL)takeBootRequest {
  if (!_client || _bootTaken || _ui->ended) return NO;
  if (_ui->start.exchange(false)) _client->StartGame(std::string(_root.UTF8String) + "/sys/main.dol");
  std::lock_guard lock(_ui->mutex);
  if (!_ui->boot) return NO;
  moderngekko::detail::SetBootSessionData(std::move(_ui->boot));
  _bootTaken = YES;
  return YES;
}
- (void)close {
  if (!_initialized) return;
  { std::lock_guard lock(activeMutex); activeClient = nullptr; }
  if (_client) { _client->Stop(); _client->StopGame(); }
  _client.reset();
  _server.reset();
  moderngekko::detail::SetBootSessionData(nullptr);
  moderngekko::detail::SetExternalUICommon(false);
  UICommon::Shutdown();
  _ui.reset();
  _initialized = NO;
  sessionOpen = false;
}
- (void)dealloc { [self close]; }
@end
