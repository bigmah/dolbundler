// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBGameCell.h"

#import "DBBanner.h"
#import "DBLibrary.h"
#import "DBTheme.h"

namespace
{
// The banner is three times as wide as it is tall. A card at 16:10 holds it
// with room above and below for the backdrop to show as colour, which is what
// makes a grid of them read as a shelf of games rather than as a strip of
// stickers.
constexpr CGFloat kCardAspect = 1.6;  // width / height
// Two lines of title plus one of status. GameCube titles run long -- the disc
// gives 64 characters and uses them -- and a single truncated line turns
// "Disney's Extreme Skate Adventure" into "Disney's Extreme Skate...".
constexpr CGFloat kLabelHeight = 64.0;
}  // namespace

@implementation DBGameCell
{
  UIImageView* _art;
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
  return round(width / kCardAspect) + kLabelHeight;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  self = [super initWithFrame:frame];
  if (!self)
    return nil;

  _art = [[UIImageView alloc] init];
  _art.contentMode = UIViewContentModeScaleAspectFill;
  _art.clipsToBounds = YES;
  _art.layer.cornerRadius = 14;
  _art.layer.cornerCurve = kCACornerCurveContinuous;
  _art.backgroundColor = UIColor.secondarySystemBackgroundColor;
  // A hairline inside the artwork's own edge. Without it a dark card and a
  // dark background merge into one shape and the grid loses its rhythm.
  _art.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;
  _art.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.16].CGColor;
  _art.translatesAutoresizingMaskIntoConstraints = NO;
  [self.contentView addSubview:_art];

  // Only the exception is marked. Playable is the common case and the whole
  // card is its button; a disc this build cannot play gets a lock, since the
  // dimmed art alone could be read as a loading state.
  _badge = [[UIView alloc] init];
  _badge.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.62];
  _badge.layer.cornerRadius = 13;
  _badge.translatesAutoresizingMaskIntoConstraints = NO;
  [_art addSubview:_badge];

  _badgeIcon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"lock.fill"]];
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
  _status.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
  _status.textColor = UIColor.secondaryLabelColor;
  _status.numberOfLines = 1;
  _status.translatesAutoresizingMaskIntoConstraints = NO;
  [self.contentView addSubview:_status];

  [NSLayoutConstraint activateConstraints:@[
    [_art.topAnchor constraintEqualToAnchor:self.contentView.topAnchor],
    [_art.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
    [_art.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
    [_art.heightAnchor constraintEqualToAnchor:_art.widthAnchor multiplier:1.0 / kCardAspect],

    [_badge.topAnchor constraintEqualToAnchor:_art.topAnchor constant:8],
    [_badge.trailingAnchor constraintEqualToAnchor:_art.trailingAnchor constant:-8],
    [_badge.widthAnchor constraintEqualToConstant:26],
    [_badge.heightAnchor constraintEqualToConstant:26],
    [_badgeIcon.centerXAnchor constraintEqualToAnchor:_badge.centerXAnchor],
    [_badgeIcon.centerYAnchor constraintEqualToAnchor:_badge.centerYAnchor],
    [_badgeIcon.widthAnchor constraintEqualToConstant:13],
    [_badgeIcon.heightAnchor constraintEqualToConstant:13],

    [_title.topAnchor constraintEqualToAnchor:_art.bottomAnchor constant:8],
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
  _title.text = entry.displayTitle;
  _title.alpha = entry.playable ? 1.0 : 0.55;
  _badge.hidden = entry.playable;

  if (entry.playable)
  {
    // The disc ID stays visible somewhere on the card: it is what a Mac build
    // is keyed by, and the one thing to quote when a game is missing from one.
    NSString* size = [NSByteCountFormatter stringFromByteCount:(long long)entry.extractedBytes
                                                    countStyle:NSByteCountFormatterCountStyleFile];
    _status.text = [NSString stringWithFormat:@"%@ · %@", entry.discID, size];
    _status.textColor = UIColor.secondaryLabelColor;
  }
  else
  {
    _status.text = [NSString stringWithFormat:@"%@ · Not in this build", entry.discID];
    _status.textColor = UIColor.tertiaryLabelColor;
  }

  [self setNeedsLayout];
  [self layoutIfNeeded];
  [self updateArtworkForEntry:entry];
}

- (void)updateArtworkForEntry:(DBGameEntry*)entry
{
  const CGSize size = _art.bounds.size;
  if (size.width <= 0 || size.height <= 0)
    return;

  // Regenerating on every reuse would decode and composite the banner per
  // cell per scroll. The card only changes when the disc, the size, or the
  // playable state does, so those three are the whole cache key.
  if ([_artDiscID isEqualToString:entry.discID] && _artDimmed == !entry.playable &&
      CGSizeEqualToSize(_artSize, size))
  {
    return;
  }

  _artDiscID = entry.discID;
  _artDimmed = !entry.playable;
  _artSize = size;
  DBBanner* banner = [DBBanner cachedBannerAtPath:entry.bannerPath discID:entry.discID];
  _art.image = [DBTheme cardImageForDiscID:entry.discID
                                    banner:banner
                                      size:size
                                     scale:self.window.screen.scale ?: UIScreen.mainScreen.scale
                                    dimmed:!entry.playable];
}

- (void)prepareForReuse
{
  [super prepareForReuse];
  _art.image = nil;
  _artDiscID = nil;
  _artSize = CGSizeZero;
}

// A press that reads on the artwork itself, so the whole card behaves like
// the button it is rather than only the title responding.
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
