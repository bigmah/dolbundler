// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBGameViewController.h"

#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>

#import "DBLibrary.h"
#import "DBMetalView.h"
#import "DBPauseMenuView.h"
#import "DBSettings.h"
#import "DBTheme.h"
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

  // The layout editor's toolbar, built the first time it is needed.
  UIView* _editBar;
  UISlider* _editScale;
  BOOL _editing;
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

- (UIButton*)editBarButton:(NSString*)title
                     image:(NSString*)symbol
                    filled:(BOOL)filled
                    action:(SEL)action
{
  UIButtonConfiguration* config = filled ? [UIButtonConfiguration filledButtonConfiguration]
                                         : [UIButtonConfiguration grayButtonConfiguration];
  config.title = title;
  if (symbol)
  {
    config.image = [UIImage systemImageNamed:symbol
                           withConfiguration:[UIImageSymbolConfiguration
                                                 configurationWithPointSize:12
                                                                     weight:UIImageSymbolWeightSemibold]];
    config.imagePadding = 5;
  }
  config.baseBackgroundColor = filled ? DBTheme.accent : [UIColor colorWithWhite:1.0 alpha:0.14];
  config.baseForegroundColor = UIColor.whiteColor;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.contentInsets = NSDirectionalEdgeInsetsMake(7, 13, 7, 13);
  config.titleTextAttributesTransformer = ^NSDictionary*(NSDictionary* incoming) {
    NSMutableDictionary* attrs = [incoming mutableCopy];
    attrs[NSFontAttributeName] = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    return attrs;
  };
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
}

// The toolbar the layout editor puts where the HUD was: reset, the size
// slider, and the way out. Everything it needs is in one strip at the top
// centre, which is the one part of the screen the default layout leaves empty
// -- and it stays there whatever the controls have been dragged to.
- (void)buildEditBar
{
  _editBar = [[UIView alloc] init];
  _editBar.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.72];
  _editBar.layer.cornerRadius = 24;
  _editBar.layer.cornerCurve = kCACornerCurveContinuous;
  _editBar.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;
  _editBar.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.22].CGColor;
  [self.view addSubview:_editBar];

  UILabel* hint = [[UILabel alloc] init];
  hint.text = @"Drag any control";
  hint.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
  hint.textColor = [UIColor colorWithWhite:1.0 alpha:0.7];

  UILabel* sizeLabel = [[UILabel alloc] init];
  sizeLabel.text = @"Size";
  sizeLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
  sizeLabel.textColor = [UIColor colorWithWhite:1.0 alpha:0.7];

  _editScale = [[UISlider alloc] init];
  _editScale.minimumValue = DBSettings.minPadScale;
  _editScale.maximumValue = DBSettings.maxPadScale;
  _editScale.value = DBSettings.shared.padScale;
  _editScale.minimumTrackTintColor = DBTheme.accent;
  [_editScale addTarget:self
                 action:@selector(editScaleChanged)
       forControlEvents:UIControlEventValueChanged];
  [_editScale.widthAnchor constraintEqualToConstant:130].active = YES;

  UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:@[
    [self editBarButton:@"Reset" image:@"arrow.counterclockwise" filled:NO action:@selector(resetLayoutFromEditor)],
    hint,
    sizeLabel,
    _editScale,
    [self editBarButton:@"Done" image:@"checkmark" filled:YES action:@selector(endEditingLayout)],
  ]];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 14;
  [row setCustomSpacing:8 afterView:sizeLabel];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  [_editBar addSubview:row];

  [NSLayoutConstraint activateConstraints:@[
    [row.leadingAnchor constraintEqualToAnchor:_editBar.leadingAnchor constant:8],
    [row.trailingAnchor constraintEqualToAnchor:_editBar.trailingAnchor constant:-8],
    [row.topAnchor constraintEqualToAnchor:_editBar.topAnchor constant:6],
    [row.bottomAnchor constraintEqualToAnchor:_editBar.bottomAnchor constant:-6],
  ]];
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
    // The editor shows the pad whatever is attached: a layout is edited on
    // the screen even by someone who mostly plays on a pad. The preview hook
    // does too -- a Mac with a controller plugged in reports it to the
    // simulator, and the pad is the thing being looked at.
    if (self->_editing || DBSettings.uiPreviewMode)
    {
      self->_pad.hidden = NO;
      return;
    }
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

  if (_editBar)
  {
    const CGSize wanted = [_editBar systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
    const CGFloat width = MIN(wanted.width, CGRectGetWidth(self.view.bounds) - 2 * inset);
    _editBar.frame = CGRectMake(round(CGRectGetMidX(self.view.bounds) - width / 2), top - 4, width,
                                MAX(48, wanted.height));
  }

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

  // Test hook: the screen without the game. See DBSettings.uiPreviewMode.
  NSString* preview = DBSettings.uiPreviewMode;
  if (preview)
  {
    [self scheduleHUDFade];
    if ([preview isEqualToString:@"menu"])
      [self showMenu];
    else if ([preview isEqualToString:@"edit"])
      [self beginEditingLayout];
    return;
  }

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
  if (_menu || _editing)
    return;
  [self wakeHUD];

  // Pause before the panel is on screen. Presenting first would let the game
  // run for the length of the animation with its controls already covered.
  db_set_paused(1);

  _menu = [[DBPauseMenuView alloc] initWithGame:_game];
  __weak __typeof(self) weakSelf = self;
  _menu.onSettingsChanged = ^{
    [weakSelf applySettings];
  };
  _menu.onResume = ^{
    [weakSelf dismissMenuAndResume];
  };
  _menu.onQuit = ^{
    [weakSelf confirmQuit];
  };
  _menu.onEditLayout = ^{
    [weakSelf beginEditingLayout];
  };
  _menu.onResetLayout = ^{
    [weakSelf resetLayout];
  };
  [_menu presentInView:self.view];
  _menuButton.hidden = YES;
  // The readout sits beside the button and would otherwise poke out above
  // the panel, dimmed, saying nothing about a paused game.
  _perfLabel.hidden = YES;
}

- (void)resetLayout
{
  [_pad resetLayout];
}

- (void)dismissMenuAndResume
{
  if (!_menu)
    return;
  DBPauseMenuView* menu = _menu;
  _menu = nil;
  [menu dismissWithCompletion:^{
    self->_menuButton.hidden = NO;
    self->_perfLabel.hidden = !DBSettings.shared.showsPerformance;
    [self scheduleHUDFade];
  }];
  db_set_paused(0);
}

- (void)applySettings
{
  [_pad refreshFromSettings];
  [_pad reloadLayout];
  _perfLabel.hidden = _menu || _editing || !DBSettings.shared.showsPerformance;
  // Hiding the readout changes how wide the HUD group is, and so where its
  // centre falls.
  [self.view setNeedsLayout];
}

// The menu's Quit asks first. It is one tap from a slider someone was
// adjusting, and quitting throws away everything since the game last saved.
- (void)confirmQuit
{
  NSString* title = [NSString stringWithFormat:@"Quit %@?", _game.displayTitle ?: @"the game"];
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:@"Anything the game has not saved is lost."
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Quit"
                                            style:UIAlertActionStyleDestructive
                                          handler:^(UIAlertAction* action) {
                                            [self quitGame];
                                          }]];
  [self presentViewController:alert animated:YES completion:nil];
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

#pragma mark - Layout editor

// The game stays paused underneath -- it was paused for the menu this came
// from -- and the controls stop driving it. Done resumes: someone who has
// just arranged the pad wants to try it, not to see the menu again.
- (void)beginEditingLayout
{
  if (_editing)
    return;
  _editing = YES;

  DBPauseMenuView* menu = _menu;
  _menu = nil;
  [menu dismissWithCompletion:nil];
  db_set_paused(1);

  if (!_editBar)
    [self buildEditBar];
  _editScale.value = DBSettings.shared.padScale;
  _editBar.hidden = NO;
  _editBar.alpha = 0;
  _menuButton.hidden = YES;
  _perfLabel.hidden = YES;

  [self updatePadVisibility];
  _pad.hidden = NO;
  _pad.editingLayout = YES;
  [self.view setNeedsLayout];
  [UIView animateWithDuration:0.18
                   animations:^{
                     self->_editBar.alpha = 1;
                   }];
}

- (void)endEditingLayout
{
  if (!_editing)
    return;
  _editing = NO;

  _pad.editingLayout = NO;
  [UIView animateWithDuration:0.15
      animations:^{
        self->_editBar.alpha = 0;
      }
      completion:^(BOOL finished) {
        self->_editBar.hidden = YES;
      }];

  _menuButton.hidden = NO;
  _perfLabel.hidden = !DBSettings.shared.showsPerformance;
  [self updatePadVisibility];
  [self.view setNeedsLayout];
  [self wakeHUD];
  [self scheduleHUDFade];
  db_set_paused(0);
}

- (void)resetLayoutFromEditor
{
  [self resetLayout];
  _editScale.value = DBSettings.shared.padScale;
}

- (void)editScaleChanged
{
  DBSettings.shared.padScale = _editScale.value;
  [_pad reloadLayout];
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
  // Not while the menu or the editor is up: someone who opened it before
  // switching away still has it open, and resuming underneath it is exactly
  // what it exists to prevent.
  if (!_menu && !_editing)
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
