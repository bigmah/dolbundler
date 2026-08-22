// SPDX-License-Identifier: GPL-3.0-or-later
//
// The import half of DolBundler, as a library.
//
// On macOS this work is done by `recompgc`, a shell script that drives four
// separate binaries. iOS has no shell and cannot spawn a process at all, so
// the same pipeline is exposed here as plain C the app calls in-process:
//
//     ISO --(disc_extract)--> game root --(DolRecomp vm backend)--> .dvm
//
// Nothing in this path generates machine code. The .dvm is bytecode that the
// DolVM interpreter reads as data, which is what makes the whole thing legal
// to ship on the App Store.

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
  char game_root[DB_PATH_SIZE];    // holds sys/ and files/
  char module_path[DB_PATH_SIZE];  // the .dvm
} DBGame;

typedef enum
{
  DB_STAGE_PROBING,
  DB_STAGE_EXTRACTING,
  DB_STAGE_RECOMPILING,
  DB_STAGE_DONE,
} DBStage;

// Called on the importing thread, not the main thread.
typedef void (*DBProgressFn)(DBStage stage, const char* detail, void* ctx);

// Read a disc's identity without extracting anything. Cheap: it touches only
// the first 0x440 bytes. game_root and module_path are left empty.
// Returns 1 on success, 0 on failure with a message written to err.
int db_probe_iso(const char* iso_path, DBGame* game, char* err, size_t err_size);

// Extract `iso_path` to <library_dir>/games/<disc-id>, recompile its main.dol
// to <library_dir>/modules/<disc-id>.dvm, and fill in `game`.
//
// Both steps are skipped if their output is already present, so this is safe
// to call again on a partially imported disc.
//
// `progress` may be NULL. Returns 1 on success, 0 on failure with a message
// written to err.
int db_import_iso(const char* iso_path, const char* library_dir, DBProgressFn progress, void* ctx,
                  DBGame* game, char* err, size_t err_size);

// Fill in the paths an already-imported disc would occupy, without touching
// the filesystem. Used to rebuild a library entry from a disc id.
void db_paths_for(const char* library_dir, const char* disc_id, DBGame* game);

// 1 if both the extracted game root and the .dvm are present on disk.
int db_is_imported(const DBGame* game);

// 1 if the .dvm was written for the bytecode ABI this build interprets. A
// module from an older build is still a complete import -- the extracted game
// is what takes the time and the space -- but has to be rebuilt before it can
// be played.
int db_module_is_current(const DBGame* game);

// Recompile an imported game's main.dol over whatever module is there.
// Blocking; seconds to a minute depending on the game. `progress` may be NULL.
// Returns 1 on success, 0 on failure with a message written to err.
int db_rebuild_module(DBGame* game, DBProgressFn progress, void* ctx, char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif  // DOLBUNDLER_CORE_H
