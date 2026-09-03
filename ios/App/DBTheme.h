// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

@class DBBanner;

// One place for the colours the app uses twice or more, and for the artwork
// on the library's cards.
//
// A card is built around the disc's own banner (see DBBanner) when the game
// has one, which every extracted disc does. For a game root that somehow has
// none, a tile is generated from the disc ID instead. It is derived, not
// random: the same disc always produces the same tile, which is what makes a
// library of grey rectangles scannable at a glance.
@interface DBTheme : NSObject

// The GameCube's own button colours, used by the on-screen pad so the overlay
// reads the way the hardware does rather than as eight identical circles.
@property(class, nonatomic, readonly) UIColor* buttonA;
@property(class, nonatomic, readonly) UIColor* buttonB;
@property(class, nonatomic, readonly) UIColor* buttonXY;
@property(class, nonatomic, readonly) UIColor* buttonZ;
@property(class, nonatomic, readonly) UIColor* shoulder;
@property(class, nonatomic, readonly) UIColor* mainStick;
@property(class, nonatomic, readonly) UIColor* cStick;

// Tint for anything interactive outside a game.
@property(class, nonatomic, readonly) UIColor* accent;

// The two ends of a disc's generated gradient. Stable for a given disc ID.
+ (NSArray<UIColor*>*)coverColorsForDiscID:(NSString*)discID;

// The art for one game's card at the given size: the banner, pixel-sharp,
// floating over a blurred and darkened copy of itself -- or, with no banner,
// the generated tile. `dimmed` renders the greyed-out version used for a disc
// this build has no module for.
+ (UIImage*)cardImageForDiscID:(NSString*)discID
                        banner:(DBBanner*)banner
                          size:(CGSize)size
                         scale:(CGFloat)scale
                        dimmed:(BOOL)dimmed;

@end
