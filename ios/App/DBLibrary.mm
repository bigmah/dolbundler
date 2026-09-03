// SPDX-License-Identifier: GPL-3.0-or-later

#import "DBLibrary.h"

#import "DBBanner.h"
#import "DBSettings.h"

#include <cstdio>
#include <cstring>

#include "dolbundler_core.h"
// For db_has_native_module(): whether a disc can be played is a property of
// how the app was linked, not of anything the import wrote.
#include "dolbundler_run.h"

@implementation DBGameEntry

- (NSString*)bannerPath
{
  return [self.gameRoot stringByAppendingPathComponent:@"files/opening.bnr"];
}

- (NSString*)displayTitle
{
  // The banner's title when it says more than the header's, which it usually
  // does -- "Super Mario Strikers" against a header of "Mario Soccer" -- and
  // the header's when it does not: Disney's skate game banners itself as
  // "D.E.S.A.". A banner title written in capitals ("STAR FOX: ASSAULT") is
  // the publisher's box art shouting; the header is preferred for those, and
  // failing a header the capitals are folded into a name.
  DBBanner* banner = [DBBanner cachedBannerAtPath:self.bannerPath discID:self.discID];
  NSString* candidate = banner.title;
  // The header title is cached beside the disc by the import; a root without
  // it reports the disc ID, which is not a title anything should defer to.
  NSString* header = [self.title isEqualToString:self.discID] ? nil : self.title;
  if (!candidate.length)
    return self.title;
  if (header.length > candidate.length)
    return header;

  NSUInteger letters = 0, capitals = 0;
  for (NSUInteger i = 0; i < candidate.length; i++)
  {
    const unichar c = [candidate characterAtIndex:i];
    if (![NSCharacterSet.letterCharacterSet characterIsMember:c])
      continue;
    letters++;
    if ([NSCharacterSet.uppercaseLetterCharacterSet characterIsMember:c])
      capitals++;
  }
  const BOOL shouting = letters > 0 && capitals * 10 > letters * 6;
  if (!shouting)
    return candidate;
  return header.length ? header : candidate.capitalizedString;
}

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
    entry.extractedBytes = [self directorySize:entry.gameRoot];
    entry.playable = db_has_native_module(game.disc_id) != 0;
    // A preview build has no modules at all, and a library of greyed-out
    // tiles is not the screen being looked at.
    if (DBSettings.uiPreviewMode)
      entry.playable = YES;

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

- (unsigned long long)totalExtractedBytes
{
  unsigned long long total = 0;
  for (DBGameEntry* entry in _games)
    total += entry.extractedBytes;
  return total;
}

- (unsigned long long)bytesOnDiskAt:(NSString*)path
{
  return [self directorySize:path];
}

- (DBGameEntry*)probeDiscAtPath:(NSString*)path error:(NSString**)error
{
  DBGame game;
  char err[512] = {0};
  if (!db_probe_iso(path.UTF8String, &game, err, sizeof(err)))
  {
    if (error)
      *error = @(err);
    return nil;
  }

  // db_paths_for() writes into the same struct it reads the disc ID from, so
  // the ID is taken out first rather than handed back in as its own source.
  char discID[DB_DISC_ID_SIZE];
  snprintf(discID, sizeof(discID), "%s", game.disc_id);
  NSString* title = @(game.title);
  db_paths_for(_libraryDirectory.UTF8String, discID, &game);

  DBGameEntry* entry = [[DBGameEntry alloc] init];
  entry.discID = @(discID);
  entry.title = title.length ? title : entry.discID;
  entry.gameRoot = @(game.game_root);
  entry.playable = db_has_native_module(discID) != 0;
  entry.extractedBytes = 0;
  return entry;
}

- (unsigned long long)estimatedExtractedBytesForDiscAtPath:(NSString*)path
{
  // A full GameCube disc. Used as a ceiling when the image on disk cannot say
  // anything useful about what will come out of it.
  static const unsigned long long kFullGameCubeDisc = 1459978240ULL;

  NSDictionary* attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
  const unsigned long long fileSize = [attributes[NSFileSize] unsignedLongLongValue];

  // A plain .iso is a byte-for-byte image, so its own size is the closest
  // thing to a measurement available before a single file is written. A CISO
  // is compressed and its size says nothing at all, so it falls back to the
  // ceiling. NKit sits in between -- it strips padding much as extraction does
  // -- and is close enough to be worth using. Every caller clamps the result,
  // because none of these is exact and a bar that reaches the end early is
  // less alarming than one that runs off it.
  char magic[4] = {0};
  if (FILE* f = fopen(path.UTF8String, "rb"))
  {
    fread(magic, 1, sizeof(magic), f);
    fclose(f);
  }
  if (memcmp(magic, "CISO", sizeof(magic)) == 0 || fileSize == 0)
    return kFullGameCubeDisc;
  return fileSize;
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
  // Verbs, not sentences: the caller puts the game's title after this, so
  // "Extracting disc Luigi's Mansion" is what a noun here reads as.
  case DB_STAGE_PROBING:
    text = @"Reading";
    break;
  case DB_STAGE_EXTRACTING:
    text = @"Extracting";
    break;
  case DB_STAGE_DONE:
    text = @"Finishing";
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
  [self reload];
  return YES;
}

@end
