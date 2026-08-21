// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBGameViewController.h"

#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>

#import "DBLibrary.h"
#import "DBMetalView.h"
#import "DBTouchPadView.h"

#include "dolbundler_run.h"

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
  _status.frame = self.view.bounds;
  _spinner.center = CGPointMake(CGRectGetMidX(self.view.bounds),
                                CGRectGetMidY(self.view.bounds) + 28);

  CAMetalLayer* layer = (CAMetalLayer*)_metalView.layer;
  layer.contentsScale = UIScreen.mainScreen.nativeScale;
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
