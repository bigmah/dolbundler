// SPDX-License-Identifier: GPL-3.0-or-later

#import <Foundation/Foundation.h>

// One imported disc: the extracted game plus the bytecode module built from it.
@interface DBGameEntry : NSObject
@property(nonatomic, copy) NSString* discID;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* gameRoot;
@property(nonatomic, copy) NSString* modulePath;
// Bytes on disk for the extracted game, so the UI can explain where the
// storage went. A GameCube disc costs well over a gigabyte extracted.
@property(nonatomic, assign) unsigned long long extractedBytes;
// The module was built by an older version of the app and has to be rebuilt
// before this one can play it. Seconds, not the minutes an import takes.
@property(nonatomic, assign) BOOL moduleStale;
@end

@interface DBLibrary : NSObject

+ (instancetype)shared;

// <Documents>. Everything lives here so it is reachable from Files.
@property(nonatomic, readonly) NSString* libraryDirectory;
// Dolphin's own user directory (configs, saves, savestates).
@property(nonatomic, readonly) NSString* userDirectory;

@property(nonatomic, readonly) NSArray<DBGameEntry*>* games;

// Rescans <Documents>/games and rebuilds the list from what is actually there,
// so deleting a folder in Files is enough to remove a game.
- (void)reload;

// Import a disc image. Blocking: call it off the main thread.
// `progress` is invoked with a short stage description.
- (DBGameEntry*)importDiscAtPath:(NSString*)path
                        progress:(void (^)(NSString* stage))progress
                           error:(NSString**)error;

// Rebuild a stale module. Blocking: call it off the main thread.
- (BOOL)rebuildModuleForGame:(DBGameEntry*)entry
                    progress:(void (^)(NSString* stage))progress
                       error:(NSString**)error;

- (BOOL)deleteGame:(DBGameEntry*)entry error:(NSString**)error;

// Free space on the volume backing Documents, for the pre-import check.
- (unsigned long long)availableBytes;

@end
