// SPDX-License-Identifier: GPL-3.0-or-later

#import <Foundation/Foundation.h>

// One imported disc: the extracted game, and whether this build can play it.
@interface DBGameEntry : NSObject
@property(nonatomic, copy) NSString* discID;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* gameRoot;
// Bytes on disk for the extracted game, so the UI can explain where the
// storage went. A GameCube disc costs well over a gigabyte extracted.
@property(nonatomic, assign) unsigned long long extractedBytes;
// This build was linked against a native module for this disc. Games are
// recompiled on a Mac and linked in before signing, so an imported disc the
// build does not cover can be stored and deleted but not played.
@property(nonatomic, assign) BOOL playable;
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

// Read a disc's identity without extracting it. Cheap -- it touches only the
// first 0x440 bytes -- and it is what lets the import banner name the game and
// watch the right directory fill up while the slow half runs.
// The returned entry describes where the disc *would* land; it is not in the
// library until importDiscAtPath: finishes.
- (DBGameEntry*)probeDiscAtPath:(NSString*)path error:(NSString**)error;

// Roughly how many bytes this image will produce once extracted, for a
// progress bar. An estimate, not a measurement -- see the implementation for
// which images it can and cannot be right about.
- (unsigned long long)estimatedExtractedBytesForDiscAtPath:(NSString*)path;

// Bytes currently on disk beneath `path`. Polled during an import, so it runs
// off the main thread.
- (unsigned long long)bytesOnDiskAt:(NSString*)path;

// Import a disc image. Blocking: call it off the main thread.
// `progress` is invoked with a short stage description.
- (DBGameEntry*)importDiscAtPath:(NSString*)path
                        progress:(void (^)(NSString* stage))progress
                           error:(NSString**)error;

- (BOOL)deleteGame:(DBGameEntry*)entry error:(NSString**)error;

// Free space on the volume backing Documents, for the pre-import check.
- (unsigned long long)availableBytes;

@end
