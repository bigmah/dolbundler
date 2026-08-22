// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBLibrary.h"

#include "dolbundler_core.h"

@implementation DBGameEntry
@end

@implementation DBLibrary
{
  NSMutableArray<DBGameEntry*>* _games;
}

+ (instancetype)shared
{
  static DBLibrary* shared;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    shared = [[DBLibrary alloc] init];
  });
  return shared;
}

- (instancetype)init
{
  self = [super init];
  if (!self)
    return nil;

  _games = [NSMutableArray array];

  NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  _libraryDirectory = paths.firstObject;
  _userDirectory = [_libraryDirectory stringByAppendingPathComponent:@"moderngekko"];

  NSFileManager* fm = NSFileManager.defaultManager;
  for (NSString* dir in @[ _userDirectory,
                           [_libraryDirectory stringByAppendingPathComponent:@"games"],
                           [_libraryDirectory stringByAppendingPathComponent:@"modules"] ])
  {
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  }

  [self reload];
  return self;
}

- (unsigned long long)directorySize:(NSString*)path
{
  NSFileManager* fm = NSFileManager.defaultManager;
  NSDirectoryEnumerator* e = [fm enumeratorAtPath:path];
  unsigned long long total = 0;
  for (NSString* file in e)
    total += e.fileAttributes.fileSize;
  return total;
}

- (void)reload
{
  // The filesystem is the source of truth rather than a manifest: a user who
  // deletes a game folder in Files should see it disappear here, and a stale
  // manifest entry would otherwise point at a game that will not boot.
  [_games removeAllObjects];

  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* gamesDir = [_libraryDirectory stringByAppendingPathComponent:@"games"];
  NSArray* ids = [fm contentsOfDirectoryAtPath:gamesDir error:nil];

  for (NSString* discID in [ids sortedArrayUsingSelector:@selector(compare:)])
  {
    DBGame game;
    db_paths_for(_libraryDirectory.UTF8String, discID.UTF8String, &game);
    if (!db_is_imported(&game))
      continue;

    DBGameEntry* entry = [[DBGameEntry alloc] init];
    entry.discID = discID;
    entry.gameRoot = @(game.game_root);
    entry.modulePath = @(game.module_path);
    entry.extractedBytes = [self directorySize:entry.gameRoot];
    entry.moduleStale = !db_module_is_current(&game);

    // The disc header's title is not kept anywhere after import, so it is
    // cached beside the game rather than re-read from an ISO that may be gone.
    NSString* titleFile = [entry.gameRoot stringByAppendingPathComponent:@"dolbundler-title.txt"];
    NSString* title = [NSString stringWithContentsOfFile:titleFile
                                                encoding:NSUTF8StringEncoding
                                                   error:nil];
    entry.title = title.length ? title : discID;

    [_games addObject:entry];
  }
}

- (NSArray<DBGameEntry*>*)games
{
  return [_games copy];
}

- (unsigned long long)availableBytes
{
  NSDictionary* attrs = [NSFileManager.defaultManager
      attributesOfFileSystemForPath:_libraryDirectory
                              error:nil];
  return [attrs[NSFileSystemFreeSize] unsignedLongLongValue];
}

namespace
{
// Trampoline from the C progress callback back into the Objective-C block.
void ProgressBridge(DBStage stage, const char* detail, void* ctx)
{
  void (^block)(NSString*) = (__bridge void (^)(NSString*))ctx;
  if (!block)
    return;

  NSString* text;
  switch (stage)
  {
  case DB_STAGE_PROBING:
    text = @"Reading disc";
    break;
  case DB_STAGE_EXTRACTING:
    text = @"Extracting disc";
    break;
  case DB_STAGE_RECOMPILING:
    text = @"Recompiling to bytecode";
    break;
  case DB_STAGE_DONE:
    text = @"Done";
    break;
  }
  block(text);
}
}  // namespace

- (DBGameEntry*)importDiscAtPath:(NSString*)path
                        progress:(void (^)(NSString*))progress
                           error:(NSString**)error
{
  DBGame game;
  char err[512] = {0};

  const int ok = db_import_iso(path.UTF8String, _libraryDirectory.UTF8String, ProgressBridge,
                               (__bridge void*)progress, &game, err, sizeof(err));
  if (!ok)
  {
    if (error)
      *error = @(err);
    return nil;
  }

  // Cache the title so reload() can show it without the ISO.
  NSString* titleFile =
      [@(game.game_root) stringByAppendingPathComponent:@"dolbundler-title.txt"];
  [@(game.title) writeToFile:titleFile
                  atomically:YES
                    encoding:NSUTF8StringEncoding
                       error:nil];

  [self reload];

  for (DBGameEntry* entry in _games)
  {
    if ([entry.discID isEqualToString:@(game.disc_id)])
      return entry;
  }
  if (error)
    *error = @"The disc imported but did not appear in the library.";
  return nil;
}

- (BOOL)rebuildModuleForGame:(DBGameEntry*)entry
                    progress:(void (^)(NSString*))progress
                       error:(NSString**)error
{
  DBGame game;
  db_paths_for(_libraryDirectory.UTF8String, entry.discID.UTF8String, &game);
  snprintf(game.title, sizeof(game.title), "%s", entry.title.UTF8String);
  char err[512] = {0};
  const int ok = db_rebuild_module(&game, ProgressBridge, (__bridge void*)progress, err,
                                   sizeof(err));
  if (!ok && error)
    *error = @(err);
  [self reload];
  return ok != 0;
}

- (BOOL)deleteGame:(DBGameEntry*)entry error:(NSString**)error
{
  NSFileManager* fm = NSFileManager.defaultManager;
  NSError* fsError = nil;

  if (![fm removeItemAtPath:entry.gameRoot error:&fsError] &&
      [fm fileExistsAtPath:entry.gameRoot])
  {
    if (error)
      *error = fsError.localizedDescription ?: @"Could not delete the extracted game.";
    return NO;
  }
  // A module without its game root is useless, but losing it is not fatal:
  // reload() keys off the game root, so a leftover .dvm just wastes space.
  [fm removeItemAtPath:entry.modulePath error:nil];

  [self reload];
  return YES;
}

@end
