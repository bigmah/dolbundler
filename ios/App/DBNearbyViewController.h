// SPDX-License-Identifier: GPL-3.0-or-later
#import <UIKit/UIKit.h>
@class DBGameEntry;
@interface DBNearbyViewController : UITableViewController
- (instancetype)initWithGame:(DBGameEntry*)game;
@end
