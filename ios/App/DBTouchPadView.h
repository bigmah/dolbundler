// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// The on-screen GameCube pad. Feeds Dolphin's touch input overrider, which is
// the same path the Android overlay uses, so it drives the emulated pad
// directly rather than pretending to be a physical device.
//
// Every control the hardware has is here: both analog sticks, the D-pad, all
// six face and shoulder buttons, and Start. Nothing is drawn with -drawRect:;
// each control owns a CAShapeLayer, so pressing a button repaints that button
// and not the whole overlay sitting on top of a running game.
//
// A physical controller paired over Bluetooth does not come through here --
// SDL picks those up on its own -- so the overlay hides itself when one is
// connected.
@interface DBTouchPadView : UIView

// Re-read DBSettings. Opacity is applied per layer rather than through the
// view's own alpha: a translucent view with this many sublayers would force
// Core Animation into an offscreen group-opacity pass every frame, on top of
// a Metal layer that is already the thing being measured.
- (void)refreshFromSettings;

@end
