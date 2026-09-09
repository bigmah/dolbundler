// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>
#import <Network/Network.h>

@interface DBNearbyRoom : NSObject
@property(nonatomic, copy) NSString* name;
@property(nonatomic, strong) nw_endpoint_t endpoint;
@end

// ENet datagrams travel unchanged through Network.framework. Each remote peer
// gets a separate loopback UDP socket, preserving ENet's peer addressing,
// channels, retransmission, and congestion control. Apple owns the Wi-Fi path.
// Public methods and callbacks use the main queue; socket work uses its own queue.
@interface DBNearbyTransport : NSObject
@property(nonatomic, copy) void (^roomsChanged)(NSArray<DBNearbyRoom*>* rooms);
@property(nonatomic, copy) void (^failed)(NSString* message);
@property(nonatomic, copy) void (^statusChanged)(NSString* message);
- (void)browse;
- (void)hostWithName:(NSString*)name serverPort:(uint16_t)port
              ready:(void (^)(uint16_t advertisedPort))ready;
- (void)joinEndpoint:(nw_endpoint_t)endpoint ready:(void (^)(uint16_t localPort))ready;
- (void)stop;
@end
