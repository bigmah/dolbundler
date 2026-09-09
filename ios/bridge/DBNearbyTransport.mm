// SPDX-License-Identifier: GPL-3.0-or-later
#import "DBNearbyTransport.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>

static const char* kNearbyService = "_dbmp7._udp";
static const size_t kDatagramLimit = 1500;
static double TransportTime(void) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static nw_parameters_t NearbyParameters(void) {
  nw_parameters_t parameters = nw_parameters_create_secure_udp(
      NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
  nw_parameters_set_include_peer_to_peer(parameters, true);
  nw_parameters_set_service_class(parameters, nw_service_class_responsive_data);
  return parameters;
}

@implementation DBNearbyRoom
@end

@interface DBDatagramTunnel : NSObject
@property(nonatomic, readonly) uint16_t port;
@property(nonatomic, readonly) NSTimeInterval lastActivity;
@property(nonatomic, readonly) BOOL stopped;
- (instancetype)initWithConnection:(nw_connection_t)connection queue:(dispatch_queue_t)queue
                       serverPort:(uint16_t)serverPort;
- (void)start:(void (^)(uint16_t))ready failure:(void (^)(void))failure;
- (void)stop;
@end

@implementation DBDatagramTunnel {
  nw_connection_t _connection;
  dispatch_queue_t _queue;
  dispatch_source_t _reader;
  dispatch_source_t _watchdog;
  int _socket;
  struct sockaddr_in _destination;
  uint16_t _port;
  NSUInteger _pendingSends;
  NSTimeInterval _lastActivity;
  NSTimeInterval _lastReceived;
  BOOL _stopped;
  BOOL _receiving;
  void (^_failure)(void);
}
- (instancetype)initWithConnection:(nw_connection_t)connection queue:(dispatch_queue_t)queue
                       serverPort:(uint16_t)serverPort {
  if (!(self = [super init])) return nil;
  _socket = -1;
  _connection = connection;
  _queue = queue;
  _lastActivity = TransportTime();
  _socket = socket(AF_INET, SOCK_DGRAM, 0);
  if (_socket < 0) return nil;
  struct sockaddr_in address = {};
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(_socket, (struct sockaddr*)&address, sizeof(address)) != 0 ||
      fcntl(_socket, F_SETFL, O_NONBLOCK) != 0) return nil;
  socklen_t length = sizeof(address);
  if (getsockname(_socket, (struct sockaddr*)&address, &length) != 0) return nil;
  _port = ntohs(address.sin_port);
  _destination = address;
  _destination.sin_port = htons(serverPort);
  return self;
}
- (uint16_t)port { return _port; }
- (NSTimeInterval)lastActivity { return _lastActivity; }
- (BOOL)stopped { return _stopped; }
- (void)start:(void (^)(uint16_t))ready failure:(void (^)(void))failure {
  _failure = [failure copy];
  __weak DBDatagramTunnel* weakSelf = self;
  _reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, _socket, 0, _queue);
  const int fd = _socket;
  dispatch_source_set_cancel_handler(_reader, ^{ close(fd); });
  dispatch_source_set_event_handler(_reader, ^{ [weakSelf readLocal]; });
  dispatch_resume(_reader);
  if (_failure) {
    _watchdog = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
    dispatch_source_set_timer(_watchdog, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                             NSEC_PER_SEC / 2, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(_watchdog, ^{
      DBDatagramTunnel* self = weakSelf;
      // Do not start this deadline during game-file verification, before ENet
      // has sent its first packet. Established ENet peers exchange heartbeats.
      if (self && self->_lastReceived && TransportTime() - self->_lastReceived > 8)
        [self connectionFailed];
    });
    dispatch_resume(_watchdog);
  }
  nw_connection_set_queue(_connection, _queue);
  nw_connection_set_state_changed_handler(_connection, ^(nw_connection_state_t state, nw_error_t error) {
    DBDatagramTunnel* strongSelf = weakSelf;
    if (!strongSelf || strongSelf->_stopped) return;
    if (state == nw_connection_state_ready) {
      if (!strongSelf->_receiving) { strongSelf->_receiving = YES; [strongSelf receive]; }
      if (ready) ready(strongSelf.port);
    } else if (state == nw_connection_state_failed) {
      [strongSelf connectionFailed];
    }
  });
  nw_connection_start(_connection);
}
- (void)readLocal {
  for (int count = 0; count < 64 && !_stopped; ++count) {
    uint8_t bytes[kDatagramLimit + 1];
    struct sockaddr_in source = {};
    socklen_t length = sizeof(source);
    const ssize_t size = recvfrom(_socket, bytes, sizeof(bytes), 0, (struct sockaddr*)&source, &length);
    if (size < 0) break;
    if (size == 0 || size > kDatagramLimit || source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
    if (!_destination.sin_port) _destination.sin_port = source.sin_port;
    if (_destination.sin_port != source.sin_port) continue;
    _lastActivity = TransportTime();
    // ENet will retransmit dropped datagrams. Never grow an unbounded send queue.
    if (_pendingSends >= 128) continue;
    ++_pendingSends;
    dispatch_data_t data = dispatch_data_create(bytes, size, _queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    __weak DBDatagramTunnel* weakSelf = self;
    nw_connection_send(_connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t error) {
      DBDatagramTunnel* strongSelf = weakSelf;
      if (strongSelf && strongSelf->_pendingSends) --strongSelf->_pendingSends;
      if (error) [strongSelf connectionFailed];
    });
  }
}
- (void)receive {
  if (_stopped) return;
  __weak DBDatagramTunnel* weakSelf = self;
  nw_connection_receive_message(_connection, ^(dispatch_data_t data, nw_content_context_t context,
                                              bool complete, nw_error_t error) {
    DBDatagramTunnel* strongSelf = weakSelf;
    if (!strongSelf || strongSelf->_stopped) return;
    if (data && complete && dispatch_data_get_size(data) <= kDatagramLimit) {
      const void* bytes = nullptr;
      size_t size = 0;
      dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &size);
      if (mapped && size && strongSelf->_destination.sin_port) {
        sendto(strongSelf->_socket, bytes, size, 0, (struct sockaddr*)&strongSelf->_destination,
               sizeof(strongSelf->_destination));
        strongSelf->_lastActivity = strongSelf->_lastReceived = TransportTime();
      }
    }
    if (!error) [strongSelf receive];
    else [strongSelf connectionFailed];
  });
}
- (void)connectionFailed {
  if (_stopped) return;
  void (^failure)(void) = _failure;
  [self stop];
  if (failure) failure();
}
- (void)stop {
  if (_stopped) return;
  _stopped = YES;
  _failure = nil;
  if (_watchdog) { dispatch_source_cancel(_watchdog); _watchdog = nil; }
  if (_connection) {
    nw_connection_set_state_changed_handler(_connection, nil);
    nw_connection_cancel(_connection);
  }
  if (_reader) { dispatch_source_cancel(_reader); _reader = nil; _socket = -1; }
  else if (_socket >= 0) { close(_socket); _socket = -1; }
}
- (void)dealloc { [self stop]; }
@end

@implementation DBNearbyTransport {
  dispatch_queue_t _queue;
  nw_listener_t _listener;
  nw_browser_t _browser;
  NSMutableDictionary<NSString*, DBNearbyRoom*>* _rooms;
  NSMutableArray<DBDatagramTunnel*>* _tunnels;
  dispatch_source_t _expiry;
  BOOL _stopped;
}
- (instancetype)init {
  if ((self = [super init])) {
    _queue = dispatch_queue_create("com.bigmah.dolbundler.nearby.transport", DISPATCH_QUEUE_SERIAL);
    _rooms = [NSMutableDictionary dictionary];
    _tunnels = [NSMutableArray array];
  }
  return self;
}
- (void)reportFailure:(NSString*)message {
  dispatch_async(dispatch_get_main_queue(), ^{ if (self.failed) self.failed(message); });
}
- (void)reportStatus:(NSString*)message {
  dispatch_async(dispatch_get_main_queue(), ^{ if (self.statusChanged) self.statusChanged(message); });
}
- (void)browse {
  dispatch_async(_queue, ^{
    if (self->_stopped || self->_browser) return;
    self->_browser = nw_browser_create(nw_browse_descriptor_create_bonjour_service(kNearbyService, nullptr), NearbyParameters());
    nw_browser_set_queue(self->_browser, self->_queue);
    __weak DBNearbyTransport* weakSelf = self;
    nw_browser_set_state_changed_handler(self->_browser, ^(nw_browser_state_t state, nw_error_t error) {
      if (state == nw_browser_state_waiting)
        [weakSelf reportStatus:@"Allow Local Network access when asked and keep Wi-Fi enabled. Nearby discovery will resume automatically."];
      else if (state == nw_browser_state_failed)
        [weakSelf reportFailure:@"Nearby discovery is unavailable. Enable Wi-Fi and allow Local Network access in Settings."];
    });
    nw_browser_set_browse_results_changed_handler(self->_browser,
      ^(nw_browse_result_t oldResult, nw_browse_result_t newResult, bool complete) {
        DBNearbyTransport* strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_stopped) return;
        if (oldResult) {
          nw_endpoint_t endpoint = nw_browse_result_copy_endpoint(oldResult);
          [strongSelf->_rooms removeObjectForKey:@(nw_endpoint_get_bonjour_service_name(endpoint))];
        }
        if (newResult) {
          nw_endpoint_t endpoint = nw_browse_result_copy_endpoint(newResult);
          DBNearbyRoom* room = [DBNearbyRoom new];
          room.name = @(nw_endpoint_get_bonjour_service_name(endpoint));
          room.endpoint = endpoint;
          strongSelf->_rooms[room.name] = room;
        }
        if (complete) {
          NSArray* rooms = [strongSelf->_rooms.allValues sortedArrayUsingComparator:^NSComparisonResult(DBNearbyRoom* a, DBNearbyRoom* b) {
            return [a.name compare:b.name];
          }];
          dispatch_async(dispatch_get_main_queue(), ^{ if (strongSelf.roomsChanged) strongSelf.roomsChanged(rooms); });
        }
      });
    nw_browser_start(self->_browser);
  });
}
- (void)hostWithName:(NSString*)name serverPort:(uint16_t)port ready:(void (^)(uint16_t))ready {
  dispatch_async(_queue, ^{
    if (self->_stopped) return;
    if (self->_browser) { nw_browser_cancel(self->_browser); self->_browser = nil; }
    self->_listener = nw_listener_create(NearbyParameters());
    if (!self->_listener) { [self reportFailure:@"Could not advertise this room."]; return; }
    nw_listener_set_queue(self->_listener, self->_queue);
    nw_listener_set_advertise_descriptor(self->_listener,
      nw_advertise_descriptor_create_bonjour_service(name.UTF8String, kNearbyService, nullptr));
    __weak DBNearbyTransport* weakSelf = self;
    nw_listener_set_state_changed_handler(self->_listener, ^(nw_listener_state_t state, nw_error_t error) {
      DBNearbyTransport* strongSelf = weakSelf;
      if (!strongSelf || strongSelf->_stopped) return;
      if (state == nw_listener_state_ready) {
        uint16_t listeningPort = nw_listener_get_port(strongSelf->_listener);
        dispatch_async(dispatch_get_main_queue(), ^{ if (ready) ready(listeningPort); });
      } else if (state == nw_listener_state_waiting) {
        [strongSelf reportStatus:@"Allow Local Network access when asked and keep Wi-Fi enabled. Your room will appear once the connection is ready."];
      } else if (state == nw_listener_state_failed) {
        [strongSelf reportFailure:@"Could not advertise this room. Enable Wi-Fi and allow Local Network access in Settings."];
      }
    });
    nw_listener_set_new_connection_handler(self->_listener, ^(nw_connection_t connection) {
      DBNearbyTransport* strongSelf = weakSelf;
      if (!strongSelf || strongSelf->_stopped || strongSelf->_tunnels.count >= 8) {
        nw_connection_cancel(connection); return;
      }
      DBDatagramTunnel* tunnel = [[DBDatagramTunnel alloc] initWithConnection:connection
          queue:strongSelf->_queue serverPort:port];
      if (!tunnel) { nw_connection_cancel(connection); return; }
      [strongSelf->_tunnels addObject:tunnel];
      [tunnel start:nil failure:nil];
    });
    self->_expiry = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self->_queue);
    dispatch_source_set_timer(self->_expiry, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                             5 * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(self->_expiry, ^{
      DBNearbyTransport* strongSelf = weakSelf;
      if (!strongSelf) return;
      for (DBDatagramTunnel* tunnel in [strongSelf->_tunnels copy]) {
        if (tunnel.stopped || TransportTime() - tunnel.lastActivity > 20) {
          [tunnel stop]; [strongSelf->_tunnels removeObject:tunnel];
        }
      }
    });
    dispatch_resume(self->_expiry);
    nw_listener_start(self->_listener);
  });
}
- (void)joinEndpoint:(nw_endpoint_t)endpoint ready:(void (^)(uint16_t))ready {
  dispatch_async(_queue, ^{
    if (self->_stopped) return;
    if (self->_browser) { nw_browser_cancel(self->_browser); self->_browser = nil; }
    nw_connection_t connection = nw_connection_create(endpoint, NearbyParameters());
    DBDatagramTunnel* tunnel = [[DBDatagramTunnel alloc] initWithConnection:connection queue:self->_queue serverPort:0];
    if (!tunnel) { [self reportFailure:@"Could not create a connection to this room."]; return; }
    [self->_tunnels addObject:tunnel];
    __weak DBNearbyTransport* weakSelf = self;
    __block BOOL connected = NO;
    [tunnel start:^(uint16_t port) {
      if (connected) return;
      connected = YES;
      dispatch_async(dispatch_get_main_queue(), ^{ if (ready) ready(port); });
    } failure:^{ [weakSelf reportFailure:@"Connection to the room failed."]; }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), self->_queue, ^{
      if (!connected && !tunnel.stopped) {
        [tunnel stop];
        [weakSelf reportFailure:@"The room did not respond. Keep both phones nearby with Wi-Fi enabled."];
      }
    });
  });
}
- (void)stop {
  self.roomsChanged = nil;
  self.failed = nil;
  self.statusChanged = nil;
  dispatch_async(_queue, ^{
    self->_stopped = YES;
    if (self->_browser) { nw_browser_cancel(self->_browser); self->_browser = nil; }
    if (self->_listener) { nw_listener_cancel(self->_listener); self->_listener = nil; }
    if (self->_expiry) { dispatch_source_cancel(self->_expiry); self->_expiry = nil; }
    for (DBDatagramTunnel* tunnel in self->_tunnels) [tunnel stop];
    [self->_tunnels removeAllObjects];
  });
}
@end
