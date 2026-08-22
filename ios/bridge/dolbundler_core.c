// SPDX-License-Identifier: GPL-3.0-or-later

#include "dolbundler_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "app/cli.h"
#include "app/pipeline.h"
#include "backend/emitter.h"
#include "frontend/container/disc_extract.h"
#include "frontend/container/dol.h"
#include "vm/dolvm.h"

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
  snprintf(game->module_path, sizeof(game->module_path), "%s/modules/%s.dvm", library_dir, disc_id);
  snprintf(game->disc_id, sizeof(game->disc_id), "%s", disc_id);
}

int db_is_imported(const DBGame* game)
{
  char dol[DB_PATH_SIZE];
  snprintf(dol, sizeof(dol), "%s/sys/main.dol", game->game_root);
  return path_exists(dol) && path_exists(game->module_path);
}

int db_module_is_current(const DBGame* game)
{
  FILE* f = fopen(game->module_path, "rb");
  if (!f)
    return 0;
  DolVMHeader header;
  const size_t read = fread(&header, 1, sizeof(header), f);
  fclose(f);
  if (read != sizeof(header))
    return 0;
  // The same checks the loader makes before it will run a module; a module
  // that fails them here would fail them at boot, so it is rebuilt instead.
  return memcmp(header.magic, DOLVM_MAGIC, DOLVM_MAGIC_SIZE) == 0 &&
         header.version == DOLVM_VERSION && header.abi_version == DOLVM_ABI_VERSION;
}

// The recompile half of an import: main.dol out of the extracted game, bytecode
// into the module path. Whatever module was there is replaced.
static int recompile_module(DBGame* game, DBProgressFn progress, void* ctx, char* err,
                            size_t err_size)
{
  char dol_path[DB_PATH_SIZE];
  snprintf(dol_path, sizeof(dol_path), "%s/sys/main.dol", game->game_root);

  if (progress)
    progress(DB_STAGE_RECOMPILING, game->title, ctx);

  DOLFile dol;
  if (!dol_load(&dol, dol_path))
  {
    set_err(err, err_size, "Could not read main.dol from the extracted disc.");
    return 0;
  }

  remove(game->module_path);

  // make_module_path() swaps the extension, so handing it the final path
  // with ".dvm" already on it lands the module exactly where we want it.
  // jobs=1: the vm backend is single threaded, and the arg only ever fed
  // the C backend's split output.
  const int ok = emit_dol_split(&dol, game->module_path, DOLRECOMP_CPU_GEKKO, 1,
                                /*local_chunks_dir=*/0, /*symbols=*/NULL,
                                DOLRECOMP_BACKEND_VM, game->disc_id);
  dol_free(&dol);

  if (!ok || !path_exists(game->module_path))
  {
    set_err(err, err_size, "Recompiling %s to bytecode failed.", game->disc_id);
    return 0;
  }
  return 1;
}

int db_rebuild_module(DBGame* game, DBProgressFn progress, void* ctx, char* err, size_t err_size)
{
  char modules_dir[DB_PATH_SIZE];
  snprintf(modules_dir, sizeof(modules_dir), "%s", game->module_path);
  char* slash = strrchr(modules_dir, '/');
  if (slash)
  {
    *slash = '\0';
    if (!make_dirs(modules_dir))
    {
      set_err(err, err_size, "Could not create the module directory.");
      return 0;
    }
  }
  if (!recompile_module(game, progress, ctx, err, err_size))
    return 0;
  if (progress)
    progress(DB_STAGE_DONE, game->title, ctx);
  return 1;
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
  char modules_dir[DB_PATH_SIZE];
  snprintf(games_dir, sizeof(games_dir), "%s/games", library_dir);
  snprintf(modules_dir, sizeof(modules_dir), "%s/modules", library_dir);
  if (!make_dirs(games_dir) || !make_dirs(modules_dir))
  {
    set_err(err, err_size, "Could not create the library directory.");
    return 0;
  }

  // --- extract -----------------------------------------------------------
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

  // --- recompile ---------------------------------------------------------
  // A module from an older build of the interpreter is rebuilt rather than
  // kept: the loader would refuse it at boot.
  if (!path_exists(game->module_path) || !db_module_is_current(game))
  {
    if (!recompile_module(game, progress, ctx, err, err_size))
      return 0;
  }

  if (progress)
    progress(DB_STAGE_DONE, game->title, ctx);
  return 1;
}
