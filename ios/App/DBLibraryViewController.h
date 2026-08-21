// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

@interface DBLibraryViewController : UIViewController
// Import a disc handed to the app from Files, AirDrop, or the document picker.
- (void)importDiscAtURL:(NSURL*)url;
@end
