// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// The on-screen GameCube pad. Feeds Dolphin's touch input overrider, which is
// the same path the Android overlay uses, so it drives the emulated pad
// directly rather than pretending to be a physical device.
//
// A physical controller paired over Bluetooth does not come through here --
// SDL picks those up on its own -- so the overlay hides itself when one is
// connected.
@interface DBTouchPadView : UIView
@end
