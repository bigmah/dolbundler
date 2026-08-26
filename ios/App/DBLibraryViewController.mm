// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBLibraryViewController.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DBGameViewController.h"
#import "DBLibrary.h"

// A GameCube disc is ~1.35 GB as an ISO and expands to roughly the same again
// once extracted. Importing with less than this free ends in a half-written
// game root, so it is refused up front with an explanation.
static const unsigned long long kMinimumFreeBytes = 3ULL * 1024 * 1024 * 1024;

@interface DBLibraryViewController () <UITableViewDataSource,
                                       UITableViewDelegate,
                                       UIDocumentPickerDelegate>
@end

@implementation DBLibraryViewController
{
  UITableView* _table;
  UILabel* _emptyLabel;
  UIAlertController* _progressAlert;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.title = @"Library";
  self.view.backgroundColor = UIColor.systemBackgroundColor;

  self.navigationItem.rightBarButtonItem =
      [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                                                    target:self
                                                    action:@selector(pickDisc)];

  _table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleInsetGrouped];
  _table.dataSource = self;
  _table.delegate = self;
  _table.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:_table];

  _emptyLabel = [[UILabel alloc] initWithFrame:self.view.bounds];
  _emptyLabel.text = @"No games yet.\n\nTap + to add a GameCube .iso,\nor drop one into DolBundler "
                     @"in the Files app.";
  _emptyLabel.numberOfLines = 0;
  _emptyLabel.textAlignment = NSTextAlignmentCenter;
  _emptyLabel.textColor = UIColor.secondaryLabelColor;
  _emptyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  _emptyLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:_emptyLabel];
}

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
  const char* autoplay = getenv("DOLBUNDLER_AUTOPLAY");
  if (!autoplay || !*autoplay)
    return;
  autoplay_done = YES;

  NSString* wanted = @(autoplay);
  for (DBGameEntry* entry in DBLibrary.shared.games)
  {
    if (![entry.discID isEqualToString:wanted])
      continue;
    // The same two paths a tap takes. A module left over from an older
    // bytecode format has to be rebuilt first, and that is exactly the state
    // an autoplay run is usually launched in -- the interpreter has just
    // changed, which is why the run is happening at all.
    if (entry.moduleStale)
      [self rebuildModuleThenPlay:entry];
    else
      [self play:entry];
    return;
  }
  NSLog(@"DOLBUNDLER_AUTOPLAY=%@ but no such game in the library", wanted);
}

- (void)refresh
{
  [_table reloadData];
  const BOOL empty = DBLibrary.shared.games.count == 0;
  _emptyLabel.hidden = !empty;
  _table.hidden = empty;
}

#pragma mark - Import

- (void)pickDisc
{
  // GameCube ISOs have no registered UTI, so the picker is opened on plain
  // data and the file is validated by reading its disc header instead.
  UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc]
      initForOpeningContentTypes:@[ UTTypeData ]
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

  _progressAlert = [UIAlertController alertControllerWithTitle:@"Importing"
                                                       message:@"Reading disc\n\n"
                                                preferredStyle:UIAlertControllerStyleAlert];
  [self presentViewController:_progressAlert animated:YES completion:nil];

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* error = nil;
    DBGameEntry* entry = [DBLibrary.shared
        importDiscAtPath:path
                progress:^(NSString* stage) {
                  dispatch_async(dispatch_get_main_queue(), ^{
                    self->_progressAlert.message =
                        [NSString stringWithFormat:@"%@\n\nThis can take a few minutes for a "
                                                   @"large game.\n",
                                                   stage];
                  });
                }
                   error:&error];

    if (scoped)
      [url stopAccessingSecurityScopedResource];

    dispatch_async(dispatch_get_main_queue(), ^{
      [self->_progressAlert dismissViewControllerAnimated:YES
                                               completion:^{
                                                 self->_progressAlert = nil;
                                                 if (entry)
                                                   [self refresh];
                                                 else
                                                   [self showError:error title:@"Import failed"];
                                               }];
    });
  });
}

- (void)showError:(NSString*)message title:(NSString*)title
{
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:message ?: @"Something went wrong."
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
  [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark - Table

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section
{
  return DBLibrary.shared.games.count;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath
{
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:@"game"];
  if (!cell)
  {
    cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                  reuseIdentifier:@"game"];
  }

  DBGameEntry* entry = DBLibrary.shared.games[indexPath.row];
  cell.textLabel.text = entry.title;
  cell.detailTextLabel.text =
      [NSString stringWithFormat:@"%@  ·  %@%@", entry.discID,
                                 [NSByteCountFormatter
                                     stringFromByteCount:(long long)entry.extractedBytes
                                              countStyle:NSByteCountFormatterCountStyleFile],
                                 entry.moduleStale ? @"  ·  needs a quick update" : @""];
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath
{
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  DBGameEntry* entry = DBLibrary.shared.games[indexPath.row];

  if (entry.moduleStale)
  {
    [self rebuildModuleThenPlay:entry];
    return;
  }
  [self play:entry];
}

- (void)play:(DBGameEntry*)entry
{
  DBGameViewController* game = [[DBGameViewController alloc] initWithGame:entry];
  game.modalPresentationStyle = UIModalPresentationFullScreen;
  [self presentViewController:game animated:YES completion:nil];
}

// A module built by an older version of the app is recompiled in place before
// the game starts. The extracted disc is untouched, so this takes seconds.
- (void)rebuildModuleThenPlay:(DBGameEntry*)entry
{
  _progressAlert = [UIAlertController alertControllerWithTitle:@"Updating"
                                                       message:@"Recompiling to bytecode\n\n"
                                                preferredStyle:UIAlertControllerStyleAlert];
  [self presentViewController:_progressAlert animated:YES completion:nil];

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* error = nil;
    const BOOL ok = [DBLibrary.shared
        rebuildModuleForGame:entry
                    progress:^(NSString* stage) {
                      dispatch_async(dispatch_get_main_queue(), ^{
                        self->_progressAlert.message = [NSString
                            stringWithFormat:@"%@\n\nThis version of the app uses a newer "
                                             @"bytecode format, so the game is being "
                                             @"recompiled once.\n",
                                             stage];
                      });
                    }
                       error:&error];

    dispatch_async(dispatch_get_main_queue(), ^{
      [self->_progressAlert dismissViewControllerAnimated:YES
                                               completion:^{
                                                 self->_progressAlert = nil;
                                                 [self refresh];
                                                 if (!ok)
                                                 {
                                                   [self showError:error title:@"Update failed"];
                                                   return;
                                                 }
                                                 for (DBGameEntry* fresh in DBLibrary.shared.games)
                                                 {
                                                   if ([fresh.discID isEqualToString:entry.discID])
                                                   {
                                                     [self play:fresh];
                                                     return;
                                                   }
                                                 }
                                               }];
    });
  });
}

- (UISwipeActionsConfiguration*)tableView:(UITableView*)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath
{
  DBGameEntry* entry = DBLibrary.shared.games[indexPath.row];
  UIContextualAction* del = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:@"Delete"
                        handler:^(UIContextualAction* action, UIView* view,
                                  void (^done)(BOOL)) {
                          NSString* error = nil;
                          const BOOL ok = [DBLibrary.shared deleteGame:entry error:&error];
                          if (!ok)
                            [self showError:error title:@"Could not delete"];
                          [self refresh];
                          done(ok);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ del ]];
}

@end
