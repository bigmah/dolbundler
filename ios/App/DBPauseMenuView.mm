// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBPauseMenuView.h"

#import "DBBanner.h"
#import "DBLibrary.h"
#import "DBSettings.h"
#import "DBTheme.h"

namespace
{
constexpr CGFloat kArtWidth = 200.0;
constexpr CGFloat kArtAspect = 1.6;
constexpr CGFloat kPanelInset = 22.0;
constexpr CGFloat kColumnGap = 26.0;
}  // namespace

@implementation DBPauseMenuView
{
  DBGameEntry* _game;
  UIView* _scrim;
  UIView* _panel;
  UISlider* _opacitySlider;
  UISlider* _scaleSlider;
  UISwitch* _hapticsSwitch;
  UISwitch* _performanceSwitch;
  UIButton* _resetButton;
}

- (instancetype)initWithGame:(DBGameEntry*)game
{
  self = [super initWithFrame:CGRectZero];
  if (!self)
    return nil;

  _game = game;
  [self buildSubviews];
  return self;
}

#pragma mark - Construction

- (UILabel*)rowLabel:(NSString*)text
{
  UILabel* label = [[UILabel alloc] init];
  label.text = text;
  label.font = [UIFont systemFontOfSize:15 weight:UIFontWeightRegular];
  label.textColor = [UIColor colorWithWhite:1.0 alpha:0.92];
  [label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                         forAxis:UILayoutConstraintAxisHorizontal];
  return label;
}

// A section heading in the style of a grouped table's, so the two groups read
// as groups without a box round each.
- (UILabel*)sectionLabel:(NSString*)text
{
  UILabel* label = [[UILabel alloc] init];
  label.attributedText = [[NSAttributedString alloc]
      initWithString:text.uppercaseString
          attributes:@{
            NSFontAttributeName : [UIFont systemFontOfSize:11 weight:UIFontWeightSemibold],
            NSForegroundColorAttributeName : [UIColor colorWithWhite:1.0 alpha:0.5],
            NSKernAttributeName : @0.8,
          }];
  return label;
}

- (UIView*)switchRow:(NSString*)text toggle:(UISwitch*)toggle action:(SEL)action
{
  toggle.onTintColor = DBTheme.accent;
  [toggle addTarget:self action:action forControlEvents:UIControlEventValueChanged];

  UIStackView* row =
      [[UIStackView alloc] initWithArrangedSubviews:@[ [self rowLabel:text], toggle ]];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  return row;
}

- (UIView*)sliderRow:(NSString*)text slider:(UISlider*)slider action:(SEL)action
{
  slider.minimumTrackTintColor = DBTheme.accent;
  [slider addTarget:self action:action forControlEvents:UIControlEventValueChanged];
  [slider setContentHuggingPriority:UILayoutPriorityDefaultLow - 1
                            forAxis:UILayoutConstraintAxisHorizontal];

  UILabel* label = [self rowLabel:text];
  UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:@[ label, slider ]];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 14;
  // The two sliders line up: their labels are given the same width, so
  // "Opacity" and "Size" do not leave the tracks starting in different places.
  [label.widthAnchor constraintEqualToConstant:64].active = YES;
  return row;
}

- (UIButton*)actionButton:(NSString*)title
                    image:(NSString*)symbol
               background:(UIColor*)background
                    title:(UIColor*)titleColor
                   action:(SEL)action
{
  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.title = title;
  if (symbol)
  {
    config.image = [UIImage systemImageNamed:symbol
                           withConfiguration:[UIImageSymbolConfiguration
                                                 configurationWithPointSize:13
                                                                     weight:UIImageSymbolWeightSemibold]];
    config.imagePadding = 6;
  }
  config.baseBackgroundColor = background;
  config.baseForegroundColor = titleColor;
  config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
  config.contentInsets = NSDirectionalEdgeInsetsMake(11, 14, 11, 14);
  config.titleTextAttributesTransformer = ^NSDictionary*(NSDictionary* incoming) {
    NSMutableDictionary* attrs = [incoming mutableCopy];
    attrs[NSFontAttributeName] = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    return attrs;
  };

  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
}

- (UIView*)buildLeftColumn
{
  // The same card the library shows, so the game someone paused is the one
  // they tapped. Drawn once, at the panel's own size.
  UIImageView* art = [[UIImageView alloc] init];
  art.contentMode = UIViewContentModeScaleAspectFill;
  art.clipsToBounds = YES;
  art.layer.cornerRadius = 12;
  art.layer.cornerCurve = kCACornerCurveContinuous;
  art.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;
  art.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.16].CGColor;
  const CGSize artSize = CGSizeMake(kArtWidth, round(kArtWidth / kArtAspect));
  art.image = [DBTheme cardImageForDiscID:_game.discID
                                   banner:[DBBanner cachedBannerAtPath:_game.bannerPath
                                                                discID:_game.discID]
                                     size:artSize
                                    scale:UIScreen.mainScreen.scale
                                   dimmed:NO];
  [art.widthAnchor constraintEqualToConstant:artSize.width].active = YES;
  [art.heightAnchor constraintEqualToConstant:artSize.height].active = YES;

  UILabel* heading = [[UILabel alloc] init];
  heading.text = _game.displayTitle.length ? _game.displayTitle : @"Game";
  heading.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
  heading.textColor = UIColor.whiteColor;
  heading.numberOfLines = 2;

  UILabel* paused = [[UILabel alloc] init];
  paused.text = [NSString stringWithFormat:@"Paused · %@", _game.discID ?: @""];
  paused.font = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
  paused.textColor = [UIColor colorWithWhite:1.0 alpha:0.45];

  UIView* spacer = [[UIView alloc] init];
  [spacer setContentHuggingPriority:UILayoutPriorityDefaultLow - 10
                            forAxis:UILayoutConstraintAxisVertical];

  UIStackView* column = [[UIStackView alloc] initWithArrangedSubviews:@[
    art,
    heading,
    paused,
    spacer,
    [self actionButton:@"Resume"
                 image:@"play.fill"
            background:DBTheme.accent
                 title:UIColor.whiteColor
                action:@selector(resumeTapped)],
    [self actionButton:@"Quit Game"
                 image:nil
            background:[UIColor colorWithWhite:1.0 alpha:0.10]
                 title:[UIColor colorWithRed:1.0 green:0.42 blue:0.40 alpha:1.0]
                action:@selector(quitTapped)],
  ]];
  column.axis = UILayoutConstraintAxisVertical;
  column.spacing = 8;
  [column setCustomSpacing:12 afterView:art];
  [column setCustomSpacing:3 afterView:heading];
  [column setCustomSpacing:14 afterView:spacer];
  [column.widthAnchor constraintEqualToConstant:kArtWidth].active = YES;
  return column;
}

- (UIView*)buildRightColumn
{
  DBSettings* settings = DBSettings.shared;

  _opacitySlider = [[UISlider alloc] init];
  _opacitySlider.minimumValue = 0.25;
  _opacitySlider.maximumValue = 1.0;
  _opacitySlider.value = settings.padOpacity;

  _scaleSlider = [[UISlider alloc] init];
  _scaleSlider.minimumValue = DBSettings.minPadScale;
  _scaleSlider.maximumValue = DBSettings.maxPadScale;
  _scaleSlider.value = settings.padScale;

  _hapticsSwitch = [[UISwitch alloc] init];
  _hapticsSwitch.on = settings.hapticsEnabled;
  _performanceSwitch = [[UISwitch alloc] init];
  _performanceSwitch.on = settings.showsPerformance;

  UIButton* edit = [self actionButton:@"Edit Layout"
                                image:@"arrow.up.and.down.and.arrow.left.and.right"
                           background:[UIColor colorWithWhite:1.0 alpha:0.12]
                                title:UIColor.whiteColor
                               action:@selector(editLayoutTapped)];
  _resetButton = [self actionButton:@"Reset"
                              image:nil
                         background:[UIColor colorWithWhite:1.0 alpha:0.12]
                              title:UIColor.whiteColor
                             action:@selector(resetLayoutTapped)];
  [self updateResetButton];

  UIStackView* layoutRow = [[UIStackView alloc] initWithArrangedSubviews:@[ edit, _resetButton ]];
  layoutRow.axis = UILayoutConstraintAxisHorizontal;
  layoutRow.spacing = 8;
  layoutRow.distribution = UIStackViewDistributionFillProportionally;

  UIStackView* column = [[UIStackView alloc] initWithArrangedSubviews:@[
    [self sectionLabel:@"Controls"],
    [self sliderRow:@"Opacity" slider:_opacitySlider action:@selector(opacityChanged)],
    [self sliderRow:@"Size" slider:_scaleSlider action:@selector(scaleChanged)],
    [self switchRow:@"Haptics" toggle:_hapticsSwitch action:@selector(hapticsChanged)],
    layoutRow,
    [self sectionLabel:@"Display"],
    [self switchRow:@"Performance stats"
             toggle:_performanceSwitch
             action:@selector(performanceChanged)],
  ]];
  column.axis = UILayoutConstraintAxisVertical;
  column.spacing = 10;
  [column setCustomSpacing:6 afterView:column.arrangedSubviews[0]];
  [column setCustomSpacing:20 afterView:layoutRow];
  [column setCustomSpacing:6 afterView:column.arrangedSubviews[5]];
  return column;
}

- (void)buildSubviews
{
  // A plain dark scrim rather than a blur. The game's last frame is sitting in
  // a CAMetalLayer underneath, and asking UIKit to blur that is both unreliable
  // and expensive on a device already short of GPU time.
  _scrim = [[UIView alloc] init];
  _scrim.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.62];
  _scrim.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:_scrim];

  UITapGestureRecognizer* tapAway =
      [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(resumeTapped)];
  [_scrim addGestureRecognizer:tapAway];

  _panel = [[UIView alloc] init];
  _panel.backgroundColor = [UIColor colorWithWhite:0.11 alpha:0.98];
  _panel.layer.cornerRadius = 22;
  _panel.layer.cornerCurve = kCACornerCurveContinuous;
  _panel.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;
  _panel.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.14].CGColor;
  _panel.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:_panel];

  UIView* left = [self buildLeftColumn];
  UIView* right = [self buildRightColumn];
  UIStackView* columns = [[UIStackView alloc] initWithArrangedSubviews:@[ left, right ]];
  columns.axis = UILayoutConstraintAxisHorizontal;
  columns.alignment = UIStackViewAlignmentFill;
  columns.spacing = kColumnGap;
  columns.translatesAutoresizingMaskIntoConstraints = NO;
  [_panel addSubview:columns];

  // The panel takes the width its content asks for, up to what fits: the left
  // column is fixed, the right one wants about 300 points of slider.
  NSLayoutConstraint* preferredWidth = [_panel.widthAnchor constraintEqualToConstant:600];
  preferredWidth.priority = UILayoutPriorityDefaultHigh;

  [NSLayoutConstraint activateConstraints:@[
    [_scrim.topAnchor constraintEqualToAnchor:self.topAnchor],
    [_scrim.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    [_scrim.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [_scrim.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],

    [_panel.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
    [_panel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    preferredWidth,
    [_panel.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor
                                                      constant:16],
    [_panel.trailingAnchor constraintLessThanOrEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor
                                                    constant:-16],
    [_panel.topAnchor constraintGreaterThanOrEqualToAnchor:self.topAnchor constant:12],

    [columns.topAnchor constraintEqualToAnchor:_panel.topAnchor constant:kPanelInset],
    [columns.bottomAnchor constraintEqualToAnchor:_panel.bottomAnchor constant:-kPanelInset],
    [columns.leadingAnchor constraintEqualToAnchor:_panel.leadingAnchor constant:kPanelInset],
    [columns.trailingAnchor constraintEqualToAnchor:_panel.trailingAnchor constant:-kPanelInset],
  ]];
}

- (void)updateResetButton
{
  const BOOL custom = DBSettings.shared.hasCustomPadLayout;
  _resetButton.enabled = custom;
  _resetButton.alpha = custom ? 1.0 : 0.4;
}

#pragma mark - Actions

- (void)settingsChanged
{
  [self updateResetButton];
  if (self.onSettingsChanged)
    self.onSettingsChanged();
}

- (void)opacityChanged
{
  DBSettings.shared.padOpacity = _opacitySlider.value;
  [self settingsChanged];
}

- (void)scaleChanged
{
  DBSettings.shared.padScale = _scaleSlider.value;
  [self settingsChanged];
}

- (void)hapticsChanged
{
  DBSettings.shared.hapticsEnabled = _hapticsSwitch.isOn;
  [self settingsChanged];
}

- (void)performanceChanged
{
  DBSettings.shared.showsPerformance = _performanceSwitch.isOn;
  [self settingsChanged];
}

- (void)editLayoutTapped
{
  if (self.onEditLayout)
    self.onEditLayout();
}

- (void)resetLayoutTapped
{
  if (self.onResetLayout)
    self.onResetLayout();
  _scaleSlider.value = DBSettings.shared.padScale;
  [self updateResetButton];
}

- (void)resumeTapped
{
  if (self.onResume)
    self.onResume();
}

- (void)quitTapped
{
  if (self.onQuit)
    self.onQuit();
}

#pragma mark - Presentation

- (void)presentInView:(UIView*)host
{
  self.frame = host.bounds;
  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.alpha = 0;
  _panel.transform = CGAffineTransformMakeScale(0.94, 0.94);
  [host addSubview:self];

  [UIView animateWithDuration:0.18
                   animations:^{
                     self.alpha = 1;
                     self->_panel.transform = CGAffineTransformIdentity;
                   }];
}

- (void)dismissWithCompletion:(void (^)(void))completion
{
  [UIView animateWithDuration:0.15
      animations:^{
        self.alpha = 0;
        self->_panel.transform = CGAffineTransformMakeScale(0.96, 0.96);
      }
      completion:^(BOOL finished) {
        [self removeFromSuperview];
        if (completion)
          completion();
      }];
}

@end
