// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBLibraryViewController.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DBBanner.h"
#import "DBGameCell.h"
#import "DBGameViewController.h"
#import "DBLibrary.h"
#import "DBSettings.h"
#import "DBSettingsViewController.h"
#import "DBTheme.h"

// A GameCube disc is ~1.35 GB as an ISO and expands to roughly the same again
// once extracted. Importing with less than this free ends in a half-written
// game root, so it is refused up front with an explanation.
static const unsigned long long kMinimumFreeBytes = 3ULL * 1024 * 1024 * 1024;

// How often the import banner re-measures the growing game root. The extractor
// reports stages but not bytes, so the only account of progress is the
// directory itself -- and walking a disc's worth of files is not free, so it
// is walked once a second and not more.
static const NSTimeInterval kImportPollInterval = 1.0;

// What a press-and-hold on a card lifts out of the grid: the art at a size
// the grid cannot afford, and the description the disc wrote for itself --
// two lines the publisher chose, which no library screen has room for and
// which is the one thing about a game a card cannot say.
@interface DBGamePreviewController : UIViewController
- (instancetype)initWithEntry:(DBGameEntry*)entry;
@end

@implementation DBGamePreviewController
{
  DBGameEntry* _entry;
}

- (instancetype)initWithEntry:(DBGameEntry*)entry
{
  self = [super initWithNibName:nil bundle:nil];
  if (self)
    _entry = entry;
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.secondarySystemBackgroundColor;

  const CGFloat width = 320;
  const CGSize artSize = CGSizeMake(width, round(width / 1.6));
  DBBanner* banner = [DBBanner cachedBannerAtPath:_entry.bannerPath discID:_entry.discID];

  UIImageView* art = [[UIImageView alloc] init];
  art.image = [DBTheme cardImageForDiscID:_entry.discID
                                   banner:banner
                                     size:artSize
                                    scale:UIScreen.mainScreen.scale
                                   dimmed:!_entry.playable];
  art.contentMode = UIViewContentModeScaleAspectFill;
  art.clipsToBounds = YES;
  [art.heightAnchor constraintEqualToConstant:artSize.height].active = YES;

  UILabel* title = [[UILabel alloc] init];
  title.text = _entry.displayTitle;
  title.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
  title.textColor = UIColor.labelColor;
  title.numberOfLines = 2;

  NSString* size = [NSByteCountFormatter stringFromByteCount:(long long)_entry.extractedBytes
                                                  countStyle:NSByteCountFormatterCountStyleFile];
  UILabel* facts = [[UILabel alloc] init];
  facts.text = banner.maker.length
                   ? [NSString stringWithFormat:@"%@ · %@ · %@", banner.maker, _entry.discID, size]
                   : [NSString stringWithFormat:@"%@ · %@", _entry.discID, size];
  facts.font = [UIFont systemFontOfSize:12 weight:UIFontWeightMedium];
  facts.textColor = UIColor.secondaryLabelColor;
  facts.numberOfLines = 2;

  UILabel* summary = [[UILabel alloc] init];
  summary.text = banner.summary.length ? banner.summary
                 : _entry.playable    ? @"Tap to play."
                                      : @"This build has no native module for this disc.";
  summary.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  summary.textColor = UIColor.labelColor;
  summary.numberOfLines = 0;

  UIStackView* text = [[UIStackView alloc] initWithArrangedSubviews:@[ title, facts, summary ]];
  text.axis = UILayoutConstraintAxisVertical;
  text.spacing = 4;
  [text setCustomSpacing:10 afterView:facts];
  text.layoutMargins = UIEdgeInsetsMake(12, 14, 14, 14);
  text.layoutMarginsRelativeArrangement = YES;

  UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ art, text ]];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [stack.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [stack.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [stack.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [stack.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [stack.widthAnchor constraintEqualToConstant:width],
  ]];

  const CGSize fitted = [stack systemLayoutSizeFittingSize:CGSizeMake(width, UILayoutFittingCompressedSize.height)
                                     withHorizontalFittingPriority:UILayoutPriorityRequired
                                           verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
  self.preferredContentSize = CGSizeMake(width, fitted.height);
}

@end

@interface DBLibraryViewController () <UICollectionViewDataSource,
                                       UICollectionViewDelegate,
                                       UIDocumentPickerDelegate>
@end

@implementation DBLibraryViewController
{
  UICollectionView* _collection;
  UIView* _emptyState;

  UIView* _importBanner;
  UILabel* _importTitle;
  UILabel* _importDetail;
  UIProgressView* _importProgress;

  BOOL _importing;
  NSString* _importWatchPath;
  unsigned long long _importEstimate;
  NSTimer* _importTimer;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.title = @"Library";
  self.view.backgroundColor = UIColor.systemBackgroundColor;

  UIBarButtonItem* add =
      [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"plus"]
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(pickDisc)];
  add.accessibilityLabel = @"Add a disc";
  UIBarButtonItem* settings =
      [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"gearshape"]
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(showSettings)];
  settings.accessibilityLabel = @"Settings";
  self.navigationItem.rightBarButtonItems = @[ add, settings ];
  self.view.tintColor = DBTheme.accent;

  [self buildCollectionView];
  [self buildEmptyState];
  [self buildImportBanner];
}

#pragma mark - Construction

- (UICollectionViewLayout*)makeLayout
{
  return [[UICollectionViewCompositionalLayout alloc] initWithSectionProvider:^NSCollectionLayoutSection*(
      NSInteger section, id<NSCollectionLayoutEnvironment> environment) {
    // Columns are chosen from the width rather than fixed at two, so a phone
    // in landscape and an iPad both get tiles of about the same size instead
    // of two enormous ones.
    const CGFloat spacing = 14;
    const CGFloat margin = 16;
    const CGFloat available = environment.container.effectiveContentSize.width - margin * 2;
    const NSInteger columns = MAX(2, (NSInteger)floor((available + spacing) / (170.0 + spacing)));
    const CGFloat tile = floor((available - spacing * (columns - 1)) / columns);
    const CGFloat height = [DBGameCell heightForWidth:tile];

    NSCollectionLayoutItem* item = [NSCollectionLayoutItem
        itemWithLayoutSize:[NSCollectionLayoutSize
                               sizeWithWidthDimension:[NSCollectionLayoutDimension
                                                          absoluteDimension:tile]
                                      heightDimension:[NSCollectionLayoutDimension
                                                          absoluteDimension:height]]];

    NSCollectionLayoutGroup* group = [NSCollectionLayoutGroup
        horizontalGroupWithLayoutSize:[NSCollectionLayoutSize
                                          sizeWithWidthDimension:[NSCollectionLayoutDimension
                                                                     fractionalWidthDimension:1.0]
                                                 heightDimension:[NSCollectionLayoutDimension
                                                                     absoluteDimension:height]]
                     repeatingSubitem:item
                                count:(int)columns];
    group.interItemSpacing = [NSCollectionLayoutSpacing fixedSpacing:spacing];

    NSCollectionLayoutSection* layoutSection = [NSCollectionLayoutSection sectionWithGroup:group];
    layoutSection.interGroupSpacing = 20;
    layoutSection.contentInsets = NSDirectionalEdgeInsetsMake(8, margin, 28, margin);
    return layoutSection;
  }];
}

- (void)buildCollectionView
{
  _collection = [[UICollectionView alloc] initWithFrame:self.view.bounds
                                   collectionViewLayout:[self makeLayout]];
  _collection.backgroundColor = UIColor.clearColor;
  _collection.alwaysBounceVertical = YES;
  _collection.dataSource = self;
  _collection.delegate = self;
  _collection.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [_collection registerClass:DBGameCell.class
      forCellWithReuseIdentifier:DBGameCell.reuseIdentifier];

  // Discs can also arrive by being dropped into the app's folder in Files,
  // which produces no event here. A pull is the cheapest way to say "look
  // again" without a button that would sit unused most of the time.
  UIRefreshControl* refresh = [[UIRefreshControl alloc] init];
  [refresh addTarget:self
                action:@selector(pulledToRefresh:)
      forControlEvents:UIControlEventValueChanged];
  _collection.refreshControl = refresh;

  [self.view addSubview:_collection];
}

- (void)buildEmptyState
{
  UIImageView* icon = [[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"opticaldisc"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithPointSize:52
                                                                      weight:UIImageSymbolWeightLight]]];
  icon.tintColor = UIColor.tertiaryLabelColor;
  icon.contentMode = UIViewContentModeScaleAspectFit;

  UILabel* headline = [[UILabel alloc] init];
  headline.text = @"No games yet";
  headline.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
  headline.textColor = UIColor.labelColor;
  headline.textAlignment = NSTextAlignmentCenter;

  UILabel* body = [[UILabel alloc] init];
  body.text = @"Add a GameCube .iso, .ciso, or NKit image — or drop one into the DolBundler "
              @"folder in the Files app.";
  body.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  body.textColor = UIColor.secondaryLabelColor;
  body.textAlignment = NSTextAlignmentCenter;
  body.numberOfLines = 0;

  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.title = @"Add a disc";
  config.baseBackgroundColor = DBTheme.accent;
  config.cornerStyle = UIButtonConfigurationCornerStyleLarge;
  config.contentInsets = NSDirectionalEdgeInsetsMake(11, 22, 11, 22);
  UIButton* add = [UIButton buttonWithConfiguration:config primaryAction:nil];
  [add addTarget:self action:@selector(pickDisc) forControlEvents:UIControlEventTouchUpInside];

  UIStackView* stack =
      [[UIStackView alloc] initWithArrangedSubviews:@[ icon, headline, body, add ]];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.spacing = 12;
  [stack setCustomSpacing:18 afterView:icon];
  [stack setCustomSpacing:22 afterView:body];
  stack.translatesAutoresizingMaskIntoConstraints = NO;

  _emptyState = [[UIView alloc] init];
  _emptyState.translatesAutoresizingMaskIntoConstraints = NO;
  [_emptyState addSubview:stack];
  [self.view addSubview:_emptyState];

  [NSLayoutConstraint activateConstraints:@[
    [_emptyState.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [_emptyState.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [_emptyState.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [_emptyState.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [stack.centerXAnchor constraintEqualToAnchor:_emptyState.centerXAnchor],
    [stack.centerYAnchor constraintEqualToAnchor:_emptyState.centerYAnchor constant:-30],
    [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:_emptyState.leadingAnchor
                                                     constant:40],
    [stack.trailingAnchor constraintLessThanOrEqualToAnchor:_emptyState.trailingAnchor
                                                   constant:-40],
  ]];
}

- (void)buildImportBanner
{
  // A card at the bottom rather than a modal alert. An import takes minutes,
  // and blocking the whole library behind it means the one thing someone can
  // do while they wait -- look at what they already have -- is the one thing
  // they cannot.
  _importBanner = [[UIView alloc] init];
  _importBanner.backgroundColor = UIColor.secondarySystemBackgroundColor;
  _importBanner.layer.cornerRadius = 16;
  _importBanner.layer.cornerCurve = kCACornerCurveContinuous;
  _importBanner.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;
  _importBanner.layer.borderColor = UIColor.separatorColor.CGColor;
  // In light mode secondarySystemBackground sits a couple of percent away from
  // the page behind it, which is not enough to read as a card floating over
  // the grid. The shadow is what separates them. It costs nothing here: this
  // view is static and no game is running behind it.
  _importBanner.layer.shadowColor = UIColor.blackColor.CGColor;
  _importBanner.layer.shadowOpacity = 0.16;
  _importBanner.layer.shadowRadius = 18;
  _importBanner.layer.shadowOffset = CGSizeMake(0, 6);
  _importBanner.translatesAutoresizingMaskIntoConstraints = NO;
  _importBanner.hidden = YES;
  [self.view addSubview:_importBanner];

  UIActivityIndicatorView* spinner = [[UIActivityIndicatorView alloc]
      initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
  [spinner startAnimating];
  spinner.translatesAutoresizingMaskIntoConstraints = NO;
  [_importBanner addSubview:spinner];

  _importTitle = [[UILabel alloc] init];
  _importTitle.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
  _importTitle.textColor = UIColor.labelColor;
  _importTitle.numberOfLines = 1;

  _importDetail = [[UILabel alloc] init];
  _importDetail.font = [UIFont monospacedDigitSystemFontOfSize:12
                                                        weight:UIFontWeightRegular];
  _importDetail.textColor = UIColor.secondaryLabelColor;
  _importDetail.numberOfLines = 1;

  _importProgress = [[UIProgressView alloc]
      initWithProgressViewStyle:UIProgressViewStyleDefault];
  _importProgress.progressTintColor = DBTheme.accent;

  UIStackView* stack = [[UIStackView alloc]
      initWithArrangedSubviews:@[ _importTitle, _importProgress, _importDetail ]];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 6;
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_importBanner addSubview:stack];

  [NSLayoutConstraint activateConstraints:@[
    [_importBanner.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:16],
    [_importBanner.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-16],
    [_importBanner.bottomAnchor
        constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor
                       constant:-12],

    [spinner.leadingAnchor constraintEqualToAnchor:_importBanner.leadingAnchor constant:16],
    [spinner.centerYAnchor constraintEqualToAnchor:_importBanner.centerYAnchor],

    [stack.leadingAnchor constraintEqualToAnchor:spinner.trailingAnchor constant:14],
    [stack.trailingAnchor constraintEqualToAnchor:_importBanner.trailingAnchor constant:-16],
    [stack.topAnchor constraintEqualToAnchor:_importBanner.topAnchor constant:14],
    [stack.bottomAnchor constraintEqualToAnchor:_importBanner.bottomAnchor constant:-14],
  ]];
}

#pragma mark - Appearance

- (void)viewWillAppear:(BOOL)animated
{
  [super viewWillAppear:animated];
  // Picks up discs dropped in through Files while the app was backgrounded.
  [[DBLibrary shared] reload];
  [self refresh];
}

- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];

  // Test hook: DOLBUNDLER_AUTOPLAY=<disc id> starts that game straight away.
  // There is no way to script a tap on the simulator, and every boot bug so
  // far has needed a build-install-launch cycle to see. Reading an environment
  // variable keeps the whole loop non-interactive.
  static BOOL autoplay_done = NO;
  if (autoplay_done)
    return;

  // The UI preview walks into the first game's screen the same way, without
  // starting it. See DBSettings.uiPreviewMode.
  NSString* preview = DBSettings.uiPreviewMode;
  if ([preview isEqualToString:@"settings"])
  {
    autoplay_done = YES;
    [self showSettings];
    return;
  }
  if (preview && ![preview isEqualToString:@"library"] && DBLibrary.shared.games.count)
  {
    autoplay_done = YES;
    [self play:DBLibrary.shared.games.firstObject];
    return;
  }

  const char* autoplay = getenv("DOLBUNDLER_AUTOPLAY");
  if (!autoplay || !*autoplay)
    return;
  autoplay_done = YES;

  NSString* wanted = @(autoplay);
  for (DBGameEntry* entry in DBLibrary.shared.games)
  {
    if (![entry.discID isEqualToString:wanted])
      continue;
    if (!entry.playable)
    {
      NSLog(@"DOLBUNDLER_AUTOPLAY=%@ but this build has no native module for it", wanted);
      return;
    }
    [self play:entry];
    return;
  }
  NSLog(@"DOLBUNDLER_AUTOPLAY=%@ but no such game in the library", wanted);
}

- (void)refresh
{
  [_collection reloadData];
  const BOOL empty = DBLibrary.shared.games.count == 0;
  _emptyState.hidden = !empty || _importing;
  _collection.hidden = empty;
}

- (void)pulledToRefresh:(UIRefreshControl*)control
{
  [[DBLibrary shared] reload];
  [self refresh];
  [control endRefreshing];
}

#pragma mark - Settings

- (void)showSettings
{
  UINavigationController* nav = [[UINavigationController alloc]
      initWithRootViewController:[[DBSettingsViewController alloc] init]];
  nav.navigationBar.tintColor = DBTheme.accent;
  // A sheet rather than a push: the settings are a side trip from the
  // library, not a place in it, and a sheet comes back to the grid with one
  // swipe.
  UISheetPresentationController* sheet = nav.sheetPresentationController;
  sheet.detents = @[ UISheetPresentationControllerDetent.mediumDetent,
                     UISheetPresentationControllerDetent.largeDetent ];
  sheet.prefersGrabberVisible = YES;
  [self presentViewController:nav animated:YES completion:nil];
}

#pragma mark - Import

- (void)pickDisc
{
  if (_importing)
  {
    [self showError:@"One disc is already being imported. Wait for it to finish before adding "
                    @"another."
              title:@"Import in progress"];
    return;
  }

  // GameCube ISOs have no registered UTI, so the picker is opened on plain
  // data and the file is validated by reading its disc header instead.
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ]
                                                                 asCopy:NO];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
  if (urls.firstObject)
    [self importDiscAtURL:urls.firstObject];
}

- (void)importDiscAtURL:(NSURL*)url
{
  if (_importing)
    return;

  if (DBLibrary.shared.availableBytes < kMinimumFreeBytes)
  {
    [self showError:@"There is not enough free space to import a disc. A GameCube game needs "
                    @"about 3 GB while it is being extracted."
              title:@"Not enough space"];
    return;
  }

  // A file picked in place lives outside the container, so access has to be
  // claimed explicitly and held for the whole read.
  const BOOL scoped = [url startAccessingSecurityScopedResource];
  NSString* path = url.path;

  // Probing before extracting costs a 0x440-byte read and buys two things: the
  // banner can name the game rather than the file, and a disc that is not a
  // GameCube image at all is rejected in a moment instead of after a minute of
  // pointless extraction.
  NSString* probeError = nil;
  DBGameEntry* probed = [DBLibrary.shared probeDiscAtPath:path error:&probeError];
  if (!probed)
  {
    if (scoped)
      [url stopAccessingSecurityScopedResource];
    [self showError:probeError title:@"Not a disc this build can read"];
    return;
  }

  _importing = YES;
  _importWatchPath = probed.gameRoot;
  _importEstimate = [DBLibrary.shared estimatedExtractedBytesForDiscAtPath:path];
  [self showImportBannerForTitle:probed.title];

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* error = nil;
    DBGameEntry* entry = [DBLibrary.shared
        importDiscAtPath:path
                progress:^(NSString* stage) {
                  dispatch_async(dispatch_get_main_queue(), ^{
                    self->_importTitle.text =
                        [NSString stringWithFormat:@"%@ %@", stage, probed.title];
                  });
                }
                   error:&error];

    if (scoped)
      [url stopAccessingSecurityScopedResource];

    dispatch_async(dispatch_get_main_queue(), ^{
      [self finishImportWithEntry:entry error:error];
    });
  });
}

- (void)showImportBannerForTitle:(NSString*)title
{
  _importTitle.text = [NSString stringWithFormat:@"Reading %@", title];
  // Named up front rather than left blank until the first poll lands: walking
  // the growing game root takes a moment on a real disc, and an empty line
  // under a stalled bar is the part that reads as a hang.
  _importDetail.text = @"Extracting to the library · this can take a few minutes";
  _importProgress.progress = 0;
  _emptyState.hidden = YES;

  _importBanner.hidden = NO;
  _importBanner.alpha = 0;
  _importBanner.transform = CGAffineTransformMakeTranslation(0, 20);
  [UIView animateWithDuration:0.25
                   animations:^{
                     self->_importBanner.alpha = 1;
                     self->_importBanner.transform = CGAffineTransformIdentity;
                   }];

  // Keep the banner from covering the last row of tiles for as long as it is
  // on screen.
  _collection.contentInset = UIEdgeInsetsMake(0, 0, 96, 0);
  _collection.verticalScrollIndicatorInsets = _collection.contentInset;

  _importTimer = [NSTimer scheduledTimerWithTimeInterval:kImportPollInterval
                                                  target:self
                                                selector:@selector(pollImportProgress)
                                                userInfo:nil
                                                 repeats:YES];
}

- (void)pollImportProgress
{
  NSString* watched = _importWatchPath;
  const unsigned long long estimate = _importEstimate;
  if (!watched)
    return;

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    const unsigned long long written = [DBLibrary.shared bytesOnDiskAt:watched];
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!self->_importing)
        return;

      NSString* writtenText =
          [NSByteCountFormatter stringFromByteCount:(long long)written
                                         countStyle:NSByteCountFormatterCountStyleFile];
      self->_importDetail.text =
          [NSString stringWithFormat:@"%@ extracted · this can take a few minutes",
                                     writtenText];


      // Clamped short of the end: the estimate is derived from the image on
      // disk and is not exact, and a bar that sits at 100% while the extractor
      // is still working reads as a hang.
      const double fraction = estimate ? (double)written / (double)estimate : 0.0;
      [self->_importProgress setProgress:(float)MIN(0.97, fraction) animated:YES];
    });
  });
}

- (void)finishImportWithEntry:(DBGameEntry*)entry error:(NSString*)error
{
  [_importTimer invalidate];
  _importTimer = nil;
  _importing = NO;
  _importWatchPath = nil;

  [_importProgress setProgress:1.0 animated:YES];
  [UIView animateWithDuration:0.22
      animations:^{
        self->_importBanner.alpha = 0;
        self->_importBanner.transform = CGAffineTransformMakeTranslation(0, 20);
      }
      completion:^(BOOL finished) {
        self->_importBanner.hidden = YES;
        self->_importBanner.transform = CGAffineTransformIdentity;
        self->_collection.contentInset = UIEdgeInsetsZero;
        self->_collection.verticalScrollIndicatorInsets = UIEdgeInsetsZero;

        [self refresh];
        if (!entry)
          [self showError:error title:@"Import failed"];
        else if (!entry.playable)
          [self explainUnplayable:entry importedJustNow:YES];
      }];
}

#pragma mark - Alerts

- (void)showError:(NSString*)message title:(NSString*)title
{
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:message ?: @"Something went wrong."
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                            style:UIAlertActionStyleDefault
                                          handler:nil]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)explainUnplayable:(DBGameEntry*)entry importedJustNow:(BOOL)justImported
{
  NSString* message = [NSString
      stringWithFormat:@"%@ imported, but this build of DolBundler has no native module for %@.\n\n"
                       @"Games are recompiled on a Mac and linked into the app before it is "
                       @"signed, so a disc has to be built in to be played. The import is still "
                       @"here — a build that includes %@ will pick it up.",
                       entry.displayTitle, entry.discID, entry.discID];
  if (!justImported)
  {
    message = [NSString
        stringWithFormat:@"This build of DolBundler has no native module for %@.\n\nGames are "
                         @"recompiled on a Mac and linked into the app before it is signed, so a "
                         @"disc has to be built in to be played.",
                         entry.discID];
  }

  [self showError:message title:@"Cannot play this game"];
}

#pragma mark - Collection view

- (NSInteger)collectionView:(UICollectionView*)collectionView
     numberOfItemsInSection:(NSInteger)section
{
  return DBLibrary.shared.games.count;
}

- (UICollectionViewCell*)collectionView:(UICollectionView*)collectionView
                 cellForItemAtIndexPath:(NSIndexPath*)indexPath
{
  DBGameCell* cell = [collectionView dequeueReusableCellWithReuseIdentifier:DBGameCell.reuseIdentifier
                                                              forIndexPath:indexPath];
  [cell configureWithEntry:DBLibrary.shared.games[indexPath.item]];
  return cell;
}

- (void)collectionView:(UICollectionView*)collectionView
    didSelectItemAtIndexPath:(NSIndexPath*)indexPath
{
  [collectionView deselectItemAtIndexPath:indexPath animated:YES];
  DBGameEntry* entry = DBLibrary.shared.games[indexPath.item];
  if (!entry.playable)
  {
    [self explainUnplayable:entry importedJustNow:NO];
    return;
  }
  [self play:entry];
}

- (UIContextMenuConfiguration*)collectionView:(UICollectionView*)collectionView
    contextMenuConfigurationForItemAtIndexPath:(NSIndexPath*)indexPath
                                         point:(CGPoint)point
{
  if (indexPath.item >= (NSInteger)DBLibrary.shared.games.count)
    return nil;
  DBGameEntry* entry = DBLibrary.shared.games[indexPath.item];

  return [UIContextMenuConfiguration
      configurationWithIdentifier:@(indexPath.item)
                  previewProvider:^UIViewController*() {
                    return [[DBGamePreviewController alloc] initWithEntry:entry];
                  }
                   actionProvider:^UIMenu*(NSArray<UIMenuElement*>* suggested) {
                     NSMutableArray<UIMenuElement*>* actions = [NSMutableArray array];

                     if (entry.playable)
                     {
                       [actions addObject:[UIAction actionWithTitle:@"Play"
                                                              image:[UIImage systemImageNamed:@"play.fill"]
                                                         identifier:nil
                                                            handler:^(UIAction* action) {
                                                              [self play:entry];
                                                            }]];
                     }

                     // Deleting a game means re-extracting from an ISO that
                     // may not be on the device any more, so it asks first
                     // even though a swipe-to-delete never used to.
                     UIAction* remove = [UIAction
                         actionWithTitle:@"Delete"
                                   image:[UIImage systemImageNamed:@"trash"]
                              identifier:nil
                                 handler:^(UIAction* action) {
                                   [self confirmDelete:entry];
                                 }];
                     remove.attributes = UIMenuElementAttributesDestructive;
                     [actions addObject:remove];

                     return [UIMenu menuWithTitle:@"" children:actions];
                   }];
}

// Tapping the lifted preview is the same as tapping the card.
- (void)collectionView:(UICollectionView*)collectionView
    willPerformPreviewActionForMenuWithConfiguration:(UIContextMenuConfiguration*)configuration
                                            animator:(id<UIContextMenuInteractionCommitAnimating>)animator
{
  const NSInteger item = [(NSNumber*)configuration.identifier integerValue];
  if (item < 0 || item >= (NSInteger)DBLibrary.shared.games.count)
    return;
  DBGameEntry* entry = DBLibrary.shared.games[item];
  animator.preferredCommitStyle = UIContextMenuInteractionCommitStyleDismiss;
  [animator addCompletion:^{
    if (entry.playable)
      [self play:entry];
    else
      [self explainUnplayable:entry importedJustNow:NO];
  }];
}

- (void)confirmDelete:(DBGameEntry*)entry
{
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:@"Delete %@?", entry.displayTitle]
                       message:@"The extracted game is removed from this device. The original "
                               @"disc image is not touched."
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Delete"
                                            style:UIAlertActionStyleDestructive
                                          handler:^(UIAlertAction* action) {
                                            NSString* error = nil;
                                            if (![DBLibrary.shared deleteGame:entry error:&error])
                                              [self showError:error title:@"Could not delete"];
                                            [self refresh];
                                          }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)play:(DBGameEntry*)entry
{
  DBGameViewController* game = [[DBGameViewController alloc] initWithGame:entry];
  game.modalPresentationStyle = UIModalPresentationFullScreen;
  [self presentViewController:game animated:YES completion:nil];
}

@end
