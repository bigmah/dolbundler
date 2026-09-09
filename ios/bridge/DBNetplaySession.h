// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>

// All instance methods run on the lobby's serial worker queue. Snapshots are
// immutable and may be handed to UIKit. Keep the session alive through gameplay.
@interface DBNetplaySession : NSObject
- (instancetype)initWithGameRoot:(NSString*)root userDirectory:(NSString*)directory;
- (BOOL)openAsHost:(BOOL)host port:(uint16_t)port nickname:(NSString*)nickname
            error:(NSString**)error;
@property(nonatomic, readonly) uint16_t port;
- (NSDictionary*)snapshot;
- (void)setReady:(BOOL)ready;
- (void)start;
// Transfers Dolphin's synchronized boot data to the runtime, once.
- (BOOL)takeBootRequest;
- (void)close;
@end

#ifdef __cplusplus
extern "C" {
#endif
int db_netplay_active(void);
int db_netplay_can_boot(void);
void db_netplay_stop(void);
#ifdef __cplusplus
}
struct ModernGekkoModuleDesc;
const ModernGekkoModuleDesc* db_native_descriptor(const char* disc_id);
#endif
