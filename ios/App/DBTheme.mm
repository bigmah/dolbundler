// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBTheme.h"

#import <CoreImage/CoreImage.h>

#import "DBBanner.h"

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

// The banner, blurred: the wash of the game's colours behind the art.
//
// Enlarging the banner directly, however good the interpolation, only makes
// its pixels bigger, and a round trip through a bitmap a few pixels across
// bands. So it is a real Gaussian, done once per card and cached with it.
// Clamped to its extent first, or the blur pulls transparent black in from
// outside the image and the edges of the wash go dark.
UIImage* BlurredBackdrop(UIImage* banner)
{
  static CIContext* context;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    context = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer : @NO}];
  });

  CIImage* source = [CIImage imageWithCGImage:banner.CGImage];
  CIFilter* blur = [CIFilter filterWithName:@"CIGaussianBlur"];
  [blur setValue:[source imageByClampingToExtent] forKey:kCIInputImageKey];
  [blur setValue:@4.0 forKey:kCIInputRadiusKey];
  CGImageRef blurred = [context createCGImage:blur.outputImage fromRect:source.extent];
  if (!blurred)
    return banner;
  UIImage* image = [UIImage imageWithCGImage:blurred scale:1.0 orientation:UIImageOrientationUp];
  CGImageRelease(blurred);
  return image;
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

+ (UIImage*)cardImageForDiscID:(NSString*)discID
                        banner:(DBBanner*)banner
                          size:(CGSize)size
                         scale:(CGFloat)scale
                        dimmed:(BOOL)dimmed
{
  if (size.width <= 0 || size.height <= 0)
    return nil;

  NSString* cacheKey =
      [NSString stringWithFormat:@"%@|%d|%.0fx%.0f@%.1f|%d", discID ?: @"", banner ? 1 : 0,
                                 size.width, size.height, scale, dimmed ? 1 : 0];
  UIImage* cached = [CoverCache() objectForKey:cacheKey];
  if (cached)
    return cached;

  UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
  format.scale = scale > 0 ? scale : UIScreen.mainScreen.scale;
  format.opaque = YES;

  UIGraphicsImageRenderer* renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size
                                                                            format:format];

  UIImage* image = [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    CGContextRef ctx = context.CGContext;
    const CGRect rect = CGRectMake(0, 0, size.width, size.height);

    if (banner)
      [self drawBanner:banner inRect:rect context:ctx];
    else
      [self drawGeneratedTileForDiscID:discID inRect:rect context:ctx];

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

// The banner is 96x32 and the card is drawn at five or six times that. Two
// copies of it make the card: one stretched to fill, which at that
// magnification is its own blur and gives the card the game's colours; and
// one drawn pixel-for-pixel with no smoothing, because a banner that was
// pixel art on a CRT stays pixel art at 6x, and smoothing it into a soft
// smear is the thing that makes a library look like a bootleg.
+ (void)drawBanner:(DBBanner*)banner inRect:(CGRect)rect context:(CGContextRef)ctx
{
  UIImage* image = banner.image;
  const CGSize size = rect.size;

  // --- backdrop --------------------------------------------------------
  // Aspect-filled and overscanned so the edges of the banner never show as
  // seams, then darkened enough for white text to sit on it.
  UIImage* wash = BlurredBackdrop(image);
  const CGFloat fillScale = MAX(size.width / wash.size.width, size.height / wash.size.height) * 1.15;
  const CGSize fillSize = CGSizeMake(wash.size.width * fillScale, wash.size.height * fillScale);
  CGContextSaveGState(ctx);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  [wash drawInRect:CGRectMake((size.width - fillSize.width) / 2, (size.height - fillSize.height) / 2,
                              fillSize.width, fillSize.height)];
  CGContextRestoreGState(ctx);
  [[UIColor colorWithWhite:0.0 alpha:0.42] setFill];
  CGContextFillRect(ctx, rect);

  // --- the banner itself -----------------------------------------------
  const CGFloat width = size.width * 0.84;
  const CGFloat height = width * image.size.height / image.size.width;
  const CGRect frame = CGRectMake(round((size.width - width) / 2), round((size.height - height) / 2),
                                  round(width), round(height));
  const CGFloat radius = MAX(3.0, size.height * 0.05);

  CGContextSaveGState(ctx);
  CGContextSetShadowWithColor(ctx, CGSizeMake(0, size.height * 0.03), size.height * 0.08,
                              [UIColor colorWithWhite:0.0 alpha:0.55].CGColor);
  [[UIColor blackColor] setFill];
  [[UIBezierPath bezierPathWithRoundedRect:frame cornerRadius:radius] fill];
  CGContextRestoreGState(ctx);

  CGContextSaveGState(ctx);
  [[UIBezierPath bezierPathWithRoundedRect:frame cornerRadius:radius] addClip];
  CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
  [image drawInRect:frame];
  CGContextRestoreGState(ctx);

  // A hairline on the banner's edge, so a banner whose border is the same
  // colour as the backdrop still reads as a thing sitting on the card.
  [[UIColor colorWithWhite:1.0 alpha:0.18] setStroke];
  UIBezierPath* edge = [UIBezierPath bezierPathWithRoundedRect:CGRectInset(frame, 0.5, 0.5)
                                                  cornerRadius:radius];
  edge.lineWidth = 1.0;
  [edge stroke];
}

+ (void)drawGeneratedTileForDiscID:(NSString*)discID inRect:(CGRect)rect context:(CGContextRef)ctx
{
  const CGSize size = rect.size;
  NSArray<UIColor*>* colours = [self coverColorsForDiscID:discID];
  NSString* label = discID.length ? discID.uppercaseString : @"??????";

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
}

@end
