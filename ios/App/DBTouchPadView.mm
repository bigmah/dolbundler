// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBTouchPadView.h"

#include "dolbundler_run.h"

namespace
{
struct ButtonSpec
{
  DBPadControl control;
  const char* label;
  CGFloat x;  // fraction of width,  from the left
  CGFloat y;  // fraction of height, from the top
  CGFloat radius;
};

// Laid out in fractions so the same table works on every device and in both
// landscape orientations.
const ButtonSpec kButtons[] = {
    {DB_PAD_A, "A", 0.90f, 0.72f, 38.f},
    {DB_PAD_B, "B", 0.80f, 0.86f, 30.f},
    {DB_PAD_X, "X", 0.965f, 0.52f, 26.f},
    {DB_PAD_Y, "Y", 0.815f, 0.55f, 26.f},
    {DB_PAD_Z, "Z", 0.93f, 0.20f, 26.f},
    {DB_PAD_L_ANALOG, "L", 0.06f, 0.16f, 30.f},
    {DB_PAD_R_ANALOG, "R", 0.94f, 0.06f, 30.f},
    {DB_PAD_START, "START", 0.50f, 0.88f, 26.f},
};
constexpr size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

constexpr CGFloat kStickRadius = 64.f;
constexpr CGFloat kStickKnobRadius = 30.f;
}  // namespace

@implementation DBTouchPadView
{
  // Touch -> what it is driving. Tracked by touch identity so a finger that
  // slides off a button still releases the right control.
  NSMutableDictionary<NSValue*, NSNumber*>* _touchToButton;
  UITouch* _stickTouch;
  CGPoint _stickCentre;
  CGPoint _stickKnob;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  self = [super initWithFrame:frame];
  if (!self)
    return nil;

  self.backgroundColor = UIColor.clearColor;
  self.multipleTouchEnabled = YES;
  self.opaque = NO;
  _touchToButton = [NSMutableDictionary dictionary];
  _stickKnob = CGPointZero;
  return self;
}

- (CGPoint)buttonCentre:(const ButtonSpec&)spec
{
  return CGPointMake(spec.x * CGRectGetWidth(self.bounds), spec.y * CGRectGetHeight(self.bounds));
}

- (CGPoint)stickCentre
{
  return CGPointMake(CGRectGetWidth(self.bounds) * 0.13f, CGRectGetHeight(self.bounds) * 0.70f);
}

#pragma mark - Drawing

- (void)drawRect:(CGRect)rect
{
  CGContextRef ctx = UIGraphicsGetCurrentContext();

  // Deliberately faint: the overlay sits on top of the game and should not
  // compete with it. Touch targets stay full size regardless of how it looks.
  [[UIColor colorWithWhite:1.0 alpha:0.16] setFill];
  [[UIColor colorWithWhite:1.0 alpha:0.34] setStroke];

  const CGPoint centre = [self stickCentre];
  UIBezierPath* well = [UIBezierPath
      bezierPathWithArcCenter:centre
                       radius:kStickRadius
                   startAngle:0
                     endAngle:2 * M_PI
                    clockwise:YES];
  well.lineWidth = 2;
  [well stroke];

  const CGPoint knob =
      CGPointMake(centre.x + _stickKnob.x * kStickRadius, centre.y + _stickKnob.y * kStickRadius);
  UIBezierPath* knobPath = [UIBezierPath bezierPathWithArcCenter:knob
                                                          radius:kStickKnobRadius
                                                      startAngle:0
                                                        endAngle:2 * M_PI
                                                       clockwise:YES];
  [knobPath fill];

  NSDictionary* attrs = @{
    NSFontAttributeName : [UIFont boldSystemFontOfSize:15],
    NSForegroundColorAttributeName : [UIColor colorWithWhite:1.0 alpha:0.7]
  };

  for (size_t i = 0; i < kButtonCount; i++)
  {
    const ButtonSpec& spec = kButtons[i];
    const CGPoint c = [self buttonCentre:spec];
    UIBezierPath* path = [UIBezierPath bezierPathWithArcCenter:c
                                                        radius:spec.radius
                                                    startAngle:0
                                                      endAngle:2 * M_PI
                                                     clockwise:YES];
    path.lineWidth = 2;
    [path fill];
    [path stroke];

    NSString* label = @(spec.label);
    const CGSize size = [label sizeWithAttributes:attrs];
    [label drawAtPoint:CGPointMake(c.x - size.width / 2, c.y - size.height / 2)
        withAttributes:attrs];
  }
  (void)ctx;
}

#pragma mark - Touches

- (NSInteger)buttonIndexAt:(CGPoint)point
{
  for (size_t i = 0; i < kButtonCount; i++)
  {
    const CGPoint c = [self buttonCentre:kButtons[i]];
    const CGFloat dx = point.x - c.x;
    const CGFloat dy = point.y - c.y;
    // A generous target: fingers are imprecise and the visual radius is small.
    const CGFloat r = kButtons[i].radius + 12.f;
    if (dx * dx + dy * dy <= r * r)
      return (NSInteger)i;
  }
  return NSNotFound;
}

- (void)applyStick:(CGPoint)point
{
  const CGPoint centre = [self stickCentre];
  CGFloat dx = (point.x - centre.x) / kStickRadius;
  CGFloat dy = (point.y - centre.y) / kStickRadius;

  const CGFloat length = sqrt(dx * dx + dy * dy);
  if (length > 1.0)
  {
    dx /= length;
    dy /= length;
  }
  _stickKnob = CGPointMake(dx, dy);

  // Dolphin's stick axes are +Y up; UIKit's coordinates are +Y down.
  db_set_control(DB_PAD_MAIN_STICK_X, dx);
  db_set_control(DB_PAD_MAIN_STICK_Y, -dy);
  [self setNeedsDisplay];
}

- (void)releaseStick
{
  _stickKnob = CGPointZero;
  db_set_control(DB_PAD_MAIN_STICK_X, 0.0);
  db_set_control(DB_PAD_MAIN_STICK_Y, 0.0);
  [self setNeedsDisplay];
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  for (UITouch* touch in touches)
  {
    const CGPoint point = [touch locationInView:self];
    const NSInteger index = [self buttonIndexAt:point];

    if (index != NSNotFound)
    {
      _touchToButton[[NSValue valueWithNonretainedObject:touch]] = @(index);
      db_set_control(kButtons[index].control, 1.0);
      continue;
    }

    // Anything on the left half that is not a button drives the stick, so the
    // thumb does not have to find an exact circle mid-game.
    if (!_stickTouch && point.x < CGRectGetWidth(self.bounds) * 0.5f)
    {
      _stickTouch = touch;
      [self applyStick:point];
    }
  }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  for (UITouch* touch in touches)
  {
    if (touch == _stickTouch)
      [self applyStick:[touch locationInView:self]];
  }
}

- (void)endTouches:(NSSet<UITouch*>*)touches
{
  for (UITouch* touch in touches)
  {
    NSValue* key = [NSValue valueWithNonretainedObject:touch];
    NSNumber* index = _touchToButton[key];
    if (index)
    {
      db_clear_control(kButtons[index.unsignedIntegerValue].control);
      [_touchToButton removeObjectForKey:key];
    }
    if (touch == _stickTouch)
    {
      _stickTouch = nil;
      [self releaseStick];
    }
  }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  [self endTouches:touches];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  [self endTouches:touches];
}

@end
