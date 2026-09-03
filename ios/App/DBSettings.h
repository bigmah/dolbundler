// SPDX-License-Identifier: GPL-3.0-or-later

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

// The handful of things someone can change about how a game plays, kept in
// NSUserDefaults so they survive a relaunch.
//
// Deliberately small. Everything here is about the *overlay*, not about
// emulation: nothing in this file can change how a game runs, only how the
// controls on top of it look and feel.
@interface DBSettings : NSObject

+ (instancetype)shared;

// How visible the on-screen pad is, 0.25 to 1.0. Some games are dark enough
// that a bright overlay is distracting and some are busy enough that a faint
// one is unfindable, so it is a slider rather than a fixed compromise.
@property(nonatomic, assign) CGFloat padOpacity;

// A multiplier on the size of every control, 0.7 to 1.4. Thumbs differ more
// than phones do.
@property(nonatomic, assign) CGFloat padScale;
@property(class, nonatomic, readonly) CGFloat minPadScale;
@property(class, nonatomic, readonly) CGFloat maxPadScale;

// A tap on a button gives a light impact. Off for anyone who finds it
// distracting, or who is chasing a frame-time hitch and wants the Taptic
// engine out of the picture.
@property(nonatomic, assign) BOOL hapticsEnabled;

// The fps/speed readout in the corner. On by default: this build exists to be
// measured, and PERFORMANCE.md's whole method is reading that number.
@property(nonatomic, assign) BOOL showsPerformance;

// Where each control has been dragged to, as a fraction of the pad's safe
// rectangle -- (0, 0) is its top-left corner, (1, 1) the bottom-right -- so a
// layout made on one phone lands in about the same place on another. A
// control with no entry is wherever the pad puts it by default.
- (BOOL)padPosition:(CGPoint*)fraction forControl:(NSString*)name;
- (void)setPadPosition:(CGPoint)fraction forControl:(NSString*)name;
@property(nonatomic, readonly) BOOL hasCustomPadLayout;
// Forget every dragged position and the size multiplier.
- (void)resetPadLayout;

// Test hook. DOLBUNDLER_UI_PREVIEW=library|settings|game|menu|edit walks the app to
// that screen without a game running: every library entry reads as playable,
// the game screen opens without booting anything, and the pad stays visible
// whether or not a controller is attached. It exists because there is no way
// to tap the simulator from a script, and the only way to look at a screen
// that sits behind two taps is to have the app take them itself.
@property(class, nonatomic, readonly) NSString* uiPreviewMode;

@end
