// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBAppDelegate.h"

#import "DBLibrary.h"
#import "DBLibraryViewController.h"
#import "DBTheme.h"

@implementation DBAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
  // Everything the app writes lives under Documents so it is visible in Files:
  // the user has to be able to get a 1.3 GB disc image in, and to delete an
  // extracted game when the device runs out of room.
  [DBLibrary shared];

  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];

  DBLibraryViewController* library = [[DBLibraryViewController alloc] init];
  UINavigationController* nav =
      [[UINavigationController alloc] initWithRootViewController:library];
  nav.navigationBar.prefersLargeTitles = YES;
  nav.navigationBar.tintColor = DBTheme.accent;

  self.window.rootViewController = nav;
  [self.window makeKeyAndVisible];
  return YES;
}

// A disc image opened from Files or AirDrop lands here.
- (BOOL)application:(UIApplication*)app
            openURL:(NSURL*)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id>*)options
{
  UINavigationController* nav = (UINavigationController*)self.window.rootViewController;
  for (UIViewController* controller in nav.viewControllers)
  {
    if ([controller isKindOfClass:[DBLibraryViewController class]])
    {
      [nav popToViewController:controller animated:NO];
      [(DBLibraryViewController*)controller importDiscAtURL:url];
      return YES;
    }
  }
  return NO;
}

@end
