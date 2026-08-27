// SPDX-License-Identifier: GPL-3.0-or-later
//
// The play half of DolBundler, as a library.
//
// On macOS a game is launched by exec()ing moderngekko-run with a handful of
// flags. iOS cannot spawn a process, so the runtime is linked into the app and
// driven through the calls below instead. db_run_game() blocks for as long as
// the game runs, so it belongs on a worker thread, not the main thread.

#ifndef DOLBUNDLER_RUN_H
#define DOLBUNDLER_RUN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Control ids, mirroring ciface::Touch::ControlID. Repeated here so the app's
// Objective-C does not have to include Dolphin's C++ headers.
typedef enum
{
  DB_PAD_A = 0,
  DB_PAD_B = 1,
  DB_PAD_X = 2,
  DB_PAD_Y = 3,
  DB_PAD_Z = 4,
  DB_PAD_START = 5,
  DB_PAD_DPAD_UP = 6,
  DB_PAD_DPAD_DOWN = 7,
  DB_PAD_DPAD_LEFT = 8,
  DB_PAD_DPAD_RIGHT = 9,
  DB_PAD_L_DIGITAL = 10,
  DB_PAD_R_DIGITAL = 11,
  DB_PAD_L_ANALOG = 12,
  DB_PAD_R_ANALOG = 13,
  DB_PAD_MAIN_STICK_X = 14,
  DB_PAD_MAIN_STICK_Y = 15,
  DB_PAD_C_STICK_X = 16,
  DB_PAD_C_STICK_Y = 17,
} DBPadControl;

// Hand over the CAMetalLayer the runtime should draw into, and its contents
// scale. Must be called before db_run_game(); the layer has to outlive the run.
void db_set_render_layer(void* ca_metal_layer, float scale);

// 1 if this build was linked against a native module for `disc_id`. Games are
// recompiled on a Mac and linked in before the app is signed -- iOS will not
// map a page executable without a valid signature behind it -- so this, not
// anything on disk, is what decides whether an imported disc can be played.
int db_has_native_module(const char* disc_id);

// Boots the disc extracted at `game_root` against this build's native module
// for it and blocks until the game stops or db_request_stop() is called.
// Fails if there is no such module; ask db_has_native_module() first.
// Returns 1 on a clean exit, 0 on failure with a message written to err.
int db_run_game(const char* game_root, const char* user_dir,
                const char* title, char* err, size_t err_size);

// Asks the running game to stop. Safe to call from any thread, including when
// nothing is running.
void db_request_stop(void);

int db_is_running(void);

// Halt or resume the emulated machine without tearing it down. This is what
// the in-game menu sits on top of: a menu that leaves the game running behind
// it costs frames for nothing and, worse, lets a game advance while the
// controls that would answer it are covered by a panel.
//
// A no-op when nothing is running. Call from the main thread.
void db_set_paused(int paused);
int db_is_paused(void);

// Diagnostic only -- drives the on-screen overlay, remove with it.
// fps is presented frames per second; speed is emulation speed as a ratio, so
// 1.0 is full speed and 0.5 is half. Either may be NULL. Safe to call when
// nothing is running, in which case both come back as zero.
void db_get_performance(double* fps, double* speed);

// Feed the on-screen controls. Analog axes take -1.0..1.0, buttons 0.0 or 1.0.
// Physical controllers do not go through here -- SDL picks those up directly.
void db_set_control(DBPadControl control, double state);
void db_clear_control(DBPadControl control);

#ifdef __cplusplus
}
#endif

#endif  // DOLBUNDLER_RUN_H
