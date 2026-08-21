// SPDX-License-Identifier: GPL-3.0-or-later

#import <UIKit/UIKit.h>

// A view whose backing layer is a CAMetalLayer. The runtime draws straight
// into that layer; nothing here touches the drawable itself.
@interface DBMetalView : UIView
@end
