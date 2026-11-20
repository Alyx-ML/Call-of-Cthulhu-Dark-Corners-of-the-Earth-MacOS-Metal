// The launcher window (launcher.m), adapted from the launcher of the Prototype Mac port.
//
// The window opens before the game starts. The player can:
//   - select the game folder (the window shows whether the folder can be used),
//   - change the enhancement settings (Enhancements.cfg, enhance.h),
//   - restore the default settings,
//   - start the game (Play) or close the window (quit).
#pragma once
#include <stddef.h>

// The check of a game folder: 1 when the game can start from dir; else 0, and why tells the player the problem.
typedef int (*coc_launcher_check_fn)(const char *dir, char *why, size_t n);

// Show the window and wait for the player. The folder that the window shows first is the folder of the last Play;
// when that folder cannot be used and initial can, or when there was no Play, it is initial (NULL: no folder).
// rt_data_dir must be set (the settings file is there).
// Play: the settings file has the values of the window, out has the game folder, the window remembers the folder,
// and the result is 1. A closed window or Quit: the result is 0.
int coc_launcher_run(const char *initial, coc_launcher_check_fn check, char *out, size_t n);
