// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// The panel behind the button in the corner of a running game.
//
// It exists because the alternative was a bare X that quit without asking, on
// a screen whose whole surface is a controller -- a mis-tap in the corner
// ended the session. Everything here either pauses, adjusts the overlay, or
// quits, and the quit asks first.
@interface DBPauseMenuView : UIView

- (instancetype)initWithTitle:(NSString*)title;

// Called as the settings rows are changed, so the pad can restyle live behind
// the panel rather than after it is dismissed.
@property(nonatomic, copy) void (^onSettingsChanged)(void);
@property(nonatomic, copy) void (^onResume)(void);
@property(nonatomic, copy) void (^onQuit)(void);

- (void)presentInView:(UIView*)host;
- (void)dismissWithCompletion:(void (^)(void))completion;

@end
