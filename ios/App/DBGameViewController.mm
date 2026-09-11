// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBGameViewController.h"

#import <QuartzCore/CAMetalLayer.h>

#import "DBControllers.h"
#import "DBExternalDisplay.h"
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
// to the screen's native pixel density before Core Animation displays it. The
// same holds on a TV, which the phone's GPU is drawing for all the same: the
// image has 480 lines either way, and only who scales it up changes.
constexpr CGFloat kGameCubeOutputShortSide = 480.0;

// How long the corner button stays at full strength before fading back. Long
// enough to find it after the game starts, short enough that it is out of the
// way by the time anyone is playing.
constexpr NSTimeInterval kHUDIdleDelay = 4.0;
constexpr CGFloat kHUDRestingAlpha = 0.22;

// While the game is on the TV, the phone shows the same few lines for as long
// as the party lasts, which is how an OLED panel keeps an image. It fades with
// the HUD, but not so far that a glance at the phone cannot read it.
constexpr CGFloat kTVCardRestingAlpha = 0.35;

// How long the list of players stays up after someone joins or leaves.
constexpr NSTimeInterval kRosterDuration = 3.5;

// The shortest time between frames shown on the TV under steady motion. A game
// renders a frame every 16.7 ms, and 25 ms falls between one frame and two, so
// exactly every other frame is shown -- while a game already running at 30
// keeps every frame it draws.
constexpr double kTVPresentInterval = 0.025;

CGFloat GameDrawableScale(CGSize bounds, UIScreen* screen)
{
  const CGFloat nativeScale = screen.nativeScale;
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

  // Who is playing with what, and the TV the game goes to when there is one.
  DBControllers* _controllers;
  DBExternalDisplay* _tv;
  // The drawable size last handed to the renderer, so a layout pass that
  // changes nothing does not make it rebuild its backbuffer.
  CGSize _surfaceSize;

  // What the phone shows while the game is on the TV.
  UIView* _tvCard;
  UILabel* _tvHeading;
  UILabel* _tvSubtitle;
  UIStackView* _tvPlayers;
  UILabel* _tvHint;
  UIStackView* _tvSteadyRow;
  UISwitch* _tvSteadySwitch;

  // Every player and what they are holding, shown for a moment wherever the
  // game is -- on the TV, both players can read it -- when that changes.
  UIView* _roster;
  UILabel* _rosterLabel;
  NSInteger _controllerCount;
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

  // Beneath the pad: when player 1 has no controller the pad is how they play,
  // and the card shrinks out of its way rather than covering it.
  [self buildTVCard];

  _pad = [[DBTouchPadView alloc] initWithFrame:self.view.bounds];
  _pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:_pad];

  // Added after the pad so it wins the hit test: the pad covers the whole
  // screen, and a menu button underneath it could never be tapped.
  [self buildHUD];

  __weak __typeof(self) weakSelf = self;
  _controllers = [[DBControllers alloc] init];
  _controllers.onChange = ^{
    [weakSelf playersChanged];
  };
  _tv = [[DBExternalDisplay alloc] init];
  _tv.onChange = ^{
    [weakSelf routeVideo];
  };

  // Both before the game boots rather than once it is on screen. Controllers
  // plugged in now are what the console starts with, which is what a game
  // counts players from; and a TV that is already connected should have the
  // game from its first frame, since the renderer sizes itself to the layer
  // as it starts. The preview hook has no game, so nothing to feed.
  [_tv start];
  if (!DBSettings.uiPreviewMode)
    [_controllers start];
  [self updatePadVisibility];

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
  // The editor shows the pad whatever is attached: a layout is edited on the
  // screen even by someone who mostly plays on a pad. The preview hook does
  // too -- a Mac with a controller plugged in reports it to the simulator, and
  // the pad is the thing being looked at -- except in its TV preview, whose
  // made-up player 1 has a controller.
  if (_editing || (DBSettings.uiPreviewMode && !self.previewingTV))
  {
    _pad.hidden = NO;
    return;
  }
  // The pad is player 1's controller whenever no Bluetooth controller is. A
  // second player's controller leaves it where it is.
  _pad.hidden = [self portHasController:0];
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

  if (!_tvCard.hidden)
  {
    const CGSize fit = [_tvCard systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
    const CGFloat width = MIN(MAX(fit.width, 280), CGRectGetWidth(self.view.bounds) - 2 * inset);
    const CGFloat below = top + size + 12;
    // Compact, it tucks under the HUD, out of the pad's way. Whole, it takes
    // the middle of what is left.
    const CGFloat y = _tvPlayers.hidden ?
                          below :
                          MAX(below, round(below + (CGRectGetHeight(self.view.bounds) - below -
                                                    fit.height) /
                                                       2));
    _tvCard.frame = CGRectMake(round(CGRectGetMidX(self.view.bounds) - width / 2), y, width,
                               fit.height);
  }

  // Only while the game is on the phone. On the TV the layer is sized by
  // -routeVideo, and this view's bounds say nothing about it.
  if (_metalView.superview == self.view)
    [self updateRenderSurface];
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
    else if (self.previewingTV)
      [self showRoster];
    return;
  }

  // Controllers that were already connected joined before there was a screen
  // to say so on.
  if (_controllerCount > 0)
    [self showRoster];

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

- (void)viewDidDisappear:(BOOL)animated
{
  [super viewDidDisappear:animated];
  // Gone for good, not just covered by an alert: the TV goes back to
  // mirroring, and the controllers let go of their ports.
  if (self.isBeingDismissed)
  {
    [_tv stop];
    [_controllers stop];
  }
}

#pragma mark - TV

- (BOOL)previewingTV
{
  return [DBSettings.uiPreviewMode isEqualToString:@"tv"];
}

- (BOOL)isOnTV
{
  return _tv.contentView != nil || self.previewingTV;
}

- (BOOL)portHasController:(NSInteger)port
{
  if (self.previewingTV)
    return port < 2;
  return [_controllers controllerNameForPort:port] != nil;
}

// What stands in each port the game can see: a controller's name, the touch
// controls for player 1, or nothing for a port whose controller has gone. nil
// for a port that is not plugged in.
- (NSString*)holderNameForPort:(NSInteger)port
{
  if (self.previewingTV)
  {
    NSArray<NSString*>* madeUp = @[ @"Xbox Wireless Controller", @"DualSense Wireless Controller" ];
    return port < (NSInteger)madeUp.count ? madeUp[port] : nil;
  }
  if (![_controllers isPortPluggedIn:port])
    return nil;
  NSString* name = [_controllers controllerNameForPort:port];
  if (name)
    return name;
  return port == 0 ? @"Touch controls" : @"No controller";
}

// Put the game wherever the TV is, and bring it back to the phone when the TV
// goes.
- (void)routeVideo
{
  UIView* host = _tv.contentView ?: self.view;
  if (_metalView.superview != host)
  {
    [_metalView removeFromSuperview];
    _metalView.frame = host.bounds;
    // At the back: on the phone the pad and the HUD are drawn over it.
    [host insertSubview:_metalView atIndex:0];
    // A roster on its way out would otherwise be left on the wrong screen.
    [_roster removeFromSuperview];
  }
  [self updateRenderSurface];
  [self updatePadVisibility];
  [self updateTVCard];
}

// Size the layer for the screen it is on, and tell the renderer when that
// changed.
- (void)updateRenderSurface
{
  CAMetalLayer* layer = (CAMetalLayer*)_metalView.layer;
  const CGSize bounds = _metalView.bounds.size;
  const CGFloat scale = GameDrawableScale(bounds, _metalView.window.screen ?: UIScreen.mainScreen);
  const CGSize size = CGSizeMake(bounds.width * scale, bounds.height * scale);
  layer.contentsScale = scale;
  layer.drawableSize = size;
  if (CGSizeEqualToSize(size, _surfaceSize))
    return;
  _surfaceSize = size;
  db_render_surface_resized();
}

- (void)buildTVCard
{
  _tvCard = [[UIView alloc] init];
  _tvCard.hidden = YES;

  UIImageView* icon = [[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"tv"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithPointSize:30
                                                                      weight:UIImageSymbolWeightMedium]]];
  icon.tintColor = DBTheme.accent;
  icon.contentMode = UIViewContentModeScaleAspectFit;

  _tvHeading = [[UILabel alloc] init];
  _tvHeading.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
  _tvHeading.textColor = UIColor.whiteColor;
  _tvHeading.textAlignment = NSTextAlignmentCenter;

  _tvSubtitle = [[UILabel alloc] init];
  _tvSubtitle.text = _game.displayTitle.length ? _game.displayTitle : _game.discID;
  _tvSubtitle.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
  _tvSubtitle.textColor = [UIColor colorWithWhite:1.0 alpha:0.5];
  _tvSubtitle.textAlignment = NSTextAlignmentCenter;

  _tvPlayers = [[UIStackView alloc] init];
  _tvPlayers.axis = UILayoutConstraintAxisVertical;
  _tvPlayers.alignment = UIStackViewAlignmentLeading;
  _tvPlayers.spacing = 8;

  _tvHint = [[UILabel alloc] init];
  _tvHint.font = [UIFont systemFontOfSize:12 weight:UIFontWeightMedium];
  _tvHint.textColor = [UIColor colorWithWhite:1.0 alpha:0.45];
  _tvHint.textAlignment = NSTextAlignmentCenter;
  _tvHint.numberOfLines = 2;

  // On the card rather than in the pause menu: the comparison is flipping it
  // and looking up at the TV, with the game still running.
  UILabel* steadyLabel = [[UILabel alloc] init];
  steadyLabel.text = @"Steady 30 fps on the TV";
  steadyLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
  steadyLabel.textColor = [UIColor colorWithWhite:1.0 alpha:0.75];
  _tvSteadySwitch = [[UISwitch alloc] init];
  _tvSteadySwitch.onTintColor = DBTheme.accent;
  _tvSteadySwitch.on = DBSettings.shared.steadyTVMotion;
  [_tvSteadySwitch addTarget:self
                      action:@selector(tvSteadyChanged)
            forControlEvents:UIControlEventValueChanged];
  _tvSteadyRow = [[UIStackView alloc] initWithArrangedSubviews:@[ steadyLabel, _tvSteadySwitch ]];
  _tvSteadyRow.axis = UILayoutConstraintAxisHorizontal;
  _tvSteadyRow.alignment = UIStackViewAlignmentCenter;
  _tvSteadyRow.spacing = 12;

  UIStackView* stack = [[UIStackView alloc]
      initWithArrangedSubviews:@[ icon, _tvHeading, _tvSubtitle, _tvPlayers, _tvHint, _tvSteadyRow ]];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.spacing = 4;
  [stack setCustomSpacing:10 afterView:icon];
  [stack setCustomSpacing:18 afterView:_tvSubtitle];
  [stack setCustomSpacing:16 afterView:_tvPlayers];
  [stack setCustomSpacing:14 afterView:_tvHint];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_tvCard addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [stack.leadingAnchor constraintEqualToAnchor:_tvCard.leadingAnchor],
    [stack.trailingAnchor constraintEqualToAnchor:_tvCard.trailingAnchor],
    [stack.topAnchor constraintEqualToAnchor:_tvCard.topAnchor],
    [stack.bottomAnchor constraintEqualToAnchor:_tvCard.bottomAnchor],
  ]];

  // Faded, it can be brought back to read without reaching for the menu.
  UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc] initWithTarget:self
                                                                        action:@selector(tvCardTapped)];
  [_tvCard addGestureRecognizer:tap];
  [self.view addSubview:_tvCard];
}

- (UIView*)playerRowForPort:(NSInteger)port name:(NSString*)name
{
  const BOOL holds = [self portHasController:port];

  UILabel* badge = [[UILabel alloc] init];
  badge.text = [NSString stringWithFormat:@"P%ld", (long)port + 1];
  badge.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightBold];
  badge.textColor = UIColor.whiteColor;
  badge.textAlignment = NSTextAlignmentCenter;
  badge.backgroundColor = holds ? DBTheme.accent : [UIColor colorWithWhite:1.0 alpha:0.14];
  badge.layer.cornerRadius = 11;
  badge.layer.cornerCurve = kCACornerCurveContinuous;
  badge.clipsToBounds = YES;
  [badge.widthAnchor constraintEqualToConstant:34].active = YES;
  [badge.heightAnchor constraintEqualToConstant:22].active = YES;

  UILabel* label = [[UILabel alloc] init];
  label.text = name;
  label.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
  label.textColor = holds ? UIColor.whiteColor : [UIColor colorWithWhite:1.0 alpha:0.5];

  UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:@[ badge, label ]];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 10;
  return row;
}

- (void)updateTVCard
{
  const BOOL onTV = self.isOnTV;
  // Paced only while the picture is on the TV: the phone's own screen shows
  // every frame the game draws, and so does the next game started without one.
  db_set_frame_pacing(onTV && DBSettings.shared.steadyTVMotion ? kTVPresentInterval : 0);

  _tvCard.hidden = !onTV || _menu || _editing;
  [self.view setNeedsLayout];
  if (!onTV)
    return;

  NSString* receiver = _tv.receiverName;
  _tvHeading.text = receiver.length ? [NSString stringWithFormat:@"Playing on %@", receiver]
                                    : @"Playing on the TV";

  // With the touch controls as player 1 the phone is a controller again, and
  // the card is only a reminder of where the picture went. It sits beneath the
  // pad then, where its switch could not be reached anyway.
  const BOOL compact = ![self portHasController:0];
  _tvSubtitle.hidden = compact;
  _tvPlayers.hidden = compact;
  _tvSteadyRow.hidden = compact;
  _tvSteadySwitch.on = DBSettings.shared.steadyTVMotion;

  for (UIView* row in _tvPlayers.arrangedSubviews)
    [row removeFromSuperview];
  NSInteger open = -1;
  for (NSInteger port = 0; port < DB_PAD_PORTS; ++port)
  {
    if (open < 0 && ![self portHasController:port])
      open = port;
    NSString* name = [self holderNameForPort:port];
    if (name)
      [_tvPlayers addArrangedSubview:[self playerRowForPort:port name:name]];
  }

  _tvHint.text = open < 0 ? nil :
                            [NSString stringWithFormat:@"To add player %ld, turn on another "
                                                       @"controller paired with this phone.",
                                                       (long)open + 1];
  _tvHint.hidden = compact || open < 0;
}

- (void)tvCardTapped
{
  [self wakeHUD];
  [self scheduleHUDFade];
}

- (void)tvSteadyChanged
{
  DBSettings.shared.steadyTVMotion = _tvSteadySwitch.isOn;
  [self updateTVCard];
  [self wakeHUD];
  [self scheduleHUDFade];
}

#pragma mark - Players

- (void)playersChanged
{
  [self updatePadVisibility];
  [self updateTVCard];

  NSInteger count = 0;
  for (NSInteger port = 0; port < DB_PAD_PORTS; ++port)
  {
    if ([self portHasController:port])
      ++count;
  }
  // Nothing to say to someone on the touch controls alone, which is every game
  // started without a controller. After that, every join and every leave.
  const BOOL worthSaying = count > 0 || count != _controllerCount;
  _controllerCount = count;
  if (!worthSaying || !_started)
    return;

  [self showRoster];
  [self wakeHUD];
  [self scheduleHUDFade];
}

- (void)showRoster
{
  UIView* host = _tv.contentView ?: self.view;
  const BOOL onTV = host != self.view;

  if (!_roster)
  {
    _roster = [[UIView alloc] init];
    _roster.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.72];
    _roster.layer.cornerCurve = kCACornerCurveContinuous;
    _roster.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.22].CGColor;
    _roster.userInteractionEnabled = NO;
    _rosterLabel = [[UILabel alloc] init];
    _rosterLabel.numberOfLines = 0;
    [_roster addSubview:_rosterLabel];
  }
  [_roster removeFromSuperview];
  [host addSubview:_roster];

  // Sized to be read from the sofa on a TV, and no bigger than the HUD's own
  // text on the phone.
  const CGFloat unit = MAX(13, round(CGRectGetHeight(host.bounds) / 36));
  NSMutableParagraphStyle* paragraph = [[NSMutableParagraphStyle alloc] init];
  paragraph.lineSpacing = round(unit * 0.35);

  NSMutableAttributedString* text = [[NSMutableAttributedString alloc] init];
  for (NSInteger port = 0; port < DB_PAD_PORTS; ++port)
  {
    NSString* name = [self holderNameForPort:port];
    if (!name)
      continue;
    if (text.length)
      [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"]];
    [text appendAttributedString:[[NSAttributedString alloc]
                                     initWithString:[NSString stringWithFormat:@"Player %ld   ",
                                                                               (long)port + 1]
                                         attributes:@{
                                           NSFontAttributeName : [UIFont systemFontOfSize:unit
                                                                                   weight:UIFontWeightBold],
                                           NSForegroundColorAttributeName : UIColor.whiteColor,
                                         }]];
    const BOOL holds = [self portHasController:port];
    [text appendAttributedString:[[NSAttributedString alloc]
                                     initWithString:name
                                         attributes:@{
                                           NSFontAttributeName : [UIFont systemFontOfSize:unit
                                                                                   weight:UIFontWeightMedium],
                                           NSForegroundColorAttributeName :
                                               [UIColor colorWithWhite:1.0 alpha:holds ? 0.85 : 0.5],
                                         }]];
  }
  [text addAttribute:NSParagraphStyleAttributeName value:paragraph range:NSMakeRange(0, text.length)];
  _rosterLabel.attributedText = text;

  const CGFloat padX = round(unit * 1.1), padY = round(unit * 0.7);
  const CGSize fit = [_rosterLabel sizeThatFits:CGSizeMake(CGRectGetWidth(host.bounds) * 0.8,
                                                           CGFLOAT_MAX)];
  const CGSize box = CGSizeMake(ceil(fit.width) + 2 * padX, ceil(fit.height) + 2 * padY);
  // On the phone, below the HUD at the top centre; on the TV, near the top
  // but inside the part every TV actually shows.
  const CGFloat y = onTV ? round(CGRectGetHeight(host.bounds) * 0.07)
                         : self.view.safeAreaInsets.top + 14 + 34 + 12;
  _roster.frame = CGRectMake(round(CGRectGetMidX(host.bounds) - box.width / 2), y, box.width,
                             box.height);
  _rosterLabel.frame = CGRectMake(padX, padY, ceil(fit.width), ceil(fit.height));
  _roster.layer.cornerRadius = round(unit * 0.8);
  _roster.layer.borderWidth = 1.0 / (host.window.screen.scale ?: 1.0);

  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(hideRoster) object:nil];
  _roster.alpha = 0;
  [UIView animateWithDuration:0.2
                   animations:^{
                     self->_roster.alpha = 1;
                   }];
  [self performSelector:@selector(hideRoster) withObject:nil afterDelay:kRosterDuration];
}

- (void)hideRoster
{
  UIView* roster = _roster;
  [UIView animateWithDuration:0.3
      animations:^{
        roster.alpha = 0;
      }
      completion:^(BOOL finished) {
        if (finished)
          [roster removeFromSuperview];
      }];
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
                     self->_tvCard.alpha = kTVCardRestingAlpha;
                   }];
}

- (void)wakeHUD
{
  [NSObject cancelPreviousPerformRequestsWithTarget:self
                                           selector:@selector(fadeHUD)
                                             object:nil];
  _menuButton.alpha = 1.0;
  _tvCard.alpha = 1.0;
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
  // the panel, dimmed, saying nothing about a paused game. The TV card would
  // show through it.
  _perfLabel.hidden = YES;
  _tvCard.hidden = YES;
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
    [self updateTVCard];
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
  _tvCard.hidden = YES;

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
  [self updateTVCard];
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
