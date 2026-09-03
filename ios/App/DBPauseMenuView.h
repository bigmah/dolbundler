// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

@class DBGameEntry;

// The panel behind the button at the top of a running game.
//
// It exists because the alternative was a bare X that quit without asking, on
// a screen whose whole surface is a controller -- a mis-tap in the corner
// ended the session. Everything here either pauses, adjusts the overlay, or
// quits, and the quit asks first.
//
// Laid out in two columns because the screen it sits on is landscape: the
// game and the two ways out on the left, the settings on the right. A single
// stack of rows did not fit a phone's short side once the layout editor and
// the size slider joined it.
@interface DBPauseMenuView : UIView

- (instancetype)initWithGame:(DBGameEntry*)game;

// Called as the settings rows are changed, so the pad can restyle live behind
// the panel rather than after it is dismissed.
@property(nonatomic, copy) void (^onSettingsChanged)(void);
@property(nonatomic, copy) void (^onResume)(void);
@property(nonatomic, copy) void (^onQuit)(void);
// Hand over to the layout editor. The panel is dismissed by whoever answers.
@property(nonatomic, copy) void (^onEditLayout)(void);
@property(nonatomic, copy) void (^onResetLayout)(void);

- (void)presentInView:(UIView*)host;
- (void)dismissWithCompletion:(void (^)(void))completion;

@end
