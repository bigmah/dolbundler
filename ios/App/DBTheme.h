// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// One place for the colours the app uses twice or more, and for the artwork it
// has to invent.
//
// No game here ships with cover art -- the app is handed a disc image and
// nothing else -- so a tile is generated from the disc ID instead. It is
// derived, not random: the same disc always produces the same tile, which is
// what makes a library of six grey rectangles scannable at a glance.
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

// A generated tile for one disc: the gradient above, a mini-disc glyph, and
// the disc ID set across the bottom. `dimmed` renders the greyed-out version
// used for a disc this build has no module for.
+ (UIImage*)coverImageForDiscID:(NSString*)discID
                           size:(CGSize)size
                          scale:(CGFloat)scale
                         dimmed:(BOOL)dimmed;

@end
