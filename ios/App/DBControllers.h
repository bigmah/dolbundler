// SPDX-License-Identifier: GPL-3.0-or-later

#import <Foundation/Foundation.h>

// Bluetooth controllers, one player each.
//
// The first controller is player 1, the next player 2, and so on. One that
// disconnects frees its player for the next to connect, so a controller that
// went to sleep and is switched back on gets its player back unless someone
// else has taken it. Controllers with player lights are told which player
// they are.
//
// Each controller feeds its own port through the input overrider the
// on-screen pad uses, not through one of Dolphin's controller backends. That
// is what lets two identical controllers be two players with no mapping file
// to write, and it is what lets the game screen ask the one thing it needs:
// does player 1 have a controller, or does the on-screen pad stand in?
//
// A port stays plugged in once a controller has used it, until the game
// ends. Pulling a controller out mid-game makes many GameCube games stop and
// ask for it back, and a Bluetooth controller left alone for a few minutes
// turns itself off -- which would put that stop in the middle of someone
// else's turn.
@interface DBControllers : NSObject

// Called on the main thread whenever a player gains or loses a controller.
@property(nonatomic, copy) void (^onChange)(void);

// Claim what is connected now and whatever connects later, and feed it to the
// game. -stop releases every controller and unplugs every port but the first.
// Both are safe to repeat.
- (void)start;
- (void)stop;

// The name the controller gives itself ("DualSense Wireless Controller"), or
// nil when no controller holds `port`.
- (NSString*)controllerNameForPort:(NSInteger)port;

// Whether the game sees a controller in `port`: port 0 always, the others
// once a controller has been given them.
- (BOOL)isPortPluggedIn:(NSInteger)port;

@end
