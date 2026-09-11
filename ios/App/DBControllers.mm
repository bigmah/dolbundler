// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBControllers.h"

#import <GameController/GameController.h>

#include "dolbundler_run.h"

namespace
{
// A GameCube trigger is analog for most of its travel and clicks at the
// bottom. An analog trigger reports that click this close to fully pressed;
// a digital one reports 0 or 1 and crosses it either way.
constexpr float kTriggerClick = 0.9f;

void SetButton(int port, DBPadControl control, BOOL pressed)
{
  db_set_port_control(port, control, pressed ? 1.0 : 0.0);
}

// The whole pad, every time anything on it changes. Sending only the element
// that moved would be less work, and wrong whenever one update is lost -- one
// that arrives during boot is dropped -- leaving a button held until it
// happened to be pressed again.
void Feed(int port, GCExtendedGamepad* pad)
{
  // Face buttons go by position, not by the letter printed on them. The
  // GameCube's A is the big button in the middle with B below and to its
  // left, X to its right and Y above, so the bottom button of a modern diamond
  // is A and the left one is B. GameController names buttons by the Xbox
  // positions: on a DualSense this makes cross A, square B, circle X and
  // triangle Y.
  SetButton(port, DB_PAD_A, pad.buttonA.isPressed);
  SetButton(port, DB_PAD_B, pad.buttonX.isPressed);
  SetButton(port, DB_PAD_X, pad.buttonB.isPressed);
  SetButton(port, DB_PAD_Y, pad.buttonY.isPressed);
  // The GameCube has one Z. Either bumper presses it, so it is under whichever
  // finger reaches for it.
  SetButton(port, DB_PAD_Z, pad.rightShoulder.isPressed || pad.leftShoulder.isPressed);
  SetButton(port, DB_PAD_START, pad.buttonMenu.isPressed);

  SetButton(port, DB_PAD_DPAD_UP, pad.dpad.up.isPressed);
  SetButton(port, DB_PAD_DPAD_DOWN, pad.dpad.down.isPressed);
  SetButton(port, DB_PAD_DPAD_LEFT, pad.dpad.left.isPressed);
  SetButton(port, DB_PAD_DPAD_RIGHT, pad.dpad.right.isPressed);

  const float left = pad.leftTrigger.value;
  const float right = pad.rightTrigger.value;
  db_set_port_control(port, DB_PAD_L_ANALOG, left);
  db_set_port_control(port, DB_PAD_R_ANALOG, right);
  SetButton(port, DB_PAD_L_DIGITAL, left >= kTriggerClick);
  SetButton(port, DB_PAD_R_DIGITAL, right >= kTriggerClick);

  // GameController's sticks are +Y up, the same as Dolphin's.
  db_set_port_control(port, DB_PAD_MAIN_STICK_X, pad.leftThumbstick.xAxis.value);
  db_set_port_control(port, DB_PAD_MAIN_STICK_Y, pad.leftThumbstick.yAxis.value);
  db_set_port_control(port, DB_PAD_C_STICK_X, pad.rightThumbstick.xAxis.value);
  db_set_port_control(port, DB_PAD_C_STICK_Y, pad.rightThumbstick.yAxis.value);
}

void Release(int port)
{
  for (int control = DB_PAD_A; control <= DB_PAD_C_STICK_Y; ++control)
    db_clear_port_control(port, static_cast<DBPadControl>(control));
}
}  // namespace

@implementation DBControllers
{
  // The controller holding each port, or nil.
  GCController* _holders[DB_PAD_PORTS];
  // Whether a controller has held each port since -start. See the header.
  BOOL _pluggedIn[DB_PAD_PORTS];
  // Where input handlers run. Not the main queue, which a menu animation or a
  // layout pass can hold up for a frame.
  dispatch_queue_t _inputQueue;
  BOOL _started;
}

- (instancetype)init
{
  self = [super init];
  if (self)
  {
    _inputQueue = dispatch_queue_create(
        "com.bigmah.dolbundler.controllers",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE,
                                                0));
    _pluggedIn[0] = YES;
  }
  return self;
}

- (void)dealloc
{
  [self stop];
}

- (void)start
{
  if (_started)
    return;
  _started = YES;

  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  [center addObserver:self
             selector:@selector(controllerDidConnect:)
                 name:GCControllerDidConnectNotification
               object:nil];
  [center addObserver:self
             selector:@selector(controllerDidDisconnect:)
                 name:GCControllerDidDisconnectNotification
               object:nil];

  for (GCController* controller in GCController.controllers)
    [self claim:controller];
  [self changed];
}

- (void)stop
{
  if (!_started)
    return;
  _started = NO;

  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  [center removeObserver:self name:GCControllerDidConnectNotification object:nil];
  [center removeObserver:self name:GCControllerDidDisconnectNotification object:nil];

  for (NSInteger port = 0; port < DB_PAD_PORTS; ++port)
  {
    if (_holders[port])
      [self releasePort:port];
    if (port > 0 && _pluggedIn[port])
    {
      _pluggedIn[port] = NO;
      db_set_port_plugged_in(static_cast<int>(port), 0);
    }
  }
}

- (NSString*)controllerNameForPort:(NSInteger)port
{
  if (port < 0 || port >= DB_PAD_PORTS || !_holders[port])
    return nil;
  GCController* controller = _holders[port];
  if (controller.vendorName.length)
    return controller.vendorName;
  if (controller.productCategory.length)
    return controller.productCategory;
  return @"Controller";
}

- (BOOL)isPortPluggedIn:(NSInteger)port
{
  return port >= 0 && port < DB_PAD_PORTS && _pluggedIn[port];
}

#pragma mark - Claiming

- (void)controllerDidConnect:(NSNotification*)note
{
  if ([self claim:note.object])
    [self changed];
}

- (void)controllerDidDisconnect:(NSNotification*)note
{
  for (NSInteger port = 0; port < DB_PAD_PORTS; ++port)
  {
    if (_holders[port] != note.object)
      continue;
    [self releasePort:port];
    [self changed];
    return;
  }
}

- (BOOL)claim:(GCController*)controller
{
  // A Siri Remote, or anything else without two sticks, cannot stand in for a
  // GameCube controller.
  GCExtendedGamepad* pad = controller.extendedGamepad;
  if (!pad)
    return NO;

  NSInteger port = -1;
  for (NSInteger p = 0; p < DB_PAD_PORTS; ++p)
  {
    if (_holders[p] == controller)
      return NO;
    if (port < 0 && !_holders[p])
      port = p;
  }
  // Four players already.
  if (port < 0)
    return NO;

  _holders[port] = controller;
  controller.playerIndex = static_cast<GCControllerPlayerIndex>(port);
  controller.handlerQueue = _inputQueue;
  const int target = static_cast<int>(port);
  pad.valueChangedHandler = ^(GCExtendedGamepad* gamepad, GCControllerElement* element) {
    Feed(target, gamepad);
  };
  // Whatever is already held, rather than waiting for it to change.
  dispatch_async(_inputQueue, ^{
    Feed(target, pad);
  });

  if (!_pluggedIn[port])
  {
    _pluggedIn[port] = YES;
    db_set_port_plugged_in(target, 1);
  }
  return YES;
}

- (void)releasePort:(NSInteger)port
{
  GCController* controller = _holders[port];
  _holders[port] = nil;
  controller.extendedGamepad.valueChangedHandler = nil;
  controller.playerIndex = GCControllerPlayerIndexUnset;

  // Queued behind anything the handler had already sent, so none of it can
  // land after the release and leave a button held down.
  const int target = static_cast<int>(port);
  dispatch_async(_inputQueue, ^{
    Release(target);
  });
}

- (void)changed
{
  if (_onChange)
    _onChange();
}

@end
