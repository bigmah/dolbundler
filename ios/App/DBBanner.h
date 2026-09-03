// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// The disc's own artwork.
//
// Every GameCube disc carries `opening.bnr` at the root of its filesystem: a
// 96x32 picture plus the game's name, its maker and a two-line description,
// which is what the console's own menu shows for an inserted disc. The import
// extracts it with everything else, so a library built from extracted discs
// has real art for every game without ever going near a network -- and it is
// the same picture the Mac app uses for its covers.
@interface DBBanner : NSObject

// 96x32, decoded from RGB5A3. Never nil on a banner that parsed.
@property(nonatomic, readonly) UIImage* image;
// The banner's long title, e.g. "Super Smash Bros. Melee" where the disc
// header only says "Super Smash Bros Melee". Empty rather than nil if the disc
// left it blank.
@property(nonatomic, readonly) NSString* title;
@property(nonatomic, readonly) NSString* maker;
@property(nonatomic, readonly) NSString* summary;

// Parse the banner at `path`, or nil if there is no file there or it is not
// one. `discID` chooses the text encoding: Japanese discs write Shift-JIS,
// everything else Windows-1252, and nothing in the file says which.
+ (instancetype)bannerAtPath:(NSString*)path discID:(NSString*)discID;

// bannerAtPath: with a per-disc cache in front of it, keyed on the path.
+ (instancetype)cachedBannerAtPath:(NSString*)path discID:(NSString*)discID;

@end
