// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

@class DBGameEntry;

// One card in the library grid: the disc's banner art, the title, and one
// line saying either how much room the game takes or why it cannot be played.
@interface DBGameCell : UICollectionViewCell

@property(class, nonatomic, readonly) NSString* reuseIdentifier;

// The height a card needs for a given width, so the layout and the cell agree
// on the artwork's proportions without either guessing.
+ (CGFloat)heightForWidth:(CGFloat)width;

- (void)configureWithEntry:(DBGameEntry*)entry;

@end
