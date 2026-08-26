// SPDX-License-Identifier: GPL-3.0-or-later

#include "dolbundler_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "frontend/container/disc_extract.h"

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(err, err_size, fmt, args);
  va_end(args);
}

static int path_exists(const char* path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

// mkdir -p. The app's container always exists, so this only ever has to create
// the last couple of components.
static int make_dirs(const char* path)
{
  char buf[DB_PATH_SIZE];
  if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
    return 0;

  for (char* p = buf + 1; *p; p++)
  {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
      return 0;
    *p = '/';
  }
  return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

void db_paths_for(const char* library_dir, const char* disc_id, DBGame* game)
{
  snprintf(game->game_root, sizeof(game->game_root), "%s/games/%s", library_dir, disc_id);
  snprintf(game->disc_id, sizeof(game->disc_id), "%s", disc_id);
}

int db_is_imported(const DBGame* game)
{
  // The extracted disc is the import: it is the slow half, the large half, and
  // the one that needs the ISO back to redo. Whether the app can then play it
  // is a property of the build -- see db_has_native_module() -- and is asked
  // separately, so a game whose module was never linked in still shows up in
  // the library as the complete import it is.
  char dol[DB_PATH_SIZE];
  snprintf(dol, sizeof(dol), "%s/sys/main.dol", game->game_root);
  return path_exists(dol);
}

int db_probe_iso(const char* iso_path, DBGame* game, char* err, size_t err_size)
{
  memset(game, 0, sizeof(*game));
  if (!disc_probe_gamecube(iso_path, game->disc_id, game->title))
  {
    set_err(err, err_size,
            "That file is not a GameCube disc image this build can read. "
            "Supported: .iso, .ciso, and NKit .nkit.iso. Wii discs and .rvz "
            "are not.");
    return 0;
  }
  return 1;
}

int db_import_iso(const char* iso_path, const char* library_dir, DBProgressFn progress, void* ctx,
                  DBGame* game, char* err, size_t err_size)
{
  if (progress)
    progress(DB_STAGE_PROBING, "Reading disc header", ctx);

  if (!db_probe_iso(iso_path, game, err, err_size))
    return 0;

  db_paths_for(library_dir, game->disc_id, game);

  char games_dir[DB_PATH_SIZE];
  snprintf(games_dir, sizeof(games_dir), "%s/games", library_dir);
  if (!make_dirs(games_dir))
  {
    set_err(err, err_size, "Could not create the library directory.");
    return 0;
  }

  char dol_path[DB_PATH_SIZE];
  snprintf(dol_path, sizeof(dol_path), "%s/sys/main.dol", game->game_root);

  if (!path_exists(dol_path))
  {
    if (progress)
      progress(DB_STAGE_EXTRACTING, game->title, ctx);

    if (!disc_extract_gamecube(iso_path, game->game_root, NULL, NULL))
    {
      set_err(err, err_size, "Extracting the disc failed. It may be truncated or not a GameCube ISO.");
      return 0;
    }
    if (!path_exists(dol_path))
    {
      set_err(err, err_size, "The disc extracted but contained no sys/main.dol.");
      return 0;
    }
  }

  if (progress)
    progress(DB_STAGE_DONE, game->title, ctx);
  return 1;
}
