// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

@class DBGameEntry;

// One tile in the library grid: generated cover art, the title, and one line
// saying either how much room the game takes or why it cannot be played.
@interface DBGameCell : UICollectionViewCell

@property(class, nonatomic, readonly) NSString* reuseIdentifier;

// The height a tile needs for a given tile width, so the layout and the cell
// agree on the artwork's proportions without either guessing.
+ (CGFloat)heightForWidth:(CGFloat)width;

- (void)configureWithEntry:(DBGameEntry*)entry;

@end
