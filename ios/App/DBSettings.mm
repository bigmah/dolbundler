// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBSettings.h"

#import <UIKit/UIKit.h>

namespace
{
NSString* const kPadOpacityKey = @"DBPadOpacity";
NSString* const kHapticsKey = @"DBHapticsEnabled";
NSString* const kPerformanceKey = @"DBShowsPerformance";

constexpr CGFloat kMinPadOpacity = 0.25;
constexpr CGFloat kMaxPadOpacity = 1.0;
}  // namespace

@implementation DBSettings

+ (instancetype)shared
{
  static DBSettings* shared;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    shared = [[DBSettings alloc] init];
  });
  return shared;
}

- (instancetype)init
{
  self = [super init];
  if (!self)
    return nil;

  // registerDefaults rather than checking for nil at every read: an unset key
  // and a key set to zero are otherwise indistinguishable through
  // -doubleForKey:, which would make an opacity of 0 impossible to store and a
  // haptics switch impossible to turn off.
  [NSUserDefaults.standardUserDefaults registerDefaults:@{
    kPadOpacityKey : @0.78,
    kHapticsKey : @YES,
    kPerformanceKey : @YES,
  }];
  return self;
}

- (CGFloat)padOpacity
{
  const CGFloat stored = [NSUserDefaults.standardUserDefaults doubleForKey:kPadOpacityKey];
  return MIN(kMaxPadOpacity, MAX(kMinPadOpacity, stored));
}

- (void)setPadOpacity:(CGFloat)padOpacity
{
  [NSUserDefaults.standardUserDefaults setDouble:MIN(kMaxPadOpacity, MAX(kMinPadOpacity, padOpacity))
                                          forKey:kPadOpacityKey];
}

- (BOOL)hapticsEnabled
{
  return [NSUserDefaults.standardUserDefaults boolForKey:kHapticsKey];
}

- (void)setHapticsEnabled:(BOOL)hapticsEnabled
{
  [NSUserDefaults.standardUserDefaults setBool:hapticsEnabled forKey:kHapticsKey];
}

- (BOOL)showsPerformance
{
  return [NSUserDefaults.standardUserDefaults boolForKey:kPerformanceKey];
}

- (void)setShowsPerformance:(BOOL)showsPerformance
{
  [NSUserDefaults.standardUserDefaults setBool:showsPerformance forKey:kPerformanceKey];
}

@end
