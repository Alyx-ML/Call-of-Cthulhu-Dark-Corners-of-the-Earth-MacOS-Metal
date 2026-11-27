// The conversion of the game on the player's Mac: the app makes the game bundle (rt_load_game) from the player's
// executable. The steps are:
//   1. translate the executable to C (convert/translate32.c),
//   2. compile the C files with the clang of the app (Contents/Helpers/llvm, run from a copy in the work folder),
//      with one process for each processor,
//   3. link the game bundle with the lld of the app, against the app binary (-bundle_loader) and libSystem.tbd,
//   4. move the bundle to its place (one rename, so a bundle at that path is always complete), and remove the
//      folders of other conversions.
//
// The conversion folder of the app (Contents/Resources/conversion, win32.mk) has:
//   provided.txt   the import shims of the app binary (translate32 --provided)
//   patches.txt    optional: translate32 --patches
//   native.txt     optional: translate32 --native
//   cflags         the compile arguments, one on each line (win32.mk GEN_FLAGS)
//   ldflags        the link arguments, one on each line (win32.mk GEN_LDFLAGS)
//   gen_include/   the C library headers of the translated code (-isystem)
//   runtime/       cpu32.h and tables32.h (-I)
//   clang/include/ the clang headers that the translated code uses (-resource-dir clang)
//   libSystem.tbd  the libSystem functions that the game bundle uses
#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *exe;        // the player's executable
    const char *conv_dir;   // the conversion folder of the app
    const char *llvm;       // the clang and lld of the app (one binary: `llvm clang`, `llvm ld64.lld`)
    const char *loader;     // the app binary (-bundle_loader)
    const char *out;        // the game bundle to make: <Converted>/<conversion ID>/game.bundle
    atomic_int *cancel;     // optional: nonzero stops the conversion (the result is 0, why: "cancelled")
    // optional: the part of the work that is done, 0 to 1. Called from other threads.
    void (*progress)(void *ctx, double done);
    void *progress_ctx;
    FILE *log;              // optional: the messages of the translator and of the compiler
} W32Convert;

// Makes c->out. 1 on success; else 0, why has the reason, and the files of the conversion are removed.
int w32_convert(const W32Convert *c, char *why, size_t n);
