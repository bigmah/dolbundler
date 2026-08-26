// SPDX-License-Identifier: GPL-3.0-or-later
//
// The import half of DolBundler, as a library.
//
// On macOS this work is done by `recompgc`, a shell script that drives four
// separate binaries. iOS has no shell and cannot spawn a process at all, so
// the same pipeline is exposed here as plain C the app calls in-process:
//
//     ISO --(disc_extract)--> game root
//
// That is the whole of it on iOS. Recompilation happens on a Mac, before the
// app is signed: iOS will not map a page executable unless a valid signature
// backs it, so nothing produced on the phone could ever be run. A game is
// playable here only if this build was linked against a native module for its
// disc ID -- ask db_has_native_module() in dolbundler_run.h.

#ifndef DOLBUNDLER_CORE_H
#define DOLBUNDLER_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DB_DISC_ID_SIZE 7    // 6 characters plus NUL
#define DB_TITLE_SIZE   65   // 64 characters plus NUL
#define DB_PATH_SIZE    1024

typedef struct
{
  char disc_id[DB_DISC_ID_SIZE];
  char title[DB_TITLE_SIZE];
  char game_root[DB_PATH_SIZE];  // holds sys/ and files/
} DBGame;

typedef enum
{
  DB_STAGE_PROBING,
  DB_STAGE_EXTRACTING,
  DB_STAGE_DONE,
} DBStage;

// Called on the importing thread, not the main thread.
typedef void (*DBProgressFn)(DBStage stage, const char* detail, void* ctx);

// Read a disc's identity without extracting anything. Cheap: it touches only
// the first 0x440 bytes. game_root is left empty.
// Returns 1 on success, 0 on failure with a message written to err.
int db_probe_iso(const char* iso_path, DBGame* game, char* err, size_t err_size);

// Extract `iso_path` to <library_dir>/games/<disc-id> and fill in `game`.
//
// Skipped if the extracted disc is already present, so this is safe to call
// again on a partially imported disc.
//
// `progress` may be NULL. Returns 1 on success, 0 on failure with a message
// written to err.
int db_import_iso(const char* iso_path, const char* library_dir, DBProgressFn progress, void* ctx,
                  DBGame* game, char* err, size_t err_size);

// Fill in the paths an already-imported disc would occupy, without touching
// the filesystem. Used to rebuild a library entry from a disc id.
void db_paths_for(const char* library_dir, const char* disc_id, DBGame* game);

// 1 if the extracted game root is on disk. Says nothing about whether the game
// can be played -- that depends on the build, not the disc.
int db_is_imported(const DBGame* game);

#ifdef __cplusplus
}
#endif

#endif  // DOLBUNDLER_CORE_H
