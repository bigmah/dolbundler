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

// Boots `module_path` (a .dvm) against the disc extracted at `game_root` and
// blocks until the game stops or db_request_stop() is called.
// Returns 1 on a clean exit, 0 on failure with a message written to err.
int db_run_game(const char* game_root, const char* module_path, const char* user_dir,
                const char* title, char* err, size_t err_size);

// Asks the running game to stop. Safe to call from any thread, including when
// nothing is running.
void db_request_stop(void);

int db_is_running(void);

// Feed the on-screen controls. Analog axes take -1.0..1.0, buttons 0.0 or 1.0.
// Physical controllers do not go through here -- SDL picks those up directly.
void db_set_control(DBPadControl control, double state);
void db_clear_control(DBPadControl control);

#ifdef __cplusplus
}
#endif

#endif  // DOLBUNDLER_RUN_H
