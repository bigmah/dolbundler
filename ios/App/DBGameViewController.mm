// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBGameViewController.h"

#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>

#import "DBLibrary.h"
#import "DBMetalView.h"
#import "DBPauseMenuView.h"
#import "DBSettings.h"
#import "DBTouchPadView.h"

#include "dolbundler_run.h"

namespace
{
// A GameCube's displayed image is at most 480 pixels tall. Keep the wide
// iPhone surface's aspect ratio, but do not spend GPU time upscaling that image
// to the screen's native pixel density before Core Animation displays it.
constexpr CGFloat kGameCubeOutputShortSide = 480.0;

// How long the corner button stays at full strength before fading back. Long
// enough to find it after the game starts, short enough that it is out of the
// way by the time anyone is playing.
constexpr NSTimeInterval kHUDIdleDelay = 4.0;
constexpr CGFloat kHUDRestingAlpha = 0.22;

CGFloat GameDrawableScale(CGSize bounds)
{
  const CGFloat nativeScale = UIScreen.mainScreen.nativeScale;
  if (UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPhone)
    return nativeScale;

  const CGFloat shortSide = MIN(bounds.width, bounds.height);
  if (shortSide <= 0)
    return nativeScale;
  return MIN(nativeScale, kGameCubeOutputShortSide / shortSide);
}
}  // namespace

@implementation DBMetalView
+ (Class)layerClass
{
  return [CAMetalLayer class];
}
@end

@implementation DBGameViewController
{
  DBGameEntry* _game;
  DBMetalView* _metalView;
  DBTouchPadView* _pad;
  UIButton* _menuButton;
  DBPauseMenuView* _menu;
  BOOL _started;
  BOOL _pausedByBackground;

  UILabel* _perfLabel;
  CADisplayLink* _perfLink;
}

- (instancetype)initWithGame:(DBGameEntry*)game
{
  self = [super initWithNibName:nil bundle:nil];
  if (self)
    _game = game;
  return self;
}

- (void)viewWillAppear:(BOOL)animated
{
  [super viewWillAppear:animated];

  // Returning a landscape mask from supportedInterfaceOrientations is not
  // enough on iOS 16 and later: a modal does not re-evaluate orientation on
  // its own, so the scene has to be asked to change geometry. Without this the
  // game renders sideways inside a portrait frame.
  if (@available(iOS 16.0, *))
  {
    UIWindowScene* scene = self.view.window.windowScene;
    if (scene)
    {
      UIWindowSceneGeometryPreferencesIOS* prefs =
          [[UIWindowSceneGeometryPreferencesIOS alloc]
              initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
      [scene requestGeometryUpdateWithPreferences:prefs errorHandler:nil];
    }
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
  }
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.blackColor;

  _metalView = [[DBMetalView alloc] initWithFrame:self.view.bounds];
  _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:_metalView];

  _pad = [[DBTouchPadView alloc] initWithFrame:self.view.bounds];
  _pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:_pad];

  // Added after the pad so it wins the hit test: the pad covers the whole
  // screen, and a menu button underneath it could never be tapped.
  [self buildHUD];

  // Hide the overlay whenever a real controller is attached, and bring it back
  // when the last one disconnects.
  [self updatePadVisibility];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(updatePadVisibility)
                                             name:GCControllerDidConnectNotification
                                           object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(updatePadVisibility)
                                             name:GCControllerDidDisconnectNotification
                                           object:nil];

  // A game left running while the app is in the background burns the battery
  // on frames nobody is looking at, and comes back having advanced through
  // whatever happened in the meantime.
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(applicationDidEnterBackground)
                                             name:UIApplicationDidEnterBackgroundNotification
                                           object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(applicationDidBecomeActive)
                                             name:UIApplicationDidBecomeActiveNotification
                                           object:nil];
}

- (void)buildHUD
{
  UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
  config.image = [UIImage systemImageNamed:@"ellipsis"];
  config.baseForegroundColor = UIColor.whiteColor;
  config.contentInsets = NSDirectionalEdgeInsetsMake(10, 10, 10, 10);

  _menuButton = [UIButton buttonWithConfiguration:config primaryAction:nil];
  _menuButton.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.45];
  _menuButton.layer.cornerRadius = 17;
  _menuButton.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;
  _menuButton.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.22].CGColor;
  _menuButton.accessibilityLabel = @"Game menu";
  [_menuButton addTarget:self
                  action:@selector(showMenu)
        forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_menuButton];

  // Emulation speed is the number that matters: 100% means the game is running
  // at the rate the hardware did. FPS on its own cannot distinguish a game
  // that renders at 30 by design from one that is running at half speed.
  _perfLabel = [[UILabel alloc] initWithFrame:CGRectZero];
  _perfLabel.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightSemibold];
  _perfLabel.textColor = [UIColor colorWithWhite:1.0 alpha:0.85];
  _perfLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.45];
  _perfLabel.textAlignment = NSTextAlignmentCenter;
  _perfLabel.layer.cornerRadius = 9;
  _perfLabel.layer.cornerCurve = kCACornerCurveContinuous;
  _perfLabel.clipsToBounds = YES;
  _perfLabel.text = @"-- fps   --%";
  _perfLabel.hidden = !DBSettings.shared.showsPerformance;
  [self.view addSubview:_perfLabel];
}

- (void)dealloc
{
  [NSNotificationCenter.defaultCenter removeObserver:self];
  [NSObject cancelPreviousPerformRequestsWithTarget:self];
  [_perfLink invalidate];
}

- (void)updatePadVisibility
{
  dispatch_async(dispatch_get_main_queue(), ^{
    self->_pad.hidden = GCController.controllers.count > 0;
  });
}

- (void)viewDidLayoutSubviews
{
  [super viewDidLayoutSubviews];

  // Both of these live at the top centre rather than in the corners, because
  // the corners are where the shoulder buttons are. L and R belong under the
  // index fingers and cannot move; the top middle of a landscape screen is the
  // one strip of the overlay with nothing in it.
  const CGFloat inset = 14;
  const CGFloat size = 34;
  const CGFloat gap = 8;
  const CGFloat perfWidth = 108, perfHeight = 22;
  const CGFloat top = self.view.safeAreaInsets.top + inset;

  const CGFloat groupWidth = size + (_perfLabel.hidden ? 0 : gap + perfWidth);
  const CGFloat originX = round(CGRectGetMidX(self.view.bounds) - groupWidth / 2);

  _menuButton.frame = CGRectMake(originX, top, size, size);
  _perfLabel.frame = CGRectMake(originX + size + gap, top + (size - perfHeight) / 2, perfWidth,
                                perfHeight);

  CAMetalLayer* layer = (CAMetalLayer*)_metalView.layer;
  layer.contentsScale = GameDrawableScale(_metalView.bounds.size);
  layer.drawableSize = CGSizeMake(CGRectGetWidth(_metalView.bounds) * layer.contentsScale,
                                  CGRectGetHeight(_metalView.bounds) * layer.contentsScale);
}

- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];
  if (_started)
    return;
  _started = YES;

  // A running game must not let the screen dim; there is no input for iOS to
  // notice while someone is only holding the on-screen stick.
  UIApplication.sharedApplication.idleTimerDisabled = YES;

  CAMetalLayer* layer = (CAMetalLayer*)_metalView.layer;
  db_set_render_layer((__bridge void*)layer, layer.contentsScale);

  // Twice a second: often enough to watch, rare enough that the label itself
  // does not show up in what it is measuring.
  _perfLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(updatePerformance)];
  _perfLink.preferredFramesPerSecond = 2;
  [_perfLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];

  [self scheduleHUDFade];

  DBGameEntry* game = _game;
  NSString* userDir = DBLibrary.shared.userDirectory;

  // db_run_game() blocks for the whole session, so it gets its own thread.
  // User-interactive QoS: this thread is the emulation, and letting the
  // scheduler treat it as background work shows up immediately as stutter.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
    char err[512] = {0};
    const int ok = db_run_game(game.gameRoot.UTF8String, userDir.UTF8String,
                               game.title.UTF8String, err, sizeof(err));

    NSString* message = ok ? nil : @(err);
    dispatch_async(dispatch_get_main_queue(), ^{
      UIApplication.sharedApplication.idleTimerDisabled = NO;
      if (message)
        [self showFailure:message];
      else
        [self dismissViewControllerAnimated:YES completion:nil];
    });
  });
}

#pragma mark - HUD

- (void)scheduleHUDFade
{
  [NSObject cancelPreviousPerformRequestsWithTarget:self
                                           selector:@selector(fadeHUD)
                                             object:nil];
  [self performSelector:@selector(fadeHUD) withObject:nil afterDelay:kHUDIdleDelay];
}

- (void)fadeHUD
{
  [UIView animateWithDuration:0.4
                   animations:^{
                     self->_menuButton.alpha = kHUDRestingAlpha;
                   }];
}

- (void)wakeHUD
{
  [NSObject cancelPreviousPerformRequestsWithTarget:self
                                           selector:@selector(fadeHUD)
                                             object:nil];
  _menuButton.alpha = 1.0;
}

- (void)updatePerformance
{
  if (_perfLabel.hidden)
    return;
  double fps = 0, speed = 0;
  db_get_performance(&fps, &speed);
  _perfLabel.text = [NSString stringWithFormat:@"%.0f fps   %.0f%%", fps, speed * 100.0];
}

#pragma mark - Menu

- (void)showMenu
{
  if (_menu)
    return;
  [self wakeHUD];

  // Pause before the panel is on screen. Presenting first would let the game
  // run for the length of the animation with its controls already covered.
  db_set_paused(1);

  _menu = [[DBPauseMenuView alloc] initWithTitle:_game.title];
  __weak __typeof(self) weakSelf = self;
  _menu.onSettingsChanged = ^{
    [weakSelf applySettings];
  };
  _menu.onResume = ^{
    [weakSelf dismissMenuAndResume];
  };
  _menu.onQuit = ^{
    [weakSelf quitGame];
  };
  [_menu presentInView:self.view];
  _menuButton.hidden = YES;
}

- (void)dismissMenuAndResume
{
  if (!_menu)
    return;
  DBPauseMenuView* menu = _menu;
  _menu = nil;
  [menu dismissWithCompletion:^{
    self->_menuButton.hidden = NO;
    [self scheduleHUDFade];
  }];
  db_set_paused(0);
}

- (void)applySettings
{
  [_pad refreshFromSettings];
  _perfLabel.hidden = !DBSettings.shared.showsPerformance;
  // Hiding the readout changes how wide the HUD group is, and so where its
  // centre falls.
  [self.view setNeedsLayout];
}

- (void)quitGame
{
  DBPauseMenuView* menu = _menu;
  _menu = nil;
  [menu dismissWithCompletion:nil];

  if (db_is_running())
  {
    // Resume first: a paused core cannot process the shutdown it is being
    // asked for, and the run thread is what dismisses this controller once
    // Run() returns.
    db_set_paused(0);
    db_request_stop();
    return;
  }
  [self dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark - Lifecycle

- (void)applicationDidEnterBackground
{
  if (!db_is_running() || db_is_paused())
    return;
  db_set_paused(1);
  _pausedByBackground = YES;
}

- (void)applicationDidBecomeActive
{
  if (!_pausedByBackground)
    return;
  _pausedByBackground = NO;
  // Not while the menu is up: someone who opened it before switching away
  // still has it open, and resuming underneath it is exactly what the menu
  // exists to prevent.
  if (!_menu)
    db_set_paused(0);
}

- (void)showFailure:(NSString*)message
{
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:@"Could not start the game"
                                          message:message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Back to library"
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction* action) {
                                            [self dismissViewControllerAnimated:YES completion:nil];
                                          }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
  return UIInterfaceOrientationMaskLandscape;
}

- (BOOL)prefersStatusBarHidden
{
  return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden
{
  return YES;
}

@end
