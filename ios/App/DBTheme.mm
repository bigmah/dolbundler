// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBTheme.h"

namespace
{
// FNV-1a over the disc ID, then an avalanche mix.
//
// The mix is not decoration. Disc IDs are far from random -- GALE01, GLME01
// and G4QE01 share a prefix and a suffix -- and FNV alone leaves that
// structure in the low bits, which is exactly where taking a hue modulo 360
// reads from. Four Nintendo first-party titles came out four shades of the
// same green. Stirring the bits first is what spreads a real library across
// the wheel instead of across one corner of it.
uint32_t HashDiscID(NSString* disc_id)
{
  uint32_t hash = 2166136261u;
  const char* bytes = disc_id.UTF8String ?: "";
  for (const char* p = bytes; *p; ++p)
  {
    hash ^= (uint32_t)(unsigned char)*p;
    hash *= 16777619u;
  }

  hash ^= hash >> 16;
  hash *= 0x7feb352du;
  hash ^= hash >> 15;
  hash *= 0x846ca68bu;
  hash ^= hash >> 16;
  return hash;
}

UIColor* RGB(CGFloat r, CGFloat g, CGFloat b)
{
  return [UIColor colorWithRed:r green:g blue:b alpha:1.0];
}

// Generated art is cheap to draw once and wasteful to draw on every cell
// reuse. Keyed on everything that changes the result, so a hit is always the
// right image and never a stale one.
NSCache<NSString*, UIImage*>* CoverCache()
{
  static NSCache<NSString*, UIImage*>* cache;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    cache = [[NSCache alloc] init];
    cache.countLimit = 64;
  });
  return cache;
}
}  // namespace

@implementation DBTheme

// Sampled from a GameCube pad rather than picked: the point of colouring these
// at all is that someone who has held the controller already knows which one A
// is without reading the letter.
+ (UIColor*)buttonA { return RGB(0.28, 0.72, 0.58); }
+ (UIColor*)buttonB { return RGB(0.87, 0.28, 0.31); }
+ (UIColor*)buttonXY { return RGB(0.85, 0.85, 0.88); }
+ (UIColor*)buttonZ { return RGB(0.42, 0.38, 0.85); }
+ (UIColor*)shoulder { return RGB(0.78, 0.78, 0.82); }
+ (UIColor*)mainStick { return RGB(0.80, 0.80, 0.84); }
+ (UIColor*)cStick { return RGB(0.94, 0.78, 0.24); }

+ (UIColor*)accent { return RGB(0.44, 0.40, 0.86); }

+ (NSArray<UIColor*>*)coverColorsForDiscID:(NSString*)discID
{
  const uint32_t hash = HashDiscID(discID ?: @"");
  const CGFloat hue = (CGFloat)(hash % 3600u) / 3600.0;
  // A second hue a short way around the wheel, so the gradient reads as one
  // colour with depth rather than as two colours fighting. Saturation and
  // brightness are fixed: letting the hash choose those too produced tiles
  // that were sometimes unreadable behind white text.
  const CGFloat hue2 = fmod(hue + 0.07, 1.0);
  return @[
    [UIColor colorWithHue:hue saturation:0.62 brightness:0.78 alpha:1.0],
    [UIColor colorWithHue:hue2 saturation:0.80 brightness:0.40 alpha:1.0],
  ];
}

+ (UIImage*)coverImageForDiscID:(NSString*)discID
                           size:(CGSize)size
                          scale:(CGFloat)scale
                         dimmed:(BOOL)dimmed
{
  if (size.width <= 0 || size.height <= 0)
    return nil;

  NSString* cacheKey = [NSString stringWithFormat:@"%@|%.0fx%.0f@%.1f|%d", discID ?: @"", size.width,
                                                  size.height, scale, dimmed ? 1 : 0];
  UIImage* cached = [CoverCache() objectForKey:cacheKey];
  if (cached)
    return cached;

  UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
  format.scale = scale > 0 ? scale : UIScreen.mainScreen.scale;
  format.opaque = YES;

  UIGraphicsImageRenderer* renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size
                                                                            format:format];

  NSArray<UIColor*>* colours = [self coverColorsForDiscID:discID];
  NSString* label = discID.length ? discID.uppercaseString : @"??????";

  UIImage* image = [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    CGContextRef ctx = context.CGContext;
    const CGRect rect = CGRectMake(0, 0, size.width, size.height);

    // --- gradient ground -------------------------------------------------
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    NSArray* cgColours = @[ (__bridge id)colours[0].CGColor, (__bridge id)colours[1].CGColor ];
    const CGFloat stops[] = {0.0, 1.0};
    CGGradientRef gradient =
        CGGradientCreateWithColors(space, (__bridge CFArrayRef)cgColours, stops);
    CGContextDrawLinearGradient(ctx, gradient, CGPointMake(0, 0),
                                CGPointMake(size.width, size.height), 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(space);

    // --- mini-disc glyph -------------------------------------------------
    // A GameCube disc is 8 cm with a large hub, which is a distinctive enough
    // silhouette to carry the tile on its own. Drawn oversized and off-centre
    // so it reads as texture rather than as an icon someone might try to tap.
    const CGFloat discRadius = MIN(size.width, size.height) * 0.46;
    const CGPoint centre = CGPointMake(size.width * 0.52, size.height * 0.44);

    [[UIColor colorWithWhite:1.0 alpha:0.17] setFill];
    CGContextFillEllipseInRect(ctx, CGRectMake(centre.x - discRadius, centre.y - discRadius,
                                               discRadius * 2, discRadius * 2));

    [[UIColor colorWithWhite:1.0 alpha:0.24] setStroke];
    CGContextSetLineWidth(ctx, MAX(1.0, size.width * 0.012));
    const CGFloat ringRadius = discRadius * 0.62;
    CGContextStrokeEllipseInRect(ctx, CGRectMake(centre.x - ringRadius, centre.y - ringRadius,
                                                 ringRadius * 2, ringRadius * 2));

    [[UIColor colorWithWhite:0.0 alpha:0.22] setFill];
    const CGFloat hubRadius = discRadius * 0.30;
    CGContextFillEllipseInRect(ctx, CGRectMake(centre.x - hubRadius, centre.y - hubRadius,
                                               hubRadius * 2, hubRadius * 2));

    // --- disc ID ---------------------------------------------------------
    // Bottom-aligned over a dark scrim: the gradient's light end can land
    // anywhere, and white letters on it alone are not reliably legible.
    const CGFloat scrimHeight = size.height * 0.30;
    CGContextSaveGState(ctx);
    NSArray* scrimColours = @[
      (__bridge id)[UIColor colorWithWhite:0.0 alpha:0.0].CGColor,
      (__bridge id)[UIColor colorWithWhite:0.0 alpha:0.55].CGColor,
    ];
    CGColorSpaceRef scrimSpace = CGColorSpaceCreateDeviceRGB();
    CGGradientRef scrim =
        CGGradientCreateWithColors(scrimSpace, (__bridge CFArrayRef)scrimColours, stops);
    CGContextClipToRect(ctx, CGRectMake(0, size.height - scrimHeight, size.width, scrimHeight));
    CGContextDrawLinearGradient(ctx, scrim, CGPointMake(0, size.height - scrimHeight),
                                CGPointMake(0, size.height), 0);
    CGGradientRelease(scrim);
    CGColorSpaceRelease(scrimSpace);
    CGContextRestoreGState(ctx);

    const CGFloat fontSize = MAX(9.0, size.width * 0.115);
    NSDictionary* attrs = @{
      NSFontAttributeName : [UIFont monospacedSystemFontOfSize:fontSize
                                                        weight:UIFontWeightSemibold],
      NSForegroundColorAttributeName : [UIColor colorWithWhite:1.0 alpha:0.92],
      NSKernAttributeName : @(fontSize * 0.08),
    };
    const CGSize labelSize = [label sizeWithAttributes:attrs];
    [label drawAtPoint:CGPointMake((size.width - labelSize.width) / 2.0,
                                   size.height - labelSize.height - size.height * 0.07)
        withAttributes:attrs];

    // --- unplayable veil -------------------------------------------------
    // A disc this build has no module for is still a real import: it took the
    // storage and the wait, and deleting it is a decision. So it stays in the
    // grid at full size and is muted rather than hidden.
    if (dimmed)
    {
      [[UIColor colorWithWhite:0.30 alpha:0.55] setFill];
      CGContextFillRect(ctx, rect);
    }
  }];

  [CoverCache() setObject:image forKey:cacheKey];
  return image;
}

@end
