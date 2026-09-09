// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real UIKit lobby with controlled asynchronous session replies.
// The fakes replace networking and emulation, not the controller under test.
#import "DBNearbyViewController.h"
#import "DBNearbyTransport.h"
#import "DBNetplaySession.h"
#import "DBGameViewController.h"
#import "DBLibrary.h"
#import "DBTheme.h"
#include "dolbundler_run.h"
#include <atomic>

static int failures = 0;
static int stopRequests = 0;
static void Check(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static bool WaitFor(BOOL (^condition)(void)) {
  NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:2];
  while (!condition() && deadline.timeIntervalSinceNow > 0)
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
  return condition();
}

@interface DBNetplaySession ()
@property(nonatomic, strong) dispatch_semaphore_t gate;
@property(nonatomic) BOOL blockOpen;
@property(nonatomic) BOOL blockSnapshot;
@property(nonatomic) BOOL wantsBoot;
@property(nonatomic, readonly) int opens;
@property(nonatomic, readonly) int snapshots;
@property(nonatomic, readonly) int closes;
@end
@implementation DBNetplaySession {
  std::atomic<int> _opens, _snapshots, _closes;
}
- (instancetype)initWithGameRoot:(NSString*)root userDirectory:(NSString*)directory {
  if ((self = [super init])) _gate = dispatch_semaphore_create(0);
  return self;
}
- (BOOL)openAsHost:(BOOL)host port:(uint16_t)port nickname:(NSString*)nickname error:(NSString**)error {
  ++_opens;
  if (_blockOpen) dispatch_semaphore_wait(_gate, DISPATCH_TIME_FOREVER);
  return YES;
}
- (uint16_t)port { return 1234; }
- (NSDictionary*)snapshot {
  ++_snapshots;
  if (_blockSnapshot) dispatch_semaphore_wait(_gate, DISPATCH_TIME_FOREVER);
  return @{@"players": @[], @"error": @"", @"ended": @NO, @"canStart": @NO};
}
- (BOOL)takeBootRequest { return _wantsBoot; }
- (void)setReady:(BOOL)ready {}
- (void)start {}
- (void)close { ++_closes; }
- (int)opens { return _opens.load(); }
- (int)snapshots { return _snapshots.load(); }
- (int)closes { return _closes.load(); }
@end

@implementation DBNearbyRoom
@end
@implementation DBNearbyTransport
- (void)browse {}
- (void)hostWithName:(NSString*)name serverPort:(uint16_t)port ready:(void (^)(uint16_t))ready {
  if (ready) ready(port);
}
- (void)joinEndpoint:(nw_endpoint_t)endpoint ready:(void (^)(uint16_t))ready {
  if (ready) ready(1234);
}
- (void)stop {}
@end

// These dependencies are deliberately inert; the tests never import or boot a disc.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincomplete-implementation"
#pragma clang diagnostic ignored "-Wobjc-property-implementation"
@implementation DBGameEntry
@end
@implementation DBLibrary
+ (instancetype)shared {
  static DBLibrary* instance;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ instance = [DBLibrary new]; });
  return instance;
}
- (NSString*)userDirectory { return NSTemporaryDirectory(); }
@end
@implementation DBTheme
+ (UIColor*)accent { return UIColor.systemBlueColor; }
@end
#pragma clang diagnostic pop
@implementation DBGameViewController
- (instancetype)initWithGame:(DBGameEntry*)game { return [super init]; }
@end
void db_request_stop(void) { ++stopRequests; }
void db_get_performance(double* fps, double* speed) { *fps = *speed = 0; }
void db_set_control(DBPadControl control, double state) {}
void db_clear_control(DBPadControl control) {}
void db_request_screenshot(void) {}

@interface DBNearbyViewController (TestAccess)
- (void)openPort:(uint16_t)port name:(NSString*)name;
- (void)poll;
- (void)fail:(NSString*)message;
- (void)gameFinished:(NSString*)message;
- (void)leave;
@end
@interface TestLobby : DBNearbyViewController
@property(nonatomic) int gamesPresented;
@property(nonatomic, copy) NSString* presentedError;
@end
@implementation TestLobby
- (void)presentViewController:(UIViewController*)controller animated:(BOOL)animated
                  completion:(void (^)(void))completion {
  if ([controller isKindOfClass:DBGameViewController.class]) ++_gamesPresented;
  if ([controller isKindOfClass:UIAlertController.class])
    _presentedError = ((UIAlertController*)controller).message;
  if (completion) completion();
}
- (void)dismissViewControllerAnimated:(BOOL)animated completion:(void (^)(void))completion {
  if (completion) completion();
}
@end

static TestLobby* NewLobby() {
  DBGameEntry* game = [DBGameEntry new];
  game.gameRoot = @"unused-test-game";
  TestLobby* lobby = [[TestLobby alloc] initWithGame:game];
  [lobby loadViewIfNeeded];
  return lobby;
}
static void Drain(TestLobby* lobby) {
  // Session work posts its completion to main before this barrier does.
  __block BOOL drained = NO;
  dispatch_async((dispatch_queue_t)[lobby valueForKey:@"worker"], ^{
    dispatch_async(dispatch_get_main_queue(), ^{ drained = YES; });
  });
  Check(WaitFor(^{ return drained; }), "worker/main completion barrier timed out");
}
static void TestLateOpen() {
  TestLobby* lobby = NewLobby();
  DBNetplaySession* session = [lobby valueForKey:@"session"];
  session.blockOpen = YES;
  [lobby openPort:1234 name:@"Test"];
  Check(WaitFor(^{ return session.opens == 1; }), "verification did not begin");
  [lobby fail:@"Connection failed during verification"];
  dispatch_semaphore_signal(session.gate);
  Drain(lobby);
  Check(![[lobby valueForKey:@"connected"] boolValue], "late verification reopened a failed lobby");
  Check(![(NSTimer*)[lobby valueForKey:@"timer"] isValid], "late verification restarted polling");
  Check(session.closes == 1, "failed verification did not close its session once");
  [lobby leave];
  Drain(lobby);
}
static void TestLateBoot() {
  TestLobby* lobby = NewLobby();
  DBNetplaySession* session = [lobby valueForKey:@"session"];
  session.blockSnapshot = YES;
  session.wantsBoot = YES;
  [lobby poll];
  Check(WaitFor(^{ return session.snapshots == 1; }), "lobby poll did not begin");
  [lobby fail:@"Connection failed before boot handoff"];
  dispatch_semaphore_signal(session.gate);
  Drain(lobby);
  Check(lobby.gamesPresented == 0, "late poll presented a game after failure");
  Check(![[lobby valueForKey:@"playing"] boolValue], "late poll resumed gameplay after failure");
  [lobby leave];
  Drain(lobby);
}
static void TestLateConnection() {
  TestLobby* lobby = NewLobby();
  DBNetplaySession* session = [lobby valueForKey:@"session"];
  [lobby fail:@"Connection timed out"];
  [lobby openPort:1234 name:@"Late connection callback"];
  Drain(lobby);
  Check(session.opens == 0, "late connection callback opened a failed session");
  [lobby leave];
  Drain(lobby);
}
static void TestGameFailureMessage() {
  TestLobby* lobby = NewLobby();
  [lobby setValue:@YES forKey:@"playing"];
  const int before = stopRequests;
  [lobby fail:@"The nearby connection stopped responding"];
  Check(stopRequests == before + 1, "transport failure did not request runtime shutdown");
  [lobby gameFinished:nil];
  Check([lobby.presentedError isEqualToString:@"The nearby connection stopped responding"],
        "runtime completion discarded the transport failure message");
  [lobby leave];
  Drain(lobby);
}
static void TestNormalOpen() {
  TestLobby* lobby = NewLobby();
  [lobby openPort:1234 name:@"Working connection"];
  Check(WaitFor(^{ return [[lobby valueForKey:@"connected"] boolValue]; }),
        "successful verification did not open the lobby");
  Check([(NSTimer*)[lobby valueForKey:@"timer"] isValid], "successful lobby did not begin polling");
  [lobby leave];
  Drain(lobby);
}

@interface LifecycleTestApp : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end
@implementation LifecycleTestApp
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [UIViewController new];
  [self.window makeKeyAndVisible];
  // A run-loop timer lets WaitFor drain main-queue callbacks. Running the
  // suite inside a main-queue block would prevent that queue from reentering.
  [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:NO block:^(NSTimer* timer) {
    TestLateOpen();
    TestLateBoot();
    TestLateConnection();
    TestGameFailureMessage();
    TestNormalOpen();
    printf("Nearby lifecycle tests: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    fflush(stdout);
    exit(failures ? 1 : 0);
  }];
  return YES;
}
@end
int main(int argc, char** argv) {
  @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(LifecycleTestApp.class)); }
}
