// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBGameCell.h"

#import "DBLibrary.h"
#import "DBTheme.h"

namespace
{
// A GameCube case is a little taller than it is wide. Keeping the tiles to
// that shape is what makes a grid of generated art read as a shelf of games
// rather than as a page of buttons.
constexpr CGFloat kCoverAspect = 0.72;  // width / height
// Two lines of title plus one of status. GameCube titles run long -- the disc
// header gives 64 characters and uses them -- and a single truncated line
// turns "Disney's Extreme Skate Adventure" into "Disney's Extreme Skate...".
constexpr CGFloat kLabelHeight = 72.0;
}  // namespace

@implementation DBGameCell
{
  UIImageView* _cover;
  UIView* _badge;
  UIImageView* _badgeIcon;
  UILabel* _title;
  UILabel* _status;
  NSString* _artDiscID;
  BOOL _artDimmed;
  CGSize _artSize;
}

+ (NSString*)reuseIdentifier
{
  return @"DBGameCell";
}

+ (CGFloat)heightForWidth:(CGFloat)width
{
  return round(width / kCoverAspect) + kLabelHeight;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  self = [super initWithFrame:frame];
  if (!self)
    return nil;

  _cover = [[UIImageView alloc] init];
  _cover.contentMode = UIViewContentModeScaleAspectFill;
  _cover.clipsToBounds = YES;
  _cover.layer.cornerRadius = 12;
  _cover.layer.cornerCurve = kCACornerCurveContinuous;
  _cover.backgroundColor = UIColor.secondarySystemBackgroundColor;
  // A hairline inside the artwork's own edge. Without it a dark tile and a
  // dark background merge into one shape and the grid loses its rhythm.
  _cover.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;
  _cover.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.16].CGColor;
  _cover.translatesAutoresizingMaskIntoConstraints = NO;
  [self.contentView addSubview:_cover];

  // The one thing worth reading at a glance: whether tapping this does
  // anything. Playable is the common case, so it gets the quiet treatment and
  // the exception gets the loud one.
  _badge = [[UIView alloc] init];
  _badge.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.55];
  _badge.layer.cornerRadius = 13;
  _badge.translatesAutoresizingMaskIntoConstraints = NO;
  [_cover addSubview:_badge];

  _badgeIcon = [[UIImageView alloc] init];
  _badgeIcon.contentMode = UIViewContentModeScaleAspectFit;
  _badgeIcon.tintColor = UIColor.whiteColor;
  _badgeIcon.translatesAutoresizingMaskIntoConstraints = NO;
  [_badge addSubview:_badgeIcon];

  _title = [[UILabel alloc] init];
  _title.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
  _title.textColor = UIColor.labelColor;
  _title.numberOfLines = 2;
  _title.lineBreakMode = NSLineBreakByTruncatingTail;
  _title.translatesAutoresizingMaskIntoConstraints = NO;
  [self.contentView addSubview:_title];

  _status = [[UILabel alloc] init];
  _status.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
  _status.textColor = UIColor.secondaryLabelColor;
  _status.numberOfLines = 1;
  _status.translatesAutoresizingMaskIntoConstraints = NO;
  [self.contentView addSubview:_status];

  [NSLayoutConstraint activateConstraints:@[
    [_cover.topAnchor constraintEqualToAnchor:self.contentView.topAnchor],
    [_cover.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
    [_cover.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
    [_cover.heightAnchor constraintEqualToAnchor:_cover.widthAnchor
                                      multiplier:1.0 / kCoverAspect],

    [_badge.topAnchor constraintEqualToAnchor:_cover.topAnchor constant:8],
    [_badge.trailingAnchor constraintEqualToAnchor:_cover.trailingAnchor constant:-8],
    [_badge.widthAnchor constraintEqualToConstant:26],
    [_badge.heightAnchor constraintEqualToConstant:26],
    [_badgeIcon.centerXAnchor constraintEqualToAnchor:_badge.centerXAnchor],
    [_badgeIcon.centerYAnchor constraintEqualToAnchor:_badge.centerYAnchor],
    [_badgeIcon.widthAnchor constraintEqualToConstant:14],
    [_badgeIcon.heightAnchor constraintEqualToConstant:14],

    [_title.topAnchor constraintEqualToAnchor:_cover.bottomAnchor constant:8],
    [_title.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:2],
    [_title.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-2],

    [_status.topAnchor constraintEqualToAnchor:_title.bottomAnchor constant:2],
    [_status.leadingAnchor constraintEqualToAnchor:_title.leadingAnchor],
    [_status.trailingAnchor constraintEqualToAnchor:_title.trailingAnchor],
    [_status.bottomAnchor constraintLessThanOrEqualToAnchor:self.contentView.bottomAnchor],
  ]];

  return self;
}

- (void)configureWithEntry:(DBGameEntry*)entry
{
  _title.text = entry.title;
  _title.alpha = entry.playable ? 1.0 : 0.55;

  if (entry.playable)
  {
    _status.text = [NSByteCountFormatter stringFromByteCount:(long long)entry.extractedBytes
                                                  countStyle:NSByteCountFormatterCountStyleFile];
    _status.textColor = UIColor.secondaryLabelColor;
    _badgeIcon.image = [UIImage systemImageNamed:@"play.fill"];
    _badge.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.5];
  }
  else
  {
    _status.text = @"Not in this build";
    _status.textColor = UIColor.tertiaryLabelColor;
    _badgeIcon.image = [UIImage systemImageNamed:@"lock.fill"];
    _badge.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.62];
  }

  [self setNeedsLayout];
  [self layoutIfNeeded];
  [self updateArtworkForEntry:entry];
}

- (void)updateArtworkForEntry:(DBGameEntry*)entry
{
  const CGSize size = _cover.bounds.size;
  if (size.width <= 0 || size.height <= 0)
    return;

  // Regenerating on every reuse would redraw a gradient and three ellipses per
  // cell per scroll. The tile only changes when the disc, the size, or the
  // playable state does, so those three are the whole cache key.
  if ([_artDiscID isEqualToString:entry.discID] && _artDimmed == !entry.playable &&
      CGSizeEqualToSize(_artSize, size))
  {
    return;
  }

  _artDiscID = entry.discID;
  _artDimmed = !entry.playable;
  _artSize = size;
  _cover.image = [DBTheme coverImageForDiscID:entry.discID
                                         size:size
                                        scale:self.window.screen.scale ?: UIScreen.mainScreen.scale
                                       dimmed:!entry.playable];
}

- (void)prepareForReuse
{
  [super prepareForReuse];
  _cover.image = nil;
  _artDiscID = nil;
  _artSize = CGSizeZero;
}

// A press that reads on the artwork itself, so the whole tile behaves like the
// button it is rather than only the title responding.
- (void)setHighlighted:(BOOL)highlighted
{
  [super setHighlighted:highlighted];
  [UIView animateWithDuration:0.12
                   animations:^{
                     self.contentView.transform = highlighted
                                                      ? CGAffineTransformMakeScale(0.96, 0.96)
                                                      : CGAffineTransformIdentity;
                     self.contentView.alpha = highlighted ? 0.85 : 1.0;
                   }];
}

@end
