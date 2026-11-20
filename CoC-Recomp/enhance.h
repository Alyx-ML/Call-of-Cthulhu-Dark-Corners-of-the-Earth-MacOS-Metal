// The settings file of the enhancements (enhance.c): Enhancements.cfg in the data folder (rt_data_dir), one line
// "<key>=<value>" for each setting. The game (enhance.c) and the launcher window (launcher.m) read and write it.
#pragma once
#include "rt_enh.h"
#include <stddef.h>

// A setting of the file: its key, its field in RtEnh, its largest value (the smallest is 0) and its default.
typedef struct CocEnhKey { const char *key; size_t off; int32_t max; int32_t def; } CocEnhKey;
extern const CocEnhKey coc_enh_keys[];
extern const size_t coc_enh_nkeys;

// Read the file into e: first the defaults, then the values of the file. A line that is not a valid setting is
// ignored (and logged). Returns 1 when the file does not exist or every line is valid. Returns 0 when the file
// cannot be read or has a line that is not valid; problem then tells the player the problem.
int coc_enh_read(RtEnh *e, char *problem, size_t n);
// Write the values of e to the file (a temporary file, then a rename). Returns 1 on success; on failure 0, the file
// does not change, and problem tells the player the problem.
int coc_enh_write(const RtEnh *e, char *problem, size_t n);
