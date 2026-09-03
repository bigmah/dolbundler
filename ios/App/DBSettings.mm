// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBSettings.h"

#import <UIKit/UIKit.h>

namespace
{
NSString* const kPadOpacityKey = @"DBPadOpacity";
NSString* const kPadScaleKey = @"DBPadScale";
NSString* const kPadLayoutKey = @"DBPadLayout";
NSString* const kHapticsKey = @"DBHapticsEnabled";
NSString* const kPerformanceKey = @"DBShowsPerformance";

constexpr CGFloat kMinPadOpacity = 0.25;
constexpr CGFloat kMaxPadOpacity = 1.0;
constexpr CGFloat kMinPadScale = 0.7;
constexpr CGFloat kMaxPadScale = 1.4;
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
    kPadScaleKey : @1.0,
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

+ (CGFloat)minPadScale
{
  return kMinPadScale;
}

+ (CGFloat)maxPadScale
{
  return kMaxPadScale;
}

- (CGFloat)padScale
{
  const CGFloat stored = [NSUserDefaults.standardUserDefaults doubleForKey:kPadScaleKey];
  return MIN(kMaxPadScale, MAX(kMinPadScale, stored));
}

- (void)setPadScale:(CGFloat)padScale
{
  [NSUserDefaults.standardUserDefaults setDouble:MIN(kMaxPadScale, MAX(kMinPadScale, padScale))
                                          forKey:kPadScaleKey];
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

#pragma mark - Layout

- (NSDictionary<NSString*, NSArray<NSNumber*>*>*)storedLayout
{
  id stored = [NSUserDefaults.standardUserDefaults objectForKey:kPadLayoutKey];
  return [stored isKindOfClass:NSDictionary.class] ? stored : @{};
}

- (BOOL)padPosition:(CGPoint*)fraction forControl:(NSString*)name
{
  NSArray* pair = [self storedLayout][name];
  if (![pair isKindOfClass:NSArray.class] || pair.count != 2)
    return NO;
  if (fraction)
    *fraction = CGPointMake([pair[0] doubleValue], [pair[1] doubleValue]);
  return YES;
}

- (void)setPadPosition:(CGPoint)fraction forControl:(NSString*)name
{
  NSMutableDictionary* layout = [[self storedLayout] mutableCopy];
  layout[name] = @[ @(fraction.x), @(fraction.y) ];
  [NSUserDefaults.standardUserDefaults setObject:layout forKey:kPadLayoutKey];
}

- (BOOL)hasCustomPadLayout
{
  return [self storedLayout].count > 0 || fabs(self.padScale - 1.0) > 0.001;
}

- (void)resetPadLayout
{
  [NSUserDefaults.standardUserDefaults removeObjectForKey:kPadLayoutKey];
  [NSUserDefaults.standardUserDefaults removeObjectForKey:kPadScaleKey];
}

#pragma mark - Test hook

+ (NSString*)uiPreviewMode
{
  const char* mode = getenv("DOLBUNDLER_UI_PREVIEW");
  return (mode && *mode) ? @(mode) : nil;
}

@end
