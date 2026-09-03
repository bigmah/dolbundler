// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// The app's settings, reachable from the library.
//
// Everything on it is also on the pause menu of a running game, where the
// change can be seen. It exists anyway for the things someone wants to do
// without launching a game -- put the controls back where they were, check
// how much of the phone the library is using, see which games this build was
// made with -- and so the app has a settings screen where a settings screen
// is expected to be.
@interface DBSettingsViewController : UITableViewController
@end
