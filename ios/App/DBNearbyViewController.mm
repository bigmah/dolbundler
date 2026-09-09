// SPDX-License-Identifier: GPL-3.0-or-later
#import "DBNearbyViewController.h"
#import "DBNearbyTransport.h"
#import "DBNetplaySession.h"
#import "DBGameViewController.h"
#import "DBLibrary.h"
#import "DBTheme.h"
#include "dolbundler_run.h"

@implementation DBNearbyViewController {
  DBGameEntry* _game;
  DBNearbyTransport* _transport;
  DBNetplaySession* _session;
  dispatch_queue_t _worker;
  NSArray<DBNearbyRoom*>* _rooms;
  NSDictionary* _snapshot;
  NSString* _pendingGameError;
  UITextField* _nickname;
  UILabel* _status;
  UIView* _footer;
  NSTimer* _timer;
  BOOL _hosting, _busy, _connected, _playing, _leaving, _pollPending, _failed;
  NSString* _testRole;
  NSString* _testRoom;
  BOOL _testStarted;
  NSInteger _testCapture;
  NSInteger _testTap;
}
- (instancetype)initWithGame:(DBGameEntry*)game {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    _game = game;
    _rooms = @[];
    _worker = dispatch_queue_create("com.bigmah.dolbundler.nearby.session", DISPATCH_QUEUE_SERIAL);
    _session = [[DBNetplaySession alloc] initWithGameRoot:game.gameRoot userDirectory:DBLibrary.shared.userDirectory];
  }
  return self;
}
- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"Nearby Multiplayer";
  self.navigationController.modalInPresentation = YES;
  self.view.tintColor = DBTheme.accent;
  if (const char* role = getenv("DOLBUNDLER_NEARBY_TEST")) {
    if (!strcmp(role, "host") || !strcmp(role, "join")) {
      _testRole = @(role);
      const char* room = getenv("DOLBUNDLER_NEARBY_ROOM");
      _testRoom = room && *room ? @(room) : @"DolBundler Nearby Test";
    }
  }
  self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Close"
      style:UIBarButtonItemStylePlain target:self action:@selector(leave)];
  _nickname = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 180, 34)];
  _nickname.placeholder = @"Your name";
  _nickname.text = [NSUserDefaults.standardUserDefaults stringForKey:@"NearbyNickname"] ?: @"Player";
  _nickname.textAlignment = NSTextAlignmentRight;
  _nickname.autocorrectionType = UITextAutocorrectionTypeNo;
  _nickname.accessibilityLabel = @"Your player name";
  _status = [[UILabel alloc] initWithFrame:CGRectMake(20, 0, 300, 100)];
  _status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  _status.textColor = UIColor.secondaryLabelColor;
  _status.numberOfLines = 0;
  _status.textAlignment = NSTextAlignmentCenter;
  _footer = [UIView new];
  [_footer addSubview:_status];
  [self setStatus:@"Keep Wi-Fi on and stay in the app. Internet isn’t required. Each phone needs the same Mario Party 7 build and disc version."];
  _transport = [DBNearbyTransport new];
  __weak DBNearbyViewController* weakSelf = self;
  _transport.roomsChanged = ^(NSArray<DBNearbyRoom*>* rooms) {
    DBNearbyViewController* self = weakSelf;
    if (!self || self->_busy || self->_connected || self->_leaving || self->_failed) return;
    self->_rooms = rooms;
    [self.tableView reloadData];
    if ([self->_testRole isEqualToString:@"join"] && !self->_testStarted) {
      for (NSUInteger i = 0; i < rooms.count; ++i) {
        if ([rooms[i].name isEqualToString:self->_testRoom]) {
          self->_testStarted = YES;
          [self tableView:self.tableView didSelectRowAtIndexPath:[NSIndexPath indexPathForRow:i inSection:1]];
          break;
        }
      }
    }
  };
  _transport.failed = ^(NSString* message) { [weakSelf fail:message]; };
  _transport.statusChanged = ^(NSString* message) {
    DBNearbyViewController* self = weakSelf;
    if (self && !self->_playing && !self->_leaving && !self->_failed) [self setStatus:message];
  };
  [_transport browse];
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(background)
      name:UIApplicationDidEnterBackgroundNotification object:nil];
}
- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  if ([_testRole isEqualToString:@"host"] && !_testStarted) {
    _testStarted = YES;
    [self tableView:self.tableView didSelectRowAtIndexPath:[NSIndexPath indexPathForRow:1 inSection:0]];
  }
}
- (void)setStatus:(NSString*)message {
  _status.text = message;
  [self layoutFooter];
}
- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [self layoutFooter];
}
- (void)layoutFooter {
  const CGFloat width = self.tableView.bounds.size.width;
  CGSize size = [_status sizeThatFits:CGSizeMake(MAX(1, width - 48), CGFLOAT_MAX)];
  _status.frame = CGRectMake(24, 20, MAX(1, width - 48), size.height);
  if (self.tableView.tableFooterView != _footer || _footer.bounds.size.height != size.height + 40 ||
      _footer.bounds.size.width != width) {
    _footer.frame = CGRectMake(0, 0, width, size.height + 40);
    self.tableView.tableFooterView = _footer;
  }
}
- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView { return 2; }
- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  if (_connected) return section == 0 ? [_snapshot[@"players"] count] : (_hosting ? 2 : 1);
  return section == 0 ? 2 : MAX(1, _rooms.count);
}
- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  if (_connected) return section == 0 ? @"Players" : @"Ready to play";
  return section == 0 ? @"Mario Party 7" : @"Join a nearby room";
}
- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
  if (section != 0) return nil;
  return _connected ? @"One player per phone, up to four. Everyone must be ready. The host keeps saved progress. Turn off microphone minigames in Mario Party 7."
                    : @"Host a room, or ask a friend to host and select their room below.";
}
- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)path {
  UITableViewCell* cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
  cell.textLabel.numberOfLines = 2;
  cell.detailTextLabel.numberOfLines = 2;
  if (_connected) {
    if (path.section == 0) {
      NSDictionary* player = _snapshot[@"players"][path.row];
      cell.textLabel.text = [NSString stringWithFormat:@"P%@ · %@%@", player[@"slot"], player[@"name"],
                             [player[@"local"] boolValue] ? @" (you)" : @""];
      cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · %@ ms",
        ![player[@"match"] boolValue] ? @"Checking game" : [player[@"ready"] boolValue] ? @"Ready" : @"Not ready",
        player[@"ping"]];
      cell.accessoryType = [player[@"ready"] boolValue] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
    } else {
      BOOL ready = [self localReady];
      cell.textLabel.text = path.row == 0 ? (ready ? @"Not Ready" : @"Ready") : @"Start Game";
      BOOL enabled = path.row == 0 || [_snapshot[@"canStart"] boolValue];
      cell.textLabel.textColor = enabled ? DBTheme.accent : UIColor.tertiaryLabelColor;
      cell.userInteractionEnabled = enabled;
    }
  } else if (path.section == 0) {
    cell.textLabel.text = path.row == 0 ? @"Name" : (_busy ? @"Connecting…" : @"Host Nearby Room");
    if (path.row == 0) { cell.accessoryView = _nickname; _nickname.enabled = !_busy; }
    else cell.textLabel.textColor = _busy ? UIColor.secondaryLabelColor : DBTheme.accent;
    cell.userInteractionEnabled = !_busy;
  } else if (_rooms.count) {
    cell.textLabel.text = _rooms[path.row].name;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    cell.userInteractionEnabled = !_busy;
  } else {
    cell.textLabel.text = @"Looking for rooms…";
    cell.textLabel.textColor = UIColor.secondaryLabelColor;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
  }
  return cell;
}
- (BOOL)localReady {
  for (NSDictionary* player in _snapshot[@"players"])
    if ([player[@"local"] boolValue]) return [player[@"ready"] boolValue];
  return NO;
}
- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)path {
  [tableView deselectRowAtIndexPath:path animated:YES];
  if (_leaving || _busy || _failed) return;
  if (_connected) {
    if (path.section == 1) {
      const BOOL ready = ![self localReady];
      if (path.row == 0) dispatch_async(_worker, ^{ [self->_session setReady:ready]; });
      else {
        [self setStatus:@"Synchronizing the host’s save and starting…"];
        dispatch_async(_worker, ^{ [self->_session start]; });
      }
    }
    return;
  }
  if (path.section == 0 && path.row == 0) { [_nickname becomeFirstResponder]; return; }
  if (path.section == 1 && !_rooms.count) return;
  [self.view endEditing:YES];
  NSString* name = [_nickname.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (!name.length) name = @"Player";
  if (name.length > 20) name = [name substringWithRange:[name rangeOfComposedCharacterSequencesForRange:NSMakeRange(0, 20)]];
  _nickname.text = name;
  if (!_testRole) [NSUserDefaults.standardUserDefaults setObject:name forKey:@"NearbyNickname"];
  _busy = YES;
  _hosting = path.section == 0;
  UIApplication.sharedApplication.idleTimerDisabled = YES;
  [self setStatus:@"Checking game files. This can take a moment…"];
  [self.tableView reloadData];
  if (_hosting) [self openPort:0 name:name];
  else {
    __weak DBNearbyViewController* weakSelf = self;
    [_transport joinEndpoint:_rooms[path.row].endpoint ready:^(uint16_t port) { [weakSelf openPort:port name:name]; }];
  }
}
- (void)openPort:(uint16_t)port name:(NSString*)name {
  if (_leaving || _failed) return;
  BOOL host = _hosting;
  dispatch_async(_worker, ^{
    NSString* error = nil;
    BOOL ok = [self->_session openAsHost:host port:port nickname:name error:&error];
    uint16_t serverPort = self->_session.port;
    dispatch_async(dispatch_get_main_queue(), ^{
      // Verification can finish after the transport has already failed and
      // queued session cleanup. Its reply must not revive the closed lobby.
      if (self->_leaving || self->_failed) return;
      if (!ok) { [self fail:error]; return; }
      self->_connected = YES;
      self->_busy = NO;
      if (host) {
        NSString* roomName = self->_testRoom ?: [NSString stringWithFormat:@"%@ · %@", name, [NSUUID.UUID.UUIDString substringToIndex:4]];
        [self->_transport hostWithName:roomName serverPort:serverPort ready:^(uint16_t advertisedPort) {
          if (!self->_leaving && !self->_failed) [self setStatus:[NSString stringWithFormat:@"Room: %@\nAsk friends to join, then mark yourself ready.", roomName]];
        }];
      } else [self setStatus:@"Mark yourself ready. The host starts once everyone is ready."];
      __weak DBNearbyViewController* weakSelf = self;
      self->_timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* timer) { [weakSelf poll]; }];
      [self poll];
    });
  });
}
- (void)poll {
  if (_pollPending || _leaving || _failed) return;
  _pollPending = YES;
  dispatch_async(_worker, ^{
    NSDictionary* snapshot = [self->_session snapshot];
    if (self->_testRole) {
      for (NSDictionary* player in snapshot[@"players"])
        if ([player[@"local"] boolValue] && [player[@"match"] boolValue] && ![player[@"ready"] boolValue])
          [self->_session setReady:YES];
      if (self->_hosting && [snapshot[@"canStart"] boolValue]) [self->_session start];
    }
    BOOL boot = [self->_session takeBootRequest];
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_pollPending = NO;
      if (self->_leaving || self->_failed) return;
      self->_snapshot = snapshot;
      if (!self->_playing && [snapshot[@"status"] length]) [self setStatus:snapshot[@"status"]];
      if (self->_testRole) {
        [self applyTestControls];
        NSMutableDictionary* sample = [snapshot mutableCopy];
        double fps = 0, speed = 0;
        db_get_performance(&fps, &speed);
        sample[@"fps"] = @(fps);
        sample[@"speed"] = @(speed);
        sample[@"playing"] = @(self->_playing);
        sample[@"time"] = @(NSDate.timeIntervalSinceReferenceDate);
        NSData* json = [NSJSONSerialization dataWithJSONObject:sample options:0 error:nil];
        NSString* path = [DBLibrary.shared.userDirectory stringByAppendingPathComponent:@"nearby-test.jsonl"];
        FILE* log = fopen(path.fileSystemRepresentation, "a");
        if (log) { fwrite(json.bytes, 1, json.length, log); fputc('\n', log); fclose(log); }
      }
      if (!self->_playing) [self.tableView reloadData];
      NSString* error = snapshot[@"error"];
      if (error.length && !self->_playing) { [self fail:error]; return; }
      if ([snapshot[@"ended"] boolValue] && !self->_playing) {
        [self fail:@"The nearby game ended before it could start. Create a new room to try again."];
        return;
      }
      if (boot && !self->_playing) {
        self->_playing = YES;
        DBGameViewController* game = [[DBGameViewController alloc] initWithGame:self->_game];
        __weak DBNearbyViewController* weakSelf = self;
        game.nearbyCompletion = ^(NSString* message) { [weakSelf gameFinished:message]; };
        game.modalPresentationStyle = UIModalPresentationFullScreen;
        [self presentViewController:game animated:YES completion:nil];
      }
    });
  });
}
- (void)applyTestControls {
  if (!_playing) return;
  // Explicit device-test mode only. This lets a Mac drive real controller
  // inputs through the normal synchronized path, without changing game RAM.
  NSString* path = [DBLibrary.shared.userDirectory stringByAppendingPathComponent:@"nearby-test-controls.json"];
  NSData* data = [NSData dataWithContentsOfFile:path];
  id controls = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
  if (![controls isKindOfClass:NSDictionary.class]) return;
  NSArray<NSString*>* keys = @[@"a", @"b", @"x", @"y", @"z", @"start", @"up", @"down",
      @"left", @"right", @"l", @"r", @"l_analog", @"r_analog", @"stick_x", @"stick_y", @"c_x", @"c_y"];
  for (NSUInteger i = 0; i < keys.count; ++i) {
    id value = controls[keys[i]];
    if ([value isKindOfClass:NSNumber.class])
      db_set_control((DBPadControl)i, MAX(i >= DB_PAD_MAIN_STICK_X ? -1.0 : 0.0, MIN(1.0, [value doubleValue])));
    else db_clear_control((DBPadControl)i);
  }
  // A held value lasts until the next file poll. Short taps need an explicit
  // release so menu directions do not repeat during that half-second interval.
  NSInteger tap = [controls[@"tapSequence"] respondsToSelector:@selector(integerValue)] ?
      [controls[@"tapSequence"] integerValue] : 0;
  NSUInteger key = [keys indexOfObject:controls[@"tap"] ?: @""];
  if (tap > _testTap && key < 12) {
    _testTap = tap;
    db_set_control((DBPadControl)key, 1.0);
    __weak DBNearbyViewController* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      DBNearbyViewController* self = weakSelf;
      if (self && self->_playing) db_clear_control((DBPadControl)key);
    });
  }
  NSInteger capture = [controls[@"capture"] respondsToSelector:@selector(integerValue)] ? [controls[@"capture"] integerValue] : 0;
  if (capture > _testCapture) { _testCapture = capture; db_request_screenshot(); }
}
- (void)gameFinished:(NSString*)message {
  [self recordTestEvent:@"gameFinished" message:message];
  _playing = NO;
  NSString* error = _pendingGameError.length ? _pendingGameError : message.length ? message : _snapshot[@"error"];
  [self fail:error.length ? error : @"The nearby game has ended. Create a new room to play again."];
}
- (void)fail:(NSString*)message {
  if (_failed || _leaving) return;
  [self recordTestEvent:@"failure" message:message];
  if (_playing) {
    // Runtime completion is asynchronous and may have no error of its own.
    // Keep the transport's reason until the game view has dismissed.
    if (!_pendingGameError.length) _pendingGameError = [message copy];
    db_request_stop();
    return;
  }
  _failed = YES;
  [_timer invalidate];
  [_transport stop];
  dispatch_async(_worker, ^{ [self->_session close]; });
  UIApplication.sharedApplication.idleTimerDisabled = NO;
  UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Nearby Multiplayer"
      message:message ?: @"Could not connect." preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self leave]; }]];
  [self presentViewController:alert animated:YES completion:nil];
}
- (void)background {
  [self recordTestEvent:@"background" message:nil];
  if (_playing) { db_request_stop(); return; }
  [self leave];
}
- (void)leave {
  if (_leaving) return;
  [self recordTestEvent:@"leave" message:nil];
  _leaving = YES;
  [_timer invalidate];
  [_transport stop];
  UIApplication.sharedApplication.idleTimerDisabled = NO;
  dispatch_async(_worker, ^{ [self->_session close]; });
  [self dismissViewControllerAnimated:YES completion:nil];
}
- (void)recordTestEvent:(NSString*)event message:(NSString*)message {
  if (!_testRole) return;
  NSDictionary* entry = @{@"event": event, @"message": message ?: @"",
      @"playing": @(_playing), @"time": @(NSDate.timeIntervalSinceReferenceDate)};
  NSData* json = [NSJSONSerialization dataWithJSONObject:entry options:0 error:nil];
  NSString* path = [DBLibrary.shared.userDirectory stringByAppendingPathComponent:@"nearby-test.jsonl"];
  FILE* log = fopen(path.fileSystemRepresentation, "a");
  if (log) { fwrite(json.bytes, 1, json.length, log); fputc('\n', log); fclose(log); }
}
- (void)dealloc {
  [NSNotificationCenter.defaultCenter removeObserver:self];
  [_timer invalidate];
}
@end
