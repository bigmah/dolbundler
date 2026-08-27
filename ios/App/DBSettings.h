// SPDX-License-Identifier: GPL-3.0-or-later

#import <Foundation/Foundation.h>

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

// A tap on a button gives a light impact. Off for anyone who finds it
// distracting, or who is chasing a frame-time hitch and wants the Taptic
// engine out of the picture.
@property(nonatomic, assign) BOOL hapticsEnabled;

// The fps/speed readout in the corner. On by default: this build exists to be
// measured, and PERFORMANCE.md's whole method is reading that number.
@property(nonatomic, assign) BOOL showsPerformance;

@end
