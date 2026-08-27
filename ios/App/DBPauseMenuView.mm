// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBPauseMenuView.h"

#import "DBSettings.h"
#import "DBTheme.h"

@implementation DBPauseMenuView
{
  UIView* _scrim;
  UIView* _panel;
  UISlider* _opacitySlider;
  UISwitch* _hapticsSwitch;
  UISwitch* _performanceSwitch;
  NSString* _title;
}

- (instancetype)initWithTitle:(NSString*)title
{
  self = [super initWithFrame:CGRectZero];
  if (!self)
    return nil;

  _title = title.length ? title : @"Game";
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
  return label;
}

- (UIView*)switchRow:(NSString*)text
              toggle:(UISwitch*)toggle
              action:(SEL)action
{
  toggle.onTintColor = DBTheme.accent;
  [toggle addTarget:self action:action forControlEvents:UIControlEventValueChanged];

  UIStackView* row = [[UIStackView alloc]
      initWithArrangedSubviews:@[ [self rowLabel:text], toggle ]];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  return row;
}

- (UIButton*)actionButton:(NSString*)title
               background:(UIColor*)background
                    title:(UIColor*)titleColor
                   action:(SEL)action
{
  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.title = title;
  config.baseBackgroundColor = background;
  config.baseForegroundColor = titleColor;
  config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
  config.contentInsets = NSDirectionalEdgeInsetsMake(12, 16, 12, 16);

  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
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

  UILabel* heading = [[UILabel alloc] init];
  heading.text = _title;
  heading.font = [UIFont systemFontOfSize:18 weight:UIFontWeightSemibold];
  heading.textColor = UIColor.whiteColor;
  heading.numberOfLines = 2;
  heading.textAlignment = NSTextAlignmentCenter;

  UILabel* paused = [[UILabel alloc] init];
  paused.text = @"Paused";
  paused.font = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
  paused.textColor = [UIColor colorWithWhite:1.0 alpha:0.45];
  paused.textAlignment = NSTextAlignmentCenter;

  // --- controls opacity -------------------------------------------------
  _opacitySlider = [[UISlider alloc] init];
  _opacitySlider.minimumValue = 0.25;
  _opacitySlider.maximumValue = 1.0;
  _opacitySlider.value = DBSettings.shared.padOpacity;
  _opacitySlider.minimumTrackTintColor = DBTheme.accent;
  [_opacitySlider addTarget:self
                     action:@selector(opacityChanged)
           forControlEvents:UIControlEventValueChanged];

  UIStackView* opacityRow =
      [[UIStackView alloc] initWithArrangedSubviews:@[ [self rowLabel:@"Control opacity"],
                                                       _opacitySlider ]];
  opacityRow.axis = UILayoutConstraintAxisHorizontal;
  opacityRow.alignment = UIStackViewAlignmentCenter;
  opacityRow.spacing = 14;
  [_opacitySlider setContentHuggingPriority:UILayoutPriorityDefaultLow - 1
                                    forAxis:UILayoutConstraintAxisHorizontal];

  _hapticsSwitch = [[UISwitch alloc] init];
  _hapticsSwitch.on = DBSettings.shared.hapticsEnabled;
  _performanceSwitch = [[UISwitch alloc] init];
  _performanceSwitch.on = DBSettings.shared.showsPerformance;

  UIView* separator = [[UIView alloc] init];
  separator.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.12];
  [separator.heightAnchor constraintEqualToConstant:1.0 / UIScreen.mainScreen.scale].active = YES;

  UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[
    heading,
    paused,
    opacityRow,
    [self switchRow:@"Haptics" toggle:_hapticsSwitch action:@selector(hapticsChanged)],
    [self switchRow:@"Performance stats"
             toggle:_performanceSwitch
             action:@selector(performanceChanged)],
    separator,
    [self actionButton:@"Resume"
            background:DBTheme.accent
                 title:UIColor.whiteColor
                action:@selector(resumeTapped)],
    [self actionButton:@"Quit game"
            background:[UIColor colorWithWhite:1.0 alpha:0.10]
                 title:[UIColor colorWithRed:1.0 green:0.42 blue:0.40 alpha:1.0]
                action:@selector(quitTapped)],
  ]];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 14;
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [stack setCustomSpacing:2 afterView:heading];
  [stack setCustomSpacing:18 afterView:paused];
  [stack setCustomSpacing:18 afterView:separator];
  [stack setCustomSpacing:8
                afterView:stack.arrangedSubviews[stack.arrangedSubviews.count - 2]];
  [_panel addSubview:stack];

  [NSLayoutConstraint activateConstraints:@[
    [_scrim.topAnchor constraintEqualToAnchor:self.topAnchor],
    [_scrim.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    [_scrim.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [_scrim.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],

    [_panel.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
    [_panel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    [_panel.widthAnchor constraintEqualToConstant:340],

    [stack.topAnchor constraintEqualToAnchor:_panel.topAnchor constant:22],
    [stack.bottomAnchor constraintEqualToAnchor:_panel.bottomAnchor constant:-22],
    [stack.leadingAnchor constraintEqualToAnchor:_panel.leadingAnchor constant:22],
    [stack.trailingAnchor constraintEqualToAnchor:_panel.trailingAnchor constant:-22],
  ]];
}

#pragma mark - Actions

- (void)opacityChanged
{
  DBSettings.shared.padOpacity = _opacitySlider.value;
  if (self.onSettingsChanged)
    self.onSettingsChanged();
}

- (void)hapticsChanged
{
  DBSettings.shared.hapticsEnabled = _hapticsSwitch.isOn;
  if (self.onSettingsChanged)
    self.onSettingsChanged();
}

- (void)performanceChanged
{
  DBSettings.shared.showsPerformance = _performanceSwitch.isOn;
  if (self.onSettingsChanged)
    self.onSettingsChanged();
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
