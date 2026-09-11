// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// A TV to put the game on: one the phone is AirPlaying to with Screen
// Mirroring, or a display on a cable.
//
// Mirroring alone shows the phone's screen on the TV, touch controls and all,
// pillarboxed from the phone's shape into the TV's. A window of the app's own
// on the second screen ends the mirroring for as long as it is up: the TV
// shows only what is put in that window, at the TV's shape, and the phone is
// free to show something else. Taking the window down hands the TV back to
// mirroring.
@interface DBExternalDisplay : NSObject

// Called on the main thread when a screen connects, disconnects, or changes
// mode.
@property(nonatomic, copy) void (^onChange)(void);

// Take any external screen, now and as one connects; -stop gives it back.
// Both are safe to repeat.
- (void)start;
- (void)stop;

// Fills the TV, or nil when there is none. Whatever is added to it is on the
// TV and not on the phone.
@property(nonatomic, readonly) UIView* contentView;

// The AirPlay receiver's name ("Living Room"), or nil when there is no TV or
// its name is not known -- a cable has none.
@property(nonatomic, readonly) NSString* receiverName;

@end
