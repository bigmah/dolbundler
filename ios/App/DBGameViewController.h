// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

@class DBGameEntry;

@interface DBGameViewController : UIViewController
- (instancetype)initWithGame:(DBGameEntry*)game;
@end
