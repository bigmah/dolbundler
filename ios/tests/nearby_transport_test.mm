// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the actual Apple transport on macOS: three independent phone tunnels
// share one host, with distinct source ports and intact datagram boundaries.
#import "DBNearbyTransport.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <set>

static int Socket(uint16_t* port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in address{};
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0 || bind(fd, (sockaddr*)&address, sizeof(address)) || fcntl(fd, F_SETFL, O_NONBLOCK)) abort();
  socklen_t size = sizeof(address);
  if (getsockname(fd, (sockaddr*)&address, &size)) abort();
  *port = ntohs(address.sin_port);
  return fd;
}

static bool TestAbruptHostLoss() {
  dispatch_queue_t queue = dispatch_get_main_queue();
  uint16_t echoPort, sourcePort;
  const int echo = Socket(&echoPort);
  const int source = Socket(&sourcePort);
  DBNearbyTransport* host = [DBNearbyTransport new];
  DBNearbyTransport* client = [DBNearbyTransport new];
  __block uint16_t proxyPort = 0;
  __block bool hostStopped = false, failed = false, unexpectedFailure = false;
  client.failed = ^(NSString* message) {
    unexpectedFailure = !hostStopped;
    failed = true;
  };
  host.failed = ^(NSString* message) { unexpectedFailure = true; failed = true; };
  dispatch_source_t echoReader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, echo, 0, queue);
  dispatch_source_set_cancel_handler(echoReader, ^{ close(echo); });
  dispatch_source_set_event_handler(echoReader, ^{
    uint8_t bytes[16];
    sockaddr_in from{};
    socklen_t length = sizeof(from);
    ssize_t size;
    while ((size = recvfrom(echo, bytes, sizeof(bytes), 0, (sockaddr*)&from, &length)) > 0)
      sendto(echo, bytes, size, 0, (sockaddr*)&from, length);
  });
  dispatch_resume(echoReader);
  dispatch_source_t reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, source, 0, queue);
  dispatch_source_set_cancel_handler(reader, ^{ close(source); });
  dispatch_source_set_event_handler(reader, ^{
    uint8_t bytes[16];
    while (recv(source, bytes, sizeof(bytes), 0) > 0) {
      if (!hostStopped) {
        hostStopped = true;
        [host stop];
      }
    }
  });
  dispatch_resume(reader);
  dispatch_source_t sender = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
  dispatch_source_set_timer(sender, DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC, 0);
  dispatch_source_set_event_handler(sender, ^{
    if (!proxyPort) return;
    sockaddr_in destination{};
    destination.sin_len = sizeof(destination);
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(proxyPort);
    const uint8_t byte = 42;
    sendto(source, &byte, 1, 0, (sockaddr*)&destination, sizeof(destination));
  });
  dispatch_resume(sender);
  [host hostWithName:[@"Disconnect Test " stringByAppendingString:NSUUID.UUID.UUIDString]
         serverPort:echoPort ready:^(uint16_t port) {
    NSString* portString = [NSString stringWithFormat:@"%u", port];
    [client joinEndpoint:nw_endpoint_create_host("127.0.0.1", portString.UTF8String)
                   ready:^(uint16_t localPort) { proxyPort = localPort; }];
  }];
  NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:12];
  while (!failed && deadline.timeIntervalSinceNow > 0)
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
  [host stop];
  [client stop];
  dispatch_source_cancel(sender);
  dispatch_source_cancel(reader);
  dispatch_source_cancel(echoReader);
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
  if (!hostStopped || !failed || unexpectedFailure) {
    fprintf(stderr, "Host-loss notification failed: hostStopped=%d failed=%d unexpected=%d\n",
            hostStopped, failed, unexpectedFailure);
    return false;
  }
  printf("Passed: an established peer reports abrupt host loss.\n");
  return true;
}

int main() {
  @autoreleasepool {
    dispatch_queue_t queue = dispatch_get_main_queue();
    uint16_t echoPort;
    int echo = Socket(&echoPort);
    __block std::set<uint16_t> peers;
    __block NSUInteger received = 0;
    __block bool done = false;
    dispatch_source_t echoReader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, echo, 0, queue);
    dispatch_source_set_event_handler(echoReader, ^{
      uint8_t data[1600];
      sockaddr_in from{};
      socklen_t size = sizeof(from);
      ssize_t count;
      while ((count = recvfrom(echo, data, sizeof(data), 0, (sockaddr*)&from, &size)) > 0) {
        peers.insert(from.sin_port);
        sendto(echo, data, count, 0, (sockaddr*)&from, size);
      }
    });
    dispatch_resume(echoReader);
    DBNearbyTransport* host = [DBNearbyTransport new];
    NSMutableArray<DBNearbyTransport*>* clients = [NSMutableArray array];
    NSMutableArray* readers = [NSMutableArray array];
    NSMutableArray* senders = [NSMutableArray array];
    void (^failure)(NSString*) = ^(NSString* message) { fprintf(stderr, "%s\n", message.UTF8String); exit(1); };
    host.failed = failure;
    NSString* room = [@"Transport Test " stringByAppendingString:NSUUID.UUID.UUIDString];
    [host hostWithName:room serverPort:echoPort ready:^(uint16_t port) {
      for (int i = 0; i < 3; ++i) {
        DBNearbyTransport* client = [DBNearbyTransport new];
        client.failed = failure;
        [clients addObject:client];
        __weak DBNearbyTransport* weakClient = client;
        client.roomsChanged = ^(NSArray<DBNearbyRoom*>* rooms) {
          for (DBNearbyRoom* found in rooms) {
            if (![found.name isEqualToString:room]) continue;
            DBNearbyTransport* joiningClient = weakClient;
            joiningClient.roomsChanged = nil;
            [joiningClient joinEndpoint:found.endpoint ready:^(uint16_t localPort) {
          uint16_t sourcePort;
          int fd = Socket(&sourcePort);
          dispatch_source_t reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, queue);
          [readers addObject:reader];
          dispatch_source_set_cancel_handler(reader, ^{ close(fd); });
          dispatch_source_set_event_handler(reader, ^{
            uint8_t data[1600];
            ssize_t size;
            while ((size = recv(fd, data, sizeof(data), 0)) > 0) {
              if (size != (data[1] == 29 ? 1392 : 32 + data[1] * 47) || data[0] != i) exit(2);
              for (int j = 2; j < size; ++j) if (data[j] != (uint8_t)(i + j)) exit(3);
              if (++received == 90) {
                if (peers.size() != 3) exit(4);
                [host stop];
                for (DBNearbyTransport* client in clients) [client stop];
                for (dispatch_source_t sender in senders) dispatch_source_cancel(sender);
                for (dispatch_source_t reader in readers) dispatch_source_cancel(reader);
                dispatch_source_cancel(echoReader);
                close(echo);
                done = true;
                printf("Passed: 90 datagrams across three isolated peer tunnels.\n");
              }
            }
          });
          dispatch_resume(reader);
          __block int sequence = 0;
          dispatch_source_t sender = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
          [senders addObject:sender];
          dispatch_source_set_timer(sender, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), 20 * NSEC_PER_MSEC, 0);
          dispatch_source_set_event_handler(sender, ^{
            if (sequence == 30) { dispatch_source_cancel(sender); return; }
            uint8_t data[1500];
            int size = sequence == 29 ? 1392 : 32 + sequence * 47;
            for (int j = 0; j < size; ++j) data[j] = (uint8_t)(i + j);
            data[0] = i;
            data[1] = sequence++;
            sockaddr_in destination{};
            destination.sin_len = sizeof(destination);
            destination.sin_family = AF_INET;
            destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            destination.sin_port = htons(localPort);
            sendto(fd, data, size, 0, (sockaddr*)&destination, sizeof(destination));
          });
          dispatch_resume(sender);
            }];
            break;
          }
        };
        [client browse];
      }
    }];
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:20];
    while (!done && deadline.timeIntervalSinceNow > 0)
      [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    if (!done) { fprintf(stderr, "Timed out after %lu/90 datagrams\n", (unsigned long)received); return 5; }
    // Allow cancellation handlers to close their sockets before exiting.
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    if (!TestAbruptHostLoss()) return 6;
  }
  return 0;
}
