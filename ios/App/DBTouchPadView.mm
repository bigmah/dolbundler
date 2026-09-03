// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBTouchPadView.h"

#import "DBSettings.h"
#import "DBTheme.h"

#include "dolbundler_run.h"

#include <cmath>

namespace
{
// Fingers are imprecise and the drawn control is deliberately small, so every
// hit test runs against a shape inflated by this much. It is the single
// biggest thing separating an overlay that feels responsive from one that
// feels broken, and it costs nothing on screen.
constexpr CGFloat kTouchSlop = 14.0;

// No control is bound to a value this small; below it a stick reads as centred
// and the D-pad as untouched. Without it a thumb resting on the pad drifts.
constexpr CGFloat kStickDeadzone = 0.14;
constexpr CGFloat kCrossDeadzone = 0.30;

constexpr NSInteger kNoCompanion = -1;

// How far outside a control its editing ring sits.
constexpr CGFloat kEditRingInset = 7.0;

// The names a dragged position is stored under. Strings rather than the
// control enum because the D-pad and the sticks are not single controls, and
// because a stored layout has to survive the enum being reordered.
NSString* const kNameL = @"L";
NSString* const kNameR = @"R";
NSString* const kNameZ = @"Z";
NSString* const kNameY = @"Y";
NSString* const kNameX = @"X";
NSString* const kNameB = @"B";
NSString* const kNameA = @"A";
NSString* const kNameStart = @"START";
NSString* const kNameCross = @"DPAD";
NSString* const kNameMainStick = @"STICK";
NSString* const kNameCStick = @"CSTICK";

// Direction bits for the D-pad, in the order its arm layers are stored.
enum : NSUInteger
{
  kDirUp = 1u << 0,
  kDirDown = 1u << 1,
  kDirLeft = 1u << 2,
  kDirRight = 1u << 3,
};

// The overlay sits on top of arbitrary game output, so the outline has to be
// the half of each control that survives when the fill does not.
//
// It is light rather than dark. A dark outline disappears into the dark scenes
// games spend most of their time in, and the translucent fill is too faint to
// carry a control on its own there; against a bright scene the fill is what
// separates the shape, so the pale line can afford to lose that fight.
UIColor* Outline(CGFloat opacity)
{
  return [UIColor colorWithWhite:1.0 alpha:0.55 * opacity];
}

// In the layer's own coordinates, where the origin is the top-left of bounds
// and not the centre. Drawing this centred on (0, 0) instead puts every shape
// half its own size up and to the left of the touch target it belongs to, and
// makes the press animation pivot about a corner.
// A rounded plus, in the layer's own coordinates, centred on `centre`.
//
// Four rounded rectangles and a square in the middle draw a shape that reads
// as five tiles: the arms' inner corners and the hub's square ones meet at
// visible seams. A D-pad is one piece of plastic, so this is one path -- twelve
// corners, eight of them convex and four concave, each rounded by the same
// arc. Every arc aims at the midpoint of the following edge, which is what
// guarantees it fits however short the arms are.
CGPathRef CreateCrossPath(CGPoint centre, CGFloat arm, CGFloat thickness)
{
  const CGFloat t = thickness / 2;
  const CGFloat a = arm + thickness / 2;
  const CGFloat radius = MIN(thickness * 0.3, arm * 0.4);

  const CGPoint corners[12] = {
      {centre.x - t, centre.y - a}, {centre.x + t, centre.y - a},
      {centre.x + t, centre.y - t}, {centre.x + a, centre.y - t},
      {centre.x + a, centre.y + t}, {centre.x + t, centre.y + t},
      {centre.x + t, centre.y + a}, {centre.x - t, centre.y + a},
      {centre.x - t, centre.y + t}, {centre.x - a, centre.y + t},
      {centre.x - a, centre.y - t}, {centre.x - t, centre.y - t},
  };

  CGMutablePathRef path = CGPathCreateMutable();
  CGPathMoveToPoint(path, nullptr, (corners[0].x + corners[1].x) / 2,
                    (corners[0].y + corners[1].y) / 2);
  for (int i = 1; i <= 12; i++)
  {
    const CGPoint corner = corners[i % 12];
    const CGPoint next = corners[(i + 1) % 12];
    CGPathAddArcToPoint(path, nullptr, corner.x, corner.y, (corner.x + next.x) / 2,
                        (corner.y + next.y) / 2, radius);
  }
  CGPathCloseSubpath(path);
  return path;
}

CGPathRef CreateCapsulePath(CGSize size)
{
  const CGRect rect = CGRectMake(0, 0, size.width, size.height);
  const CGFloat radius = MIN(size.width, size.height) / 2;
  return CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
}

// Distance from a point to a capsule's spine, which is what containment for
// both a circle and a rounded bar reduces to: clamp onto the segment, measure.
BOOL CapsuleContains(CGPoint local, CGSize size, CGFloat slop)
{
  const CGFloat width = size.width + slop * 2;
  const CGFloat height = size.height + slop * 2;
  const CGFloat radius = MIN(width, height) / 2;
  const CGFloat spine_x = MAX(0.0, width / 2 - radius);
  const CGFloat spine_y = MAX(0.0, height / 2 - radius);

  const CGFloat cx = MIN(spine_x, MAX(-spine_x, local.x));
  const CGFloat cy = MIN(spine_y, MAX(-spine_y, local.y));
  const CGFloat dx = local.x - cx;
  const CGFloat dy = local.y - cy;
  return dx * dx + dy * dy <= radius * radius;
}
}  // namespace

#pragma mark - Controls

typedef NS_ENUM(NSUInteger, DBKeyShape)
{
  DBKeyShapeCircle,
  DBKeyShapeCapsule,
};

// One tappable button: A, B, X, Y, Z, L, R, Start.
@interface DBPadKey : NSObject
@property(nonatomic, assign) DBPadControl control;
// Triggers report two controls. A game may read either the analog depth or the
// digital click at the bottom of the throw, and which one it uses is not
// knowable from here, so a full press sends both.
@property(nonatomic, assign) NSInteger companion;
@property(nonatomic, copy) NSString* label;
@property(nonatomic, strong) UIColor* fill;
@property(nonatomic, strong) UIColor* ink;
@property(nonatomic, assign) DBKeyShape shape;
@property(nonatomic, assign) CGPoint centre;
@property(nonatomic, assign) CGSize size;
@property(nonatomic, assign) CGFloat rotation;
@property(nonatomic, assign) CGFloat labelSize;
@property(nonatomic, strong) CAShapeLayer* shapeLayer;
@property(nonatomic, strong) CATextLayer* textLayer;
@property(nonatomic, assign) BOOL pressed;
// For the layout editor: what the position is stored under, and the ring
// drawn round the control while editing.
@property(nonatomic, copy) NSString* name;
@property(nonatomic, strong) CAShapeLayer* editRing;
@end

@implementation DBPadKey
@end

// An analog stick. The main stick floats -- a touch anywhere in its half of
// the screen picks it up and re-centres it under the thumb -- because finding
// an exact circle without looking is the thing people get wrong. The C stick
// stays put: the right side is crowded with buttons, and a floating stick
// there would fire on every stray tap.
@interface DBPadStick : NSObject
@property(nonatomic, assign) DBPadControl axisX;
@property(nonatomic, assign) DBPadControl axisY;
@property(nonatomic, assign) CGPoint home;
@property(nonatomic, assign) CGPoint centre;
@property(nonatomic, assign) CGFloat radius;
@property(nonatomic, assign) CGFloat knobRadius;
@property(nonatomic, assign) BOOL floats;
@property(nonatomic, strong) UIColor* tint;
@property(nonatomic, copy) NSString* label;
@property(nonatomic, strong) CAShapeLayer* wellLayer;
@property(nonatomic, strong) CAShapeLayer* knobLayer;
@property(nonatomic, strong) CATextLayer* textLayer;
@property(nonatomic, assign) CGPoint offset;
@property(nonatomic, assign) BOOL engaged;
@property(nonatomic, copy) NSString* name;
@property(nonatomic, strong) CAShapeLayer* editRing;
@end

@implementation DBPadStick
@end

// The D-pad, as one control rather than four buttons: a finger between two
// arms has to be able to hold a diagonal, and four independent buttons each
// claiming their own touch cannot express that.
@interface DBPadCross : NSObject
@property(nonatomic, assign) CGPoint centre;
@property(nonatomic, assign) CGFloat arm;
@property(nonatomic, assign) CGFloat thickness;
// The pad itself, then one highlight per direction sitting inside it. The
// highlights are inset rather than clipped, so a lit arm never pokes out past
// the body's rounded edge and no mask layer is needed to stop it.
@property(nonatomic, strong) CAShapeLayer* body;
@property(nonatomic, strong) NSArray<CAShapeLayer*>* arms;
@property(nonatomic, assign) NSUInteger mask;
@property(nonatomic, copy) NSString* name;
@property(nonatomic, strong) CAShapeLayer* editRing;
@end

@implementation DBPadCross
@end

#pragma mark - View

@implementation DBTouchPadView
{
  NSArray<DBPadKey*>* _keys;
  DBPadStick* _mainStick;
  DBPadStick* _cStick;
  DBPadCross* _cross;

  // Touch -> the control it is driving. Keyed by touch identity so a finger
  // that slides off still releases the right thing, and so two thumbs on two
  // sticks never get confused for one another.
  NSMutableDictionary<NSValue*, id>* _bindings;

  CGFloat _opacity;
  UIImpactFeedbackGenerator* _haptics;
  BOOL _hapticsEnabled;
  CGRect _laidOutFor;

  // The rectangle every position is a fraction of, and the size factor the
  // last layout used. Both are needed again when a drag ends and its result
  // has to be turned back into something storable.
  CGRect _safeRect;
  CGFloat _scale;

  // Layout editing. One control moves at a time; a second finger is ignored
  // rather than allowed to start a second drag under the first.
  BOOL _editing;
  id _dragTarget;
  UITouch* _dragTouch;
  CGPoint _dragAnchor;  // where the control was when the drag began
  CGPoint _dragOrigin;  // where the finger was
}

- (instancetype)initWithFrame:(CGRect)frame
{
  self = [super initWithFrame:frame];
  if (!self)
    return nil;

  self.backgroundColor = UIColor.clearColor;
  self.opaque = NO;
  self.multipleTouchEnabled = YES;
  self.exclusiveTouch = NO;
  _bindings = [NSMutableDictionary dictionary];
  _laidOutFor = CGRectNull;

  [self buildControls];
  [self refreshFromSettings];
  return self;
}

#pragma mark - Construction

// A dashed ring, hidden until the layout editor needs it. One per control so
// that "everything on this screen can be dragged" is visible at a glance
// rather than discovered by trying.
- (CAShapeLayer*)makeEditRing
{
  CAShapeLayer* ring = [CAShapeLayer layer];
  ring.fillColor = UIColor.clearColor.CGColor;
  ring.strokeColor = [UIColor colorWithWhite:1.0 alpha:0.9].CGColor;
  ring.lineWidth = 1.5;
  ring.lineDashPattern = @[ @5, @4 ];
  ring.hidden = YES;
  [self.layer addSublayer:ring];
  return ring;
}

- (DBPadKey*)keyWithControl:(DBPadControl)control
                  companion:(NSInteger)companion
                      label:(NSString*)label
                       name:(NSString*)name
                       fill:(UIColor*)fill
                        ink:(UIColor*)ink
                      shape:(DBKeyShape)shape
{
  DBPadKey* key = [[DBPadKey alloc] init];
  key.control = control;
  key.companion = companion;
  key.label = label;
  key.name = name;
  key.fill = fill;
  key.ink = ink;
  key.shape = shape;

  key.shapeLayer = [CAShapeLayer layer];
  key.shapeLayer.lineWidth = 2.0;
  key.shapeLayer.lineJoin = kCALineJoinRound;
  [self.layer addSublayer:key.shapeLayer];
  key.editRing = [self makeEditRing];

  key.textLayer = [CATextLayer layer];
  key.textLayer.contentsScale = UIScreen.mainScreen.scale;
  key.textLayer.alignmentMode = kCAAlignmentCenter;
  [self.layer addSublayer:key.textLayer];
  return key;
}

- (DBPadStick*)stickWithX:(DBPadControl)x
                        y:(DBPadControl)y
                     tint:(UIColor*)tint
                    label:(NSString*)label
                     name:(NSString*)name
                   floats:(BOOL)floats
{
  DBPadStick* stick = [[DBPadStick alloc] init];
  stick.axisX = x;
  stick.axisY = y;
  stick.tint = tint;
  stick.label = label;
  stick.name = name;
  stick.floats = floats;

  stick.wellLayer = [CAShapeLayer layer];
  stick.wellLayer.lineWidth = 2.0;
  [self.layer addSublayer:stick.wellLayer];
  stick.editRing = [self makeEditRing];

  // Well, then knob, then label: the C stick's letter rides on the knob the
  // way the hardware's does, so it has to be added last to stay on top of it.
  stick.knobLayer = [CAShapeLayer layer];
  stick.knobLayer.lineWidth = 2.0;
  [self.layer addSublayer:stick.knobLayer];

  stick.textLayer = [CATextLayer layer];
  stick.textLayer.contentsScale = UIScreen.mainScreen.scale;
  stick.textLayer.alignmentMode = kCAAlignmentCenter;
  [self.layer addSublayer:stick.textLayer];
  return stick;
}

- (void)buildControls
{
  // Order matters only for painting: shoulders first so the face cluster sits
  // over them if a small screen ever forces an overlap.
  _keys = @[
    [self keyWithControl:DB_PAD_L_ANALOG
               companion:DB_PAD_L_DIGITAL
                   label:@"L"
                    name:kNameL
                    fill:DBTheme.shoulder
                     ink:[UIColor colorWithWhite:0.1 alpha:1.0]
                   shape:DBKeyShapeCapsule],
    [self keyWithControl:DB_PAD_R_ANALOG
               companion:DB_PAD_R_DIGITAL
                   label:@"R"
                    name:kNameR
                    fill:DBTheme.shoulder
                     ink:[UIColor colorWithWhite:0.1 alpha:1.0]
                   shape:DBKeyShapeCapsule],
    [self keyWithControl:DB_PAD_Z
               companion:kNoCompanion
                   label:@"Z"
                    name:kNameZ
                    fill:DBTheme.buttonZ
                     ink:UIColor.whiteColor
                   shape:DBKeyShapeCapsule],
    [self keyWithControl:DB_PAD_Y
               companion:kNoCompanion
                   label:@"Y"
                    name:kNameY
                    fill:DBTheme.buttonXY
                     ink:[UIColor colorWithWhite:0.1 alpha:1.0]
                   shape:DBKeyShapeCapsule],
    [self keyWithControl:DB_PAD_X
               companion:kNoCompanion
                   label:@"X"
                    name:kNameX
                    fill:DBTheme.buttonXY
                     ink:[UIColor colorWithWhite:0.1 alpha:1.0]
                   shape:DBKeyShapeCapsule],
    [self keyWithControl:DB_PAD_B
               companion:kNoCompanion
                   label:@"B"
                    name:kNameB
                    fill:DBTheme.buttonB
                     ink:UIColor.whiteColor
                   shape:DBKeyShapeCircle],
    [self keyWithControl:DB_PAD_A
               companion:kNoCompanion
                   label:@"A"
                    name:kNameA
                    fill:DBTheme.buttonA
                     ink:UIColor.whiteColor
                   shape:DBKeyShapeCircle],
    [self keyWithControl:DB_PAD_START
               companion:kNoCompanion
                   label:@"START"
                    name:kNameStart
                    fill:DBTheme.shoulder
                     ink:[UIColor colorWithWhite:0.1 alpha:1.0]
                   shape:DBKeyShapeCapsule],
  ];

  _cross = [[DBPadCross alloc] init];
  _cross.name = kNameCross;
  _cross.body = [CAShapeLayer layer];
  _cross.body.lineWidth = 2.0;
  [self.layer addSublayer:_cross.body];
  _cross.editRing = [self makeEditRing];

  NSMutableArray<CAShapeLayer*>* arms = [NSMutableArray array];
  for (int i = 0; i < 4; i++)
  {
    CAShapeLayer* arm = [CAShapeLayer layer];
    arm.strokeColor = UIColor.clearColor.CGColor;
    [self.layer addSublayer:arm];
    [arms addObject:arm];
  }
  _cross.arms = arms;

  _mainStick = [self stickWithX:DB_PAD_MAIN_STICK_X
                              y:DB_PAD_MAIN_STICK_Y
                           tint:DBTheme.mainStick
                          label:nil
                           name:kNameMainStick
                         floats:YES];
  _cStick = [self stickWithX:DB_PAD_C_STICK_X
                           y:DB_PAD_C_STICK_Y
                        tint:DBTheme.cStick
                       label:@"C"
                        name:kNameCStick
                      floats:NO];
}

#pragma mark - Settings

- (void)refreshFromSettings
{
  DBSettings* settings = DBSettings.shared;
  _opacity = settings.padOpacity;
  _hapticsEnabled = settings.hapticsEnabled;
  if (_hapticsEnabled && !_haptics)
  {
    _haptics = [[UIImpactFeedbackGenerator alloc]
        initWithStyle:UIImpactFeedbackStyleLight];
  }
  [self applyStyle];
}

#pragma mark - Editing

- (void)setEditingLayout:(BOOL)editing
{
  if (_editing == editing)
    return;
  // Whatever a thumb was holding is let go before the mode flips: a button
  // that stayed pressed through the editor would be held for as long as the
  // editor was open, and the game is paused behind it with no way to see.
  [self releaseAll];
  _editing = editing;
  _dragTarget = nil;
  _dragTouch = nil;

  // The game is paused behind the editor, so a scrim over its last frame
  // costs nothing and is what makes the rings readable on any scene.
  self.backgroundColor =
      editing ? [UIColor colorWithWhite:0.0 alpha:0.45] : UIColor.clearColor;
  [self applyStyle];
}

- (BOOL)isEditingLayout
{
  return _editing;
}

- (void)reloadLayout
{
  _laidOutFor = CGRectNull;
  [self setNeedsLayout];
  [self layoutIfNeeded];
}

- (void)resetLayout
{
  [DBSettings.shared resetPadLayout];
  [self reloadLayout];
}

// Turn a dropped centre into what DBSettings keeps: its position as a
// fraction of the safe rectangle the layout was made in.
- (void)storeCentre:(CGPoint)centre forName:(NSString*)name
{
  if (CGRectIsEmpty(_safeRect))
    return;
  const CGPoint fraction = CGPointMake((centre.x - CGRectGetMinX(_safeRect)) / CGRectGetWidth(_safeRect),
                                       (centre.y - CGRectGetMinY(_safeRect)) / CGRectGetHeight(_safeRect));
  [DBSettings.shared setPadPosition:fraction forControl:name];
}

// The default position, unless one has been stored for this control.
- (CGPoint)placedCentre:(CGPoint)fallback forName:(NSString*)name safe:(CGRect)safe
{
  CGPoint fraction;
  if (![DBSettings.shared padPosition:&fraction forControl:name])
    return fallback;
  return CGPointMake(CGRectGetMinX(safe) + fraction.x * CGRectGetWidth(safe),
                     CGRectGetMinY(safe) + fraction.y * CGRectGetHeight(safe));
}

// Keep a control of the given half-extent inside the safe rectangle.
- (CGPoint)clampCentre:(CGPoint)centre halfWidth:(CGFloat)hw halfHeight:(CGFloat)hh
{
  const CGRect safe = _safeRect;
  const CGFloat minX = CGRectGetMinX(safe) + hw, maxX = CGRectGetMaxX(safe) - hw;
  const CGFloat minY = CGRectGetMinY(safe) + hh, maxY = CGRectGetMaxY(safe) - hh;
  return CGPointMake(minX <= maxX ? MIN(maxX, MAX(minX, centre.x)) : CGRectGetMidX(safe),
                     minY <= maxY ? MIN(maxY, MAX(minY, centre.y)) : CGRectGetMidY(safe));
}

#pragma mark - Layout

- (void)safeAreaInsetsDidChange
{
  [super safeAreaInsetsDidChange];
  _laidOutFor = CGRectNull;
  [self setNeedsLayout];
}

- (DBPadKey*)keyFor:(DBPadControl)control
{
  for (DBPadKey* key in _keys)
    if (key.control == control)
      return key;
  return nil;
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  if (CGRectEqualToRect(_laidOutFor, self.bounds))
    return;

  // Everything is anchored to the safe rect rather than to the bounds. In
  // landscape the sensor housing eats one whole side, and a fixed fraction of
  // the raw width puts the shoulder button underneath it on exactly one of the
  // two orientations -- which is the kind of bug that only shows up after the
  // phone is turned around.
  const CGRect safe =
      CGRectInset(UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets), 12, 12);
  if (CGRectIsEmpty(safe))
    return;
  _laidOutFor = self.bounds;
  _safeRect = safe;

  const CGFloat left = CGRectGetMinX(safe);
  const CGFloat right = CGRectGetMaxX(safe);
  const CGFloat top = CGRectGetMinY(safe);
  const CGFloat bottom = CGRectGetMaxY(safe);
  const CGFloat height = CGRectGetHeight(safe);

  // One scale factor for the whole pad, taken from the short side. A phone in
  // landscape is about 352 points tall inside the safe area; an iPad is more
  // than twice that, and a pad drawn at phone size on it would be a row of
  // dots in the corner.
  // The size multiplier from the settings rides on top of that: a device's
  // factor says how big the screen is, the multiplier how big the thumb is.
  const CGFloat s = MIN(1.6, MAX(0.80, height / 352.0)) * DBSettings.shared.padScale;
  _scale = s;

  // Shoulders across the top, where the index fingers already are.
  DBPadKey* l = [self keyFor:DB_PAD_L_ANALOG];
  l.centre = [self placedCentre:CGPointMake(left + 60 * s, top + 22 * s) forName:l.name safe:safe];
  l.size = CGSizeMake(116 * s, 40 * s);
  l.labelSize = 15 * s;

  DBPadKey* r = [self keyFor:DB_PAD_R_ANALOG];
  r.centre = [self placedCentre:CGPointMake(right - 60 * s, top + 22 * s) forName:r.name safe:safe];
  r.size = CGSizeMake(116 * s, 40 * s);
  r.labelSize = 15 * s;

  DBPadKey* z = [self keyFor:DB_PAD_Z];
  z.centre = [self placedCentre:CGPointMake(right - 60 * s, top + 62 * s) forName:z.name safe:safe];
  z.size = CGSizeMake(84 * s, 30 * s);
  z.labelSize = 13 * s;

  // The face cluster, in the hardware's own arrangement: A large in the
  // middle, B down and left, Y a wide bean above, X a tall one to the right.
  // Anyone who has held the controller can find these without reading them,
  // which is the entire reason for not laying them out in a neat row.
  // The cluster is placed as a unit -- B, X and Y hang off A -- and then each
  // button is free to have been dragged away from it on its own.
  DBPadKey* a = [self keyFor:DB_PAD_A];
  const CGPoint centreA = CGPointMake(right - 90 * s, bottom - 148 * s);
  a.centre = [self placedCentre:centreA forName:a.name safe:safe];
  a.size = CGSizeMake(86 * s, 86 * s);
  a.labelSize = 24 * s;

  DBPadKey* b = [self keyFor:DB_PAD_B];
  b.centre = [self placedCentre:CGPointMake(centreA.x - 60 * s, centreA.y + 40 * s)
                        forName:b.name
                           safe:safe];
  b.size = CGSizeMake(56 * s, 56 * s);
  b.labelSize = 17 * s;

  DBPadKey* y = [self keyFor:DB_PAD_Y];
  y.centre = [self placedCentre:CGPointMake(centreA.x - 30 * s, centreA.y - 66 * s)
                        forName:y.name
                           safe:safe];
  y.size = CGSizeMake(70 * s, 38 * s);
  y.rotation = -0.42;
  y.labelSize = 15 * s;

  DBPadKey* x = [self keyFor:DB_PAD_X];
  x.centre = [self placedCentre:CGPointMake(centreA.x + 58 * s, centreA.y - 26 * s)
                        forName:x.name
                           safe:safe];
  x.size = CGSizeMake(38 * s, 70 * s);
  x.rotation = -0.42;
  x.labelSize = 15 * s;

  DBPadKey* start = [self keyFor:DB_PAD_START];
  start.centre = [self placedCentre:CGPointMake(CGRectGetMidX(safe), bottom - 22 * s)
                            forName:start.name
                               safe:safe];
  start.size = CGSizeMake(74 * s, 28 * s);
  start.labelSize = 11 * s;

  _cross.centre = [self placedCentre:CGPointMake(left + 66 * s, top + height * 0.44)
                             forName:_cross.name
                                safe:safe];
  _cross.arm = 34 * s;
  _cross.thickness = 34 * s;

  _mainStick.radius = 62 * s;
  _mainStick.knobRadius = 30 * s;
  _mainStick.home = [self placedCentre:CGPointMake(left + 84 * s, bottom - 84 * s)
                               forName:_mainStick.name
                                  safe:safe];
  if (!_mainStick.engaged)
    _mainStick.centre = _mainStick.home;

  _cStick.radius = 44 * s;
  _cStick.knobRadius = 22 * s;
  _cStick.home = [self placedCentre:CGPointMake(right - 76 * s, bottom - 50 * s)
                            forName:_cStick.name
                               safe:safe];
  _cStick.centre = _cStick.home;
  _cStick.label = @"C";

  [self applyGeometry];
  [self applyStyle];
}

- (void)applyGeometry
{
  [CATransaction begin];
  [CATransaction setDisableActions:YES];

  for (DBPadKey* key in _keys)
  {
    CGPathRef path = CreateCapsulePath(key.size);
    key.shapeLayer.path = path;
    CGPathRelease(path);
    key.shapeLayer.bounds = CGRectMake(0, 0, key.size.width, key.size.height);
    key.shapeLayer.position = key.centre;
    key.textLayer.position = key.centre;
    [self applyKeyTransform:key];
    [self applyKeyRing:key];
  }

  [self applyCrossGeometry];
  [self applyStickGeometry:_mainStick];
  [self applyStickGeometry:_cStick];

  [CATransaction commit];
}

- (void)applyKeyRing:(DBPadKey*)key
{
  const CGSize size = CGSizeMake(key.size.width + kEditRingInset * 2,
                                 key.size.height + kEditRingInset * 2);
  CGPathRef path = CreateCapsulePath(size);
  key.editRing.path = path;
  CGPathRelease(path);
  key.editRing.bounds = CGRectMake(0, 0, size.width, size.height);
  key.editRing.position = key.centre;
  key.editRing.affineTransform = CGAffineTransformMakeRotation(key.rotation);
}

- (void)applyKeyTransform:(DBPadKey*)key
{
  // A press has to be visible on a screen a thumb is covering, so the shape
  // shrinks as well as brightening -- the movement is what shows at the edges
  // of the finger.
  const CGFloat scale = key.pressed ? 0.93 : 1.0;

  key.shapeLayer.affineTransform = CGAffineTransformScale(
      CGAffineTransformMakeRotation(key.rotation), scale, scale);
  // The letter does not follow the tilt. X and Y are canted beans on the
  // hardware but their letters are printed upright, and a rotated glyph is
  // measurably slower to read at a glance.
  key.textLayer.affineTransform = CGAffineTransformMakeScale(scale, scale);
}

- (void)applyCrossGeometry
{
  const CGFloat arm = _cross.arm;
  const CGFloat thickness = _cross.thickness;
  const CGPoint centre = _cross.centre;

  CGPathRef body = CreateCrossPath(centre, arm, thickness);
  _cross.body.path = body;
  CGPathRelease(body);

  // A diagonal lights two arms at once, and that read-out is most of what
  // tells someone their thumb is where they think it is -- so the highlights
  // stay four separate layers even though the body is now one.
  const CGFloat inset = 5;
  const CGFloat width = thickness - inset * 2;
  const CGFloat length = arm + thickness / 2 - inset * 2;
  const CGFloat corner = width * 0.34;

  const CGRect frames[4] = {
      CGRectMake(centre.x - width / 2, centre.y - length - inset / 2, width, length),
      CGRectMake(centre.x - width / 2, centre.y + inset / 2, width, length),
      CGRectMake(centre.x - length - inset / 2, centre.y - width / 2, length, width),
      CGRectMake(centre.x + inset / 2, centre.y - width / 2, length, width),
  };

  for (NSUInteger i = 0; i < 4; i++)
  {
    _cross.arms[i].path =
        [UIBezierPath bezierPathWithRoundedRect:frames[i] cornerRadius:corner].CGPath;
  }

  const CGFloat reach = arm + thickness / 2 + kEditRingInset;
  _cross.editRing.path =
      [UIBezierPath bezierPathWithRoundedRect:CGRectMake(centre.x - reach, centre.y - reach,
                                                         reach * 2, reach * 2)
                                 cornerRadius:thickness * 0.4]
          .CGPath;
}

- (void)applyStickGeometry:(DBPadStick*)stick
{
  const CGFloat diameter = stick.radius * 2;
  stick.wellLayer.bounds = CGRectMake(0, 0, diameter, diameter);
  stick.wellLayer.path =
      [UIBezierPath bezierPathWithOvalInRect:CGRectMake(0, 0, diameter, diameter)].CGPath;
  stick.wellLayer.position = stick.centre;

  const CGFloat knobDiameter = stick.knobRadius * 2;
  stick.knobLayer.bounds = CGRectMake(0, 0, knobDiameter, knobDiameter);
  stick.knobLayer.path =
      [UIBezierPath bezierPathWithOvalInRect:CGRectMake(0, 0, knobDiameter, knobDiameter)].CGPath;
  stick.knobLayer.position =
      CGPointMake(stick.centre.x + stick.offset.x * stick.radius,
                  stick.centre.y + stick.offset.y * stick.radius);

  stick.textLayer.position = stick.knobLayer.position;

  // The ring marks the home, not wherever a floating stick was last picked
  // up: the home is the thing being edited.
  const CGFloat ringDiameter = diameter + kEditRingInset * 2;
  stick.editRing.bounds = CGRectMake(0, 0, ringDiameter, ringDiameter);
  stick.editRing.path =
      [UIBezierPath bezierPathWithOvalInRect:CGRectMake(0, 0, ringDiameter, ringDiameter)].CGPath;
  stick.editRing.position = stick.home;
}

#pragma mark - Style

- (void)applyStyle
{
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  for (DBPadKey* key in _keys)
  {
    [self styleKey:key];
    [self styleRing:key.editRing dragging:key == _dragTarget];
  }
  [self styleCross];
  [self styleRing:_cross.editRing dragging:_cross == _dragTarget];
  [self styleStick:_mainStick];
  [self styleRing:_mainStick.editRing dragging:_mainStick == _dragTarget];
  [self styleStick:_cStick];
  [self styleRing:_cStick.editRing dragging:_cStick == _dragTarget];
  [CATransaction commit];
}

// The opacity every layer is drawn at. The editor ignores the setting: a pad
// being arranged has to be seen, whatever it has been faded to for play.
- (CGFloat)drawnOpacity
{
  return _editing ? 1.0 : _opacity;
}

- (void)styleRing:(CAShapeLayer*)ring dragging:(BOOL)dragging
{
  ring.hidden = !_editing;
  ring.strokeColor = dragging ? DBTheme.accent.CGColor
                              : [UIColor colorWithWhite:1.0 alpha:0.75].CGColor;
  ring.lineWidth = dragging ? 2.5 : 1.5;
  ring.lineDashPattern = dragging ? nil : @[ @5, @4 ];
}

- (void)styleKey:(DBPadKey*)key
{
  const CGFloat opacity = [self drawnOpacity];
  key.shapeLayer.fillColor =
      [key.fill colorWithAlphaComponent:(key.pressed ? 0.95 : 0.58) * opacity].CGColor;
  key.shapeLayer.strokeColor = Outline(opacity).CGColor;

  if (key.labelSize <= 0)
  {
    key.textLayer.string = nil;
    return;
  }

  UIFont* font = [UIFont systemFontOfSize:key.labelSize weight:UIFontWeightHeavy];
  NSAttributedString* label = [[NSAttributedString alloc]
      initWithString:key.label
          attributes:@{
            NSFontAttributeName : font,
            NSForegroundColorAttributeName :
                [key.ink colorWithAlphaComponent:(key.pressed ? 1.0 : 0.88) * opacity],
            NSKernAttributeName : @(key.labelSize * 0.06),
          }];
  key.textLayer.string = label;
  const CGSize size = label.size;
  key.textLayer.bounds = CGRectMake(0, 0, ceil(size.width) + 2, ceil(size.height));
}

- (void)styleCross
{
  const CGFloat opacity = [self drawnOpacity];
  _cross.body.fillColor =
      [DBTheme.shoulder colorWithAlphaComponent:0.54 * opacity].CGColor;
  _cross.body.strokeColor = Outline(opacity).CGColor;

  const NSUInteger bits[4] = {kDirUp, kDirDown, kDirLeft, kDirRight};
  for (NSUInteger i = 0; i < 4; i++)
  {
    const BOOL held = (_cross.mask & bits[i]) != 0;
    _cross.arms[i].fillColor =
        held ? [UIColor colorWithWhite:1.0 alpha:0.85 * opacity].CGColor
             : UIColor.clearColor.CGColor;
  }
}

- (void)styleStick:(DBPadStick*)stick
{
  const CGFloat opacity = [self drawnOpacity];
  stick.wellLayer.fillColor = [UIColor colorWithWhite:1.0 alpha:0.14 * opacity].CGColor;
  stick.wellLayer.strokeColor = Outline(opacity).CGColor;
  stick.knobLayer.fillColor =
      [stick.tint colorWithAlphaComponent:(stick.engaged ? 0.92 : 0.55) * opacity].CGColor;
  stick.knobLayer.strokeColor = Outline(opacity).CGColor;

  if (!stick.label.length || stick.knobRadius <= 0)
  {
    stick.textLayer.string = nil;
    return;
  }

  UIFont* font = [UIFont systemFontOfSize:stick.knobRadius * 0.85
                                   weight:UIFontWeightHeavy];
  NSAttributedString* label = [[NSAttributedString alloc]
      initWithString:stick.label
          attributes:@{
            NSFontAttributeName : font,
            NSForegroundColorAttributeName :
                [UIColor colorWithWhite:0.1 alpha:0.85 * opacity],
          }];
  stick.textLayer.string = label;
  const CGSize size = label.size;
  stick.textLayer.bounds = CGRectMake(0, 0, ceil(size.width) + 2, ceil(size.height));
}

#pragma mark - Hit testing

- (CGPoint)localPoint:(CGPoint)point forKey:(DBPadKey*)key
{
  const CGFloat dx = point.x - key.centre.x;
  const CGFloat dy = point.y - key.centre.y;
  if (key.rotation == 0)
    return CGPointMake(dx, dy);

  const CGFloat c = cos(-key.rotation);
  const CGFloat s = sin(-key.rotation);
  return CGPointMake(dx * c - dy * s, dx * s + dy * c);
}

// Nearest centre wins where inflated targets overlap, which they do all round
// the face cluster. Without that tie-break the answer would depend on the
// order the buttons happen to be declared in.
- (DBPadKey*)keyAt:(CGPoint)point
{
  DBPadKey* best = nil;
  CGFloat bestDistance = CGFLOAT_MAX;
  for (DBPadKey* key in _keys)
  {
    if (!CapsuleContains([self localPoint:point forKey:key], key.size, kTouchSlop))
      continue;
    const CGFloat dx = point.x - key.centre.x;
    const CGFloat dy = point.y - key.centre.y;
    const CGFloat distance = dx * dx + dy * dy;
    if (distance < bestDistance)
    {
      bestDistance = distance;
      best = key;
    }
  }
  return best;
}

- (BOOL)crossContains:(CGPoint)point
{
  const CGFloat reach = _cross.arm + _cross.thickness / 2 + kTouchSlop;
  return fabs(point.x - _cross.centre.x) <= reach && fabs(point.y - _cross.centre.y) <= reach;
}

- (BOOL)stick:(DBPadStick*)stick contains:(CGPoint)point
{
  const CGFloat dx = point.x - stick.centre.x;
  const CGFloat dy = point.y - stick.centre.y;
  const CGFloat reach = stick.radius + kTouchSlop;
  return dx * dx + dy * dy <= reach * reach;
}

// Where the main stick will accept a touch that hit nothing else.
//
// Bounded below the D-pad rather than at some fraction of the height. The
// stick floats to wherever the thumb lands, so a touch taken above that line
// would drag the whole well up over the D-pad -- and a finger that had just
// missed the L button would move the stick into the top corner. A layout that
// has put the stick above the D-pad still gets its own well, which is what the
// second term is for.
- (BOOL)isInMainStickZone:(CGPoint)point
{
  const CGFloat crossBottom = _cross.centre.y + _cross.arm + _cross.thickness / 2;
  const CGFloat zoneTop = MIN(crossBottom, _mainStick.home.y - _mainStick.radius);
  return point.x < CGRectGetWidth(self.bounds) * 0.45 && point.y > zoneTop;
}

- (CGPoint)clampedCentre:(CGPoint)point forStick:(DBPadStick*)stick
{
  const CGRect safe = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
  const CGFloat r = stick.radius;
  return CGPointMake(
      MIN(CGRectGetMaxX(safe) - r, MAX(CGRectGetMinX(safe) + r, point.x)),
      MIN(CGRectGetMaxY(safe) - r, MAX(CGRectGetMinY(safe) + r, point.y)));
}

#pragma mark - Driving the emulated pad

- (void)tap
{
  if (!_hapticsEnabled)
    return;
  [_haptics impactOccurredWithIntensity:0.55];
  [_haptics prepare];
}

- (void)setKey:(DBPadKey*)key pressed:(BOOL)pressed
{
  if (key.pressed == pressed)
    return;
  key.pressed = pressed;

  if (pressed)
  {
    db_set_control(key.control, 1.0);
    if (key.companion != kNoCompanion)
      db_set_control((DBPadControl)key.companion, 1.0);
    [self tap];
  }
  else
  {
    db_clear_control(key.control);
    if (key.companion != kNoCompanion)
      db_clear_control((DBPadControl)key.companion);
  }

  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  [self styleKey:key];
  [self applyKeyTransform:key];
  [CATransaction commit];
}

- (void)updateStick:(DBPadStick*)stick at:(CGPoint)point
{
  CGFloat dx = (point.x - stick.centre.x) / stick.radius;
  CGFloat dy = (point.y - stick.centre.y) / stick.radius;

  const CGFloat length = sqrt(dx * dx + dy * dy);
  if (length > 1.0)
  {
    dx /= length;
    dy /= length;
  }
  stick.offset = CGPointMake(dx, dy);

  // The knob follows the thumb exactly, but the value it reports does not:
  // inside the deadzone it is zero, and outside it the remaining travel is
  // stretched back over the full range. Reporting the raw offset instead makes
  // a stick that can never quite be centred and never quite reach the edge.
  CGFloat vx = 0, vy = 0;
  const CGFloat magnitude = MIN(1.0, length);
  if (magnitude > kStickDeadzone)
  {
    const CGFloat scaled = (magnitude - kStickDeadzone) / (1.0 - kStickDeadzone);
    vx = dx / magnitude * scaled;
    vy = dy / magnitude * scaled;
  }

  // Dolphin's stick axes are +Y up; UIKit's coordinates are +Y down.
  db_set_control(stick.axisX, vx);
  db_set_control(stick.axisY, -vy);

  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  stick.knobLayer.position = CGPointMake(stick.centre.x + dx * stick.radius,
                                         stick.centre.y + dy * stick.radius);
  stick.textLayer.position = stick.knobLayer.position;
  [CATransaction commit];
}

- (void)engageStick:(DBPadStick*)stick at:(CGPoint)point
{
  stick.engaged = YES;
  if (stick.floats)
    stick.centre = [self clampedCentre:point forStick:stick];

  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  [self applyStickGeometry:stick];
  [self styleStick:stick];
  [CATransaction commit];

  [self updateStick:stick at:point];
  [self tap];
}

- (void)releaseStick:(DBPadStick*)stick
{
  stick.engaged = NO;
  stick.offset = CGPointZero;
  stick.centre = stick.home;
  db_set_control(stick.axisX, 0.0);
  db_set_control(stick.axisY, 0.0);

  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  [self applyStickGeometry:stick];
  stick.textLayer.position = stick.knobLayer.position;
  [self styleStick:stick];
  [CATransaction commit];
}

- (void)updateCrossAt:(CGPoint)point
{
  const CGFloat reach = _cross.arm + _cross.thickness / 2;
  const CGFloat dx = (point.x - _cross.centre.x) / reach;
  const CGFloat dy = (point.y - _cross.centre.y) / reach;

  NSUInteger mask = 0;
  if (dy < -kCrossDeadzone)
    mask |= kDirUp;
  else if (dy > kCrossDeadzone)
    mask |= kDirDown;
  if (dx < -kCrossDeadzone)
    mask |= kDirLeft;
  else if (dx > kCrossDeadzone)
    mask |= kDirRight;

  [self setCrossMask:mask];
}

- (void)setCrossMask:(NSUInteger)mask
{
  if (mask == _cross.mask)
    return;

  const NSUInteger bits[4] = {kDirUp, kDirDown, kDirLeft, kDirRight};
  const DBPadControl controls[4] = {DB_PAD_DPAD_UP, DB_PAD_DPAD_DOWN, DB_PAD_DPAD_LEFT,
                                    DB_PAD_DPAD_RIGHT};
  BOOL newlyHeld = NO;
  for (NSUInteger i = 0; i < 4; i++)
  {
    const BOOL was = (_cross.mask & bits[i]) != 0;
    const BOOL now = (mask & bits[i]) != 0;
    if (was == now)
      continue;
    if (now)
    {
      db_set_control(controls[i], 1.0);
      newlyHeld = YES;
    }
    else
    {
      db_clear_control(controls[i]);
    }
  }

  _cross.mask = mask;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  [self styleCross];
  [CATransaction commit];
  if (newlyHeld)
    [self tap];
}

#pragma mark - Editing touches

// The control under a point, for dragging. The same order as play -- buttons,
// D-pad, sticks -- so that what a finger picks up in the editor is what it
// would have pressed.
- (id)editTargetAt:(CGPoint)point
{
  DBPadKey* key = [self keyAt:point];
  if (key)
    return key;
  if ([self crossContains:point])
    return _cross;
  if ([self stick:_cStick contains:point])
    return _cStick;
  if ([self stick:_mainStick contains:point])
    return _mainStick;
  return nil;
}

- (CGPoint)centreOf:(id)target
{
  if ([target isKindOfClass:DBPadKey.class])
    return ((DBPadKey*)target).centre;
  if ([target isKindOfClass:DBPadStick.class])
    return ((DBPadStick*)target).home;
  return _cross.centre;
}

- (void)moveTarget:(id)target to:(CGPoint)centre
{
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  if ([target isKindOfClass:DBPadKey.class])
  {
    DBPadKey* key = (DBPadKey*)target;
    // A rotated bean is kept inside by its longer side, which over-clamps by
    // a few points on the diagonal and never lets a corner poke out.
    const CGFloat half = MAX(key.size.width, key.size.height) / 2;
    key.centre = [self clampCentre:centre halfWidth:half halfHeight:half];
    key.shapeLayer.position = key.centre;
    key.textLayer.position = key.centre;
    [self applyKeyRing:key];
  }
  else if ([target isKindOfClass:DBPadStick.class])
  {
    DBPadStick* stick = (DBPadStick*)target;
    stick.home = [self clampCentre:centre halfWidth:stick.radius halfHeight:stick.radius];
    stick.centre = stick.home;
    [self applyStickGeometry:stick];
  }
  else if (target == _cross)
  {
    const CGFloat reach = _cross.arm + _cross.thickness / 2;
    _cross.centre = [self clampCentre:centre halfWidth:reach halfHeight:reach];
    [self applyCrossGeometry];
  }
  [CATransaction commit];
}

- (NSString*)nameOf:(id)target
{
  if ([target isKindOfClass:DBPadKey.class])
    return ((DBPadKey*)target).name;
  if ([target isKindOfClass:DBPadStick.class])
    return ((DBPadStick*)target).name;
  return _cross.name;
}

- (void)editTouchesBegan:(NSSet<UITouch*>*)touches
{
  if (_dragTouch)
    return;
  for (UITouch* touch in touches)
  {
    const CGPoint point = [touch locationInView:self];
    id target = [self editTargetAt:point];
    if (!target)
      continue;
    _dragTouch = touch;
    _dragTarget = target;
    _dragAnchor = [self centreOf:target];
    _dragOrigin = point;
    [self applyStyle];
    [self tap];
    return;
  }
}

- (void)editTouchesMoved:(NSSet<UITouch*>*)touches
{
  if (!_dragTouch || ![touches containsObject:_dragTouch])
    return;
  const CGPoint point = [_dragTouch locationInView:self];
  // The control keeps its offset from the finger rather than jumping to sit
  // centred under it: a thumb that picked up the edge of A should still be
  // holding the edge of A.
  [self moveTarget:_dragTarget
                to:CGPointMake(_dragAnchor.x + point.x - _dragOrigin.x,
                               _dragAnchor.y + point.y - _dragOrigin.y)];
}

- (void)editTouchesEnded:(NSSet<UITouch*>*)touches
{
  if (!_dragTouch || ![touches containsObject:_dragTouch])
    return;
  id target = _dragTarget;
  _dragTouch = nil;
  _dragTarget = nil;
  [self storeCentre:[self centreOf:target] forName:[self nameOf:target]];
  [self applyStyle];
}

#pragma mark - Touches

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  if (_hapticsEnabled)
    [_haptics prepare];

  if (_editing)
  {
    [self editTouchesBegan:touches];
    return;
  }

  for (UITouch* touch in touches)
  {
    const CGPoint point = [touch locationInView:self];
    NSValue* identity = [NSValue valueWithNonretainedObject:touch];

    // Buttons first, then the D-pad, then the fixed C stick, and only then the
    // main stick's catch-all zone. Ordered by how specific the target is: the
    // zone is the widest thing on the screen and would otherwise swallow every
    // touch that landed near it.
    DBPadKey* key = [self keyAt:point];
    if (key)
    {
      _bindings[identity] = key;
      [self setKey:key pressed:YES];
      continue;
    }

    if ([self crossContains:point])
    {
      _bindings[identity] = _cross;
      [self updateCrossAt:point];
      continue;
    }

    if (!_cStick.engaged && [self stick:_cStick contains:point])
    {
      _bindings[identity] = _cStick;
      [self engageStick:_cStick at:point];
      continue;
    }

    if (!_mainStick.engaged &&
        ([self stick:_mainStick contains:point] || [self isInMainStickZone:point]))
    {
      _bindings[identity] = _mainStick;
      [self engageStick:_mainStick at:point];
    }
  }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  if (_editing)
  {
    [self editTouchesMoved:touches];
    return;
  }

  for (UITouch* touch in touches)
  {
    const CGPoint point = [touch locationInView:self];
    id binding = _bindings[[NSValue valueWithNonretainedObject:touch]];
    if (!binding)
      continue;

    if ([binding isKindOfClass:DBPadStick.class])
    {
      [self updateStick:(DBPadStick*)binding at:point];
    }
    else if (binding == _cross)
    {
      [self updateCrossAt:point];
    }
    else if ([binding isKindOfClass:DBPadKey.class])
    {
      // Sliding from one face button onto another re-targets, which is how a
      // roll from A to B is played on a screen with no edges to feel for.
      // Sliding onto nothing holds the button instead of dropping it: a thumb
      // wanders during a long press, and a press that dies halfway is worse
      // than one that outstays a few millimetres.
      DBPadKey* now = [self keyAt:point];
      DBPadKey* held = (DBPadKey*)binding;
      if (now && now != held)
      {
        [self setKey:held pressed:NO];
        [self setKey:now pressed:YES];
        _bindings[[NSValue valueWithNonretainedObject:touch]] = now;
      }
    }
  }
}

- (void)endTouches:(NSSet<UITouch*>*)touches
{
  if (_editing)
  {
    [self editTouchesEnded:touches];
    return;
  }

  for (UITouch* touch in touches)
  {
    NSValue* identity = [NSValue valueWithNonretainedObject:touch];
    id binding = _bindings[identity];
    if (!binding)
      continue;
    [_bindings removeObjectForKey:identity];

    if ([binding isKindOfClass:DBPadStick.class])
      [self releaseStick:(DBPadStick*)binding];
    else if (binding == _cross)
      [self setCrossMask:0];
    else if ([binding isKindOfClass:DBPadKey.class])
      [self setKey:(DBPadKey*)binding pressed:NO];
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

// Everything the overlay is holding, let go at once. Called when a physical
// controller takes over and when the game view goes away: a control left set
// would otherwise stay pressed for the rest of the session, with nothing on
// screen still able to release it.
- (void)releaseAll
{
  [_bindings removeAllObjects];
  for (DBPadKey* key in _keys)
    [self setKey:key pressed:NO];
  [self setCrossMask:0];
  [self releaseStick:_mainStick];
  [self releaseStick:_cStick];
}

- (void)setHidden:(BOOL)hidden
{
  if (hidden && !self.hidden)
    [self releaseAll];
  [super setHidden:hidden];
}

- (void)willMoveToWindow:(UIWindow*)window
{
  [super willMoveToWindow:window];
  if (!window)
    [self releaseAll];
}

@end
