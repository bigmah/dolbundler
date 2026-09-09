// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

@class DBGameEntry;

@interface DBGameViewController : UIViewController
- (instancetype)initWithGame:(DBGameEntry*)game;
// Called on the main queue after a nearby game's runtime has stopped and its
// full-screen view has dismissed. The lobby owns the networking lifetime.
@property(nonatomic, copy) void (^nearbyCompletion)(NSString* error);
@end
