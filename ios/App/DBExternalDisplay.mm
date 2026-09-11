// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBExternalDisplay.h"

#import <AVFoundation/AVFoundation.h>

// UIScreen's connect notifications and UIWindow.screen are how an app that
// does not use scenes reaches a second display, and this app does not. Both
// are deprecated in favour of an external-display scene session; both still
// work. Moving the app to scenes replaces the inside of this file, not its
// callers.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@implementation DBExternalDisplay
{
  UIScreen* _screen;
  UIWindow* _window;
  BOOL _started;
}

- (void)dealloc
{
  [self stop];
}

- (void)start
{
  if (_started)
    return;
  _started = YES;

  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  for (NSNotificationName name in @[
         UIScreenDidConnectNotification, UIScreenDidDisconnectNotification,
         UIScreenModeDidChangeNotification
       ])
  {
    [center addObserver:self selector:@selector(screensChanged) name:name object:nil];
  }
  [self screensChanged];
}

- (void)stop
{
  if (!_started)
    return;
  _started = NO;
  [NSNotificationCenter.defaultCenter removeObserver:self];
  [self takeDownWindow];
}

- (UIView*)contentView
{
  return _window.rootViewController.view;
}

- (NSString*)receiverName
{
  if (!_screen)
    return nil;
  for (AVAudioSessionPortDescription* output in AVAudioSession.sharedInstance.currentRoute.outputs)
  {
    if ([output.portType isEqualToString:AVAudioSessionPortAirPlay] && output.portName.length)
      return output.portName;
  }
  return nil;
}

- (void)screensChanged
{
  UIScreen* external = nil;
  for (UIScreen* screen in UIScreen.screens)
  {
    if (screen != UIScreen.mainScreen)
    {
      external = screen;
      break;
    }
  }

  if (external != _screen)
  {
    [self takeDownWindow];
    if (external)
      [self putUpWindowOn:external];
  }
  else if (_window)
  {
    // A mode change: the same TV at a different size.
    _window.frame = _screen.bounds;
  }

  if (_onChange)
    _onChange();
}

- (void)putUpWindowOn:(UIScreen*)screen
{
  _screen = screen;
  _window = [[UIWindow alloc] initWithFrame:screen.bounds];
  _window.screen = screen;

  UIViewController* root = [[UIViewController alloc] init];
  root.view.backgroundColor = UIColor.blackColor;
  _window.rootViewController = root;
  // Shown, never made key: nothing on a TV takes a touch.
  _window.hidden = NO;
}

- (void)takeDownWindow
{
  _window.hidden = YES;
  _window = nil;
  _screen = nil;
}

@end

#pragma clang diagnostic pop
