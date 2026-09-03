// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBSettingsViewController.h"

#import "DBLibrary.h"
#import "DBSettings.h"
#import "DBTheme.h"

#include "dolbundler_run.h"

namespace
{
enum Section : NSInteger
{
  kSectionControls,
  kSectionDisplay,
  kSectionStorage,
  kSectionAbout,
  kSectionCount,
};

enum ControlsRow : NSInteger
{
  kRowOpacity,
  kRowSize,
  kRowHaptics,
  kRowResetLayout,
  kControlsRowCount,
};

enum StorageRow : NSInteger
{
  kRowLibrarySize,
  kRowFreeSpace,
  kStorageRowCount,
};

enum AboutRow : NSInteger
{
  kRowVersion,
  kRowBuiltInGames,
  kAboutRowCount,
};

}  // namespace

@implementation DBSettingsViewController
{
  UISlider* _opacity;
  UISlider* _scale;
  UISwitch* _haptics;
  UISwitch* _performance;
}

- (instancetype)init
{
  return [self initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.title = @"Settings";
  self.navigationItem.rightBarButtonItem =
      [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                    target:self
                                                    action:@selector(done)];
  DBSettings* settings = DBSettings.shared;

  _opacity = [[UISlider alloc] init];
  _opacity.minimumValue = 0.25;
  _opacity.maximumValue = 1.0;
  _opacity.value = settings.padOpacity;
  _opacity.minimumTrackTintColor = DBTheme.accent;
  [_opacity addTarget:self action:@selector(opacityChanged) forControlEvents:UIControlEventValueChanged];

  _scale = [[UISlider alloc] init];
  _scale.minimumValue = DBSettings.minPadScale;
  _scale.maximumValue = DBSettings.maxPadScale;
  _scale.value = settings.padScale;
  _scale.minimumTrackTintColor = DBTheme.accent;
  [_scale addTarget:self action:@selector(scaleChanged) forControlEvents:UIControlEventValueChanged];

  _haptics = [[UISwitch alloc] init];
  _haptics.on = settings.hapticsEnabled;
  _haptics.onTintColor = DBTheme.accent;
  [_haptics addTarget:self action:@selector(hapticsChanged) forControlEvents:UIControlEventValueChanged];

  _performance = [[UISwitch alloc] init];
  _performance.on = settings.showsPerformance;
  _performance.onTintColor = DBTheme.accent;
  [_performance addTarget:self
                   action:@selector(performanceChanged)
         forControlEvents:UIControlEventValueChanged];
}

- (void)done
{
  [self dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark - Values

- (void)opacityChanged
{
  DBSettings.shared.padOpacity = _opacity.value;
}

- (void)scaleChanged
{
  DBSettings.shared.padScale = _scale.value;
  [self reloadResetRow];
}

- (void)hapticsChanged
{
  DBSettings.shared.hapticsEnabled = _haptics.isOn;
}

- (void)performanceChanged
{
  DBSettings.shared.showsPerformance = _performance.isOn;
}

- (void)reloadResetRow
{
  [self.tableView
      reloadRowsAtIndexPaths:@[ [NSIndexPath indexPathForRow:kRowResetLayout
                                                   inSection:kSectionControls] ]
            withRowAnimation:UITableViewRowAnimationNone];
}

- (void)resetLayout
{
  [DBSettings.shared resetPadLayout];
  _scale.value = DBSettings.shared.padScale;
  [self reloadResetRow];
}

- (NSString*)builtInGames
{
  const int count = db_native_module_count();
  if (count <= 0)
    return @"None";
  NSMutableArray<NSString*>* ids = [NSMutableArray array];
  for (int i = 0; i < count; i++)
    [ids addObject:@(db_native_module_id(i))];
  return [ids componentsJoinedByString:@", "];
}

#pragma mark - Table

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView
{
  return kSectionCount;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section
{
  switch (section)
  {
  case kSectionControls:
    return kControlsRowCount;
  case kSectionDisplay:
    return 1;
  case kSectionStorage:
    return kStorageRowCount;
  case kSectionAbout:
    return kAboutRowCount;
  }
  return 0;
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section
{
  switch (section)
  {
  case kSectionControls:
    return @"On-screen controls";
  case kSectionDisplay:
    return @"Display";
  case kSectionStorage:
    return @"Storage";
  case kSectionAbout:
    return @"About";
  }
  return nil;
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section
{
  switch (section)
  {
  case kSectionControls:
    return @"To move the controls, open the menu in a running game and choose Edit Layout.";
  case kSectionStorage:
    return @"Games live in the DolBundler folder in Files. Delete one there, or press and hold "
           @"it in the library.";
  case kSectionAbout:
    return @"Games are recompiled on a Mac and linked into the app before it is signed, so "
           @"only the discs this build was made with can be played.";
  }
  return nil;
}

- (UIView*)sliderAccessory:(UISlider*)slider
{
  slider.frame = CGRectMake(0, 0, 150, 31);
  return slider;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath
{
  UITableViewCell* cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                                 reuseIdentifier:nil];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.accessoryView = nil;
  cell.textLabel.textColor = UIColor.labelColor;

  switch (indexPath.section)
  {
  case kSectionControls:
    switch (indexPath.row)
    {
    case kRowOpacity:
      cell.textLabel.text = @"Opacity";
      cell.accessoryView = [self sliderAccessory:_opacity];
      break;
    case kRowSize:
      cell.textLabel.text = @"Size";
      cell.accessoryView = [self sliderAccessory:_scale];
      break;
    case kRowHaptics:
      cell.textLabel.text = @"Haptics";
      cell.accessoryView = _haptics;
      break;
    case kRowResetLayout:
    {
      const BOOL custom = DBSettings.shared.hasCustomPadLayout;
      cell.textLabel.text = @"Reset Layout";
      cell.textLabel.textColor = custom ? DBTheme.accent : UIColor.tertiaryLabelColor;
      cell.selectionStyle =
          custom ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
      cell.detailTextLabel.text = custom ? nil : @"Using defaults";
      break;
    }
    }
    break;

  case kSectionDisplay:
    cell.textLabel.text = @"Performance stats";
    cell.accessoryView = _performance;
    break;

  case kSectionStorage:
  {
    DBLibrary* library = DBLibrary.shared;
    if (indexPath.row == kRowLibrarySize)
    {
      const NSUInteger count = library.games.count;
      cell.textLabel.text = count == 1 ? @"1 game" : [NSString stringWithFormat:@"%lu games",
                                                                                 (unsigned long)count];
      cell.detailTextLabel.text =
          [NSByteCountFormatter stringFromByteCount:(long long)library.totalExtractedBytes
                                         countStyle:NSByteCountFormatterCountStyleFile];
    }
    else
    {
      cell.textLabel.text = @"Available";
      cell.detailTextLabel.text =
          [NSByteCountFormatter stringFromByteCount:(long long)library.availableBytes
                                         countStyle:NSByteCountFormatterCountStyleFile];
    }
    break;
  }

  case kSectionAbout:
    if (indexPath.row == kRowVersion)
    {
      NSDictionary* info = NSBundle.mainBundle.infoDictionary;
      cell.textLabel.text = @"Version";
      cell.detailTextLabel.text =
          [NSString stringWithFormat:@"%@ (%@)", info[@"CFBundleShortVersionString"] ?: @"?",
                                     info[@"CFBundleVersion"] ?: @"?"];
    }
    else
    {
      cell.textLabel.text = @"Games in this build";
      cell.detailTextLabel.text = [self builtInGames];
      cell.detailTextLabel.numberOfLines = 0;
    }
    break;
  }
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath
{
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == kSectionControls && indexPath.row == kRowResetLayout &&
      DBSettings.shared.hasCustomPadLayout)
  {
    [self resetLayout];
  }
}

@end
