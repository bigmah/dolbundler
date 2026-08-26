// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBGameViewController.h"

#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>

#import "DBLibrary.h"
#import "DBMetalView.h"
#import "DBTouchPadView.h"

#include "dolbundler_run.h"

namespace
{
// A GameCube's displayed image is at most 480 pixels tall. Keep the wide
// iPhone surface's aspect ratio, but do not spend GPU time upscaling that image
// to the screen's native pixel density before Core Animation displays it.
constexpr CGFloat kGameCubeOutputShortSide = 480.0;

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
  UIButton* _closeButton;
  UILabel* _status;
  UIActivityIndicatorView* _spinner;
  BOOL _started;
  // --- diagnostic overlay, remove before release --------------------------
  UILabel* _perfLabel;
  CADisplayLink* _perfLink;
  // -----------------------------------------------------------------------
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

  // Create() spends about twelve seconds verifying the bytecode module against
  // the DOL before anything is drawn. Without this the screen is simply black
  // for that whole time, which reads as a hang.
  _status = [[UILabel alloc] initWithFrame:self.view.bounds];
  _status.text = [NSString stringWithFormat:@"Starting %@\u2026", _game.title];
  _status.textAlignment = NSTextAlignmentCenter;
  _status.textColor = [UIColor colorWithWhite:1.0 alpha:0.55];
  _status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  _status.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:_status];

  _spinner = [[UIActivityIndicatorView alloc]
      initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
  _spinner.color = [UIColor colorWithWhite:1.0 alpha:0.55];
  [_spinner startAnimating];
  [self.view addSubview:_spinner];

  // --- diagnostic overlay, remove before release --------------------------
  // Emulation speed is the number that matters: 100% means the game is running
  // at the rate the hardware did. FPS on its own cannot distinguish a game
  // that renders at 30 by design from one that is running at half speed.
  _perfLabel = [[UILabel alloc] initWithFrame:CGRectZero];
  _perfLabel.font = [UIFont monospacedDigitSystemFontOfSize:12
                                                     weight:UIFontWeightMedium];
  _perfLabel.textColor = [UIColor colorWithWhite:1.0 alpha:0.75];
  _perfLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.35];
  _perfLabel.textAlignment = NSTextAlignmentCenter;
  _perfLabel.layer.cornerRadius = 4;
  _perfLabel.clipsToBounds = YES;
  _perfLabel.text = @"-- fps  --%";
  [self.view addSubview:_perfLabel];
  // -----------------------------------------------------------------------

  _closeButton = [UIButton buttonWithType:UIButtonTypeSystem];
  [_closeButton setImage:[UIImage systemImageNamed:@"xmark.circle.fill"]
                forState:UIControlStateNormal];
  _closeButton.tintColor = [UIColor colorWithWhite:1.0 alpha:0.6];
  [_closeButton addTarget:self
                   action:@selector(stopAndDismiss)
         forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_closeButton];

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
}

- (void)dealloc
{
  [NSNotificationCenter.defaultCenter removeObserver:self];
  // --- diagnostic overlay, remove before release ---
  [_perfLink invalidate];
  // ------------------------------------------------
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

  const CGFloat inset = 16;
  const CGFloat size = 36;
  _closeButton.frame =
      CGRectMake(self.view.safeAreaInsets.left + inset, self.view.safeAreaInsets.top + inset, size,
                 size);
  // --- diagnostic overlay, remove before release ---
  const CGFloat pw = 104, ph = 20;
  _perfLabel.frame = CGRectMake(CGRectGetWidth(self.view.bounds) -
                                    self.view.safeAreaInsets.right - inset - pw,
                                self.view.safeAreaInsets.top + inset, pw, ph);
  // ------------------------------------------------
  _status.frame = self.view.bounds;
  _spinner.center = CGPointMake(CGRectGetMidX(self.view.bounds),
                                CGRectGetMidY(self.view.bounds) + 28);

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

  // --- diagnostic overlay, remove before release ---
  // Twice a second: often enough to watch, rare enough that the label itself
  // does not show up in what it is measuring.
  _perfLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(updatePerformance)];
  _perfLink.preferredFramesPerSecond = 2;
  [_perfLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  // ------------------------------------------------

  DBGameEntry* game = _game;
  NSString* userDir = DBLibrary.shared.userDirectory;

  // Nothing reports "first frame drawn", so this clears once boot has had long
  // enough to reach the point where the game is drawing over it anyway.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(18 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [self->_spinner stopAnimating];
                   self->_status.hidden = YES;
                 });

  // db_run_game() blocks for the whole session, so it gets its own thread.
  // User-interactive QoS: this thread is the emulation, and letting the
  // scheduler treat it as background work shows up immediately as stutter.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
    char err[512] = {0};
    const int ok = db_run_game(game.gameRoot.UTF8String, game.modulePath.UTF8String,
                               userDir.UTF8String, game.title.UTF8String, err, sizeof(err));

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

// --- diagnostic overlay, remove before release ----------------------------
- (void)updatePerformance
{
  double fps = 0, speed = 0;
  db_get_performance(&fps, &speed);
  _perfLabel.text =
      [NSString stringWithFormat:@"%.0f fps  %.0f%%", fps, speed * 100.0];
}
// --------------------------------------------------------------------------

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

- (void)stopAndDismiss
{
  if (db_is_running())
  {
    // The run thread dismisses this controller once Run() returns, so there is
    // nothing to do here but ask.
    db_request_stop();
    return;
  }
  [self dismissViewControllerAnimated:YES completion:nil];
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
