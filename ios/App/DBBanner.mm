// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBBanner.h"

namespace
{
constexpr NSUInteger kBannerWidth = 96;
constexpr NSUInteger kBannerHeight = 32;
constexpr NSUInteger kImageOffset = 0x20;
constexpr NSUInteger kImageBytes = kBannerWidth * kBannerHeight * 2;
constexpr NSUInteger kTextOffset = kImageOffset + kImageBytes;  // 0x1820
// One language block: short title, short maker, long title, long maker, and
// the description. BNR1 has one; BNR2 (PAL) has six, and the first is English.
constexpr NSUInteger kShortTitleLength = 32;
constexpr NSUInteger kShortMakerLength = 32;
constexpr NSUInteger kLongTitleLength = 64;
constexpr NSUInteger kLongMakerLength = 64;
constexpr NSUInteger kDescriptionLength = 128;
constexpr NSUInteger kTextBlockLength = kShortTitleLength + kShortMakerLength +
                                        kLongTitleLength + kLongMakerLength +
                                        kDescriptionLength;  // 0x140

// RGB5A3 is stored as 4x4 tiles of big-endian 16-bit texels. A texel with the
// top bit set is opaque RGB555; without it, three bits of alpha and four of
// each channel. The result is premultiplied RGBA8, which is what Core Graphics
// wants for a bitmap it will composite.
NSData* DecodeRGB5A3(const uint8_t* texels)
{
  NSMutableData* pixels = [NSMutableData dataWithLength:kBannerWidth * kBannerHeight * 4];
  uint8_t* out = (uint8_t*)pixels.mutableBytes;

  const uint8_t* in = texels;
  for (NSUInteger tileY = 0; tileY < kBannerHeight; tileY += 4)
  {
    for (NSUInteger tileX = 0; tileX < kBannerWidth; tileX += 4)
    {
      for (NSUInteger y = 0; y < 4; y++)
      {
        for (NSUInteger x = 0; x < 4; x++)
        {
          const uint16_t v = (uint16_t)((in[0] << 8) | in[1]);
          in += 2;

          unsigned r, g, b, a;
          if (v & 0x8000)
          {
            r = (v >> 10) & 0x1f;
            g = (v >> 5) & 0x1f;
            b = v & 0x1f;
            r = (r << 3) | (r >> 2);
            g = (g << 3) | (g >> 2);
            b = (b << 3) | (b >> 2);
            a = 255;
          }
          else
          {
            a = (v >> 12) & 0x7;
            a = (a << 5) | (a << 2) | (a >> 1);
            r = ((v >> 8) & 0xf) * 17;
            g = ((v >> 4) & 0xf) * 17;
            b = (v & 0xf) * 17;
          }

          uint8_t* p = out + ((tileY + y) * kBannerWidth + (tileX + x)) * 4;
          p[0] = (uint8_t)(r * a / 255);
          p[1] = (uint8_t)(g * a / 255);
          p[2] = (uint8_t)(b * a / 255);
          p[3] = (uint8_t)a;
        }
      }
    }
  }
  return pixels;
}

UIImage* ImageFromPixels(NSData* pixels)
{
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
  CGImageRef cgImage = CGImageCreate(kBannerWidth, kBannerHeight, 8, 32, kBannerWidth * 4, space,
                                     (CGBitmapInfo)kCGImageAlphaPremultipliedLast,
                                     provider, nullptr, false, kCGRenderingIntentDefault);
  CGDataProviderRelease(provider);
  CGColorSpaceRelease(space);
  if (!cgImage)
    return nil;
  UIImage* image = [UIImage imageWithCGImage:cgImage scale:1.0 orientation:UIImageOrientationUp];
  CGImageRelease(cgImage);
  return image;
}

// A fixed-width field, NUL-padded, in an encoding the disc does not name.
NSString* FieldString(const uint8_t* bytes, NSUInteger length, NSStringEncoding encoding)
{
  NSUInteger used = 0;
  while (used < length && bytes[used] != 0)
    used++;
  NSString* text = [[NSString alloc] initWithBytes:bytes length:used encoding:encoding];
  if (!text)
    text = [[NSString alloc] initWithBytes:bytes length:used encoding:NSISOLatin1StringEncoding];
  return [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
             ?: @"";
}

NSCache<NSString*, DBBanner*>* BannerCache()
{
  static NSCache<NSString*, DBBanner*>* cache;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    cache = [[NSCache alloc] init];
    cache.countLimit = 64;
  });
  return cache;
}
}  // namespace

@implementation DBBanner

+ (instancetype)bannerAtPath:(NSString*)path discID:(NSString*)discID
{
  if (!path.length)
    return nil;
  NSData* data = [NSData dataWithContentsOfFile:path];
  if (data.length < kTextOffset + kTextBlockLength)
    return nil;

  const uint8_t* bytes = (const uint8_t*)data.bytes;
  if (memcmp(bytes, "BNR1", 4) != 0 && memcmp(bytes, "BNR2", 4) != 0)
    return nil;

  DBBanner* banner = [[DBBanner alloc] init];
  banner->_image = ImageFromPixels(DecodeRGB5A3(bytes + kImageOffset));
  if (!banner->_image)
    return nil;

  // The region letter is the fourth character of the disc ID. Only Japan
  // writes Shift-JIS; a PAL disc's six blocks and an NTSC-U disc's one are all
  // Windows-1252, whatever the file's magic says.
  const BOOL japanese = discID.length >= 4 && [discID characterAtIndex:3] == 'J';
  const NSStringEncoding encoding =
      japanese ? NSShiftJISStringEncoding : NSWindowsCP1252StringEncoding;

  const uint8_t* text = bytes + kTextOffset;
  NSString* shortTitle = FieldString(text, kShortTitleLength, encoding);
  NSString* shortMaker = FieldString(text + kShortTitleLength, kShortMakerLength, encoding);
  NSString* longTitle = FieldString(text + kShortTitleLength + kShortMakerLength,
                                    kLongTitleLength, encoding);
  NSString* longMaker = FieldString(
      text + kShortTitleLength + kShortMakerLength + kLongTitleLength, kLongMakerLength, encoding);
  NSString* summary = FieldString(text + kTextBlockLength - kDescriptionLength,
                                  kDescriptionLength, encoding);

  // Titles are laid out for a 640-pixel screen and may carry a line break in
  // the middle; a label wraps them better than the disc did. The description
  // keeps its breaks: those are paragraphs, not fitting.
  NSString* title = longTitle.length ? longTitle : shortTitle;
  banner->_title = [title stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
  banner->_maker = longMaker.length ? longMaker : shortMaker;
  banner->_summary = summary;
  return banner;
}

+ (instancetype)cachedBannerAtPath:(NSString*)path discID:(NSString*)discID
{
  if (!path.length)
    return nil;
  DBBanner* cached = [BannerCache() objectForKey:path];
  if (cached)
    return cached;
  DBBanner* banner = [self bannerAtPath:path discID:discID];
  if (banner)
    [BannerCache() setObject:banner forKey:path];
  return banner;
}

@end
