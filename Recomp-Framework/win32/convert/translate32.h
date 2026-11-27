// The translator of Recomp-Framework win32 (PE32 i386 -> C), in C: the app runs it on the player's Mac, so the
// conversion of the game needs no Python. It is the port of tools/recomp_w32.py and tools/pe32.py, and its output
// is the same, byte for byte (the files code_NNNN.c, funcs.h and tables.c of OUTDIR).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *exe;          // the Windows executable (PE32 i386)
    const char *outdir;       // the folder of the C files (made when it does not exist)
    const char *provided;     // the file of the import shims of the runtime (I_ names, one on each line): required
    const char *patches;      // optional: guest instructions replaced by C (address, then the C code)
    const char *probes;       // optional: C code logged before guest instructions (address, then the C code)
    const char *native;       // optional: guest functions replaced by native ones (address, then the function name)
    const uint32_t *only;     // optional: translate only these functions
    size_t nonly;
    int jobs;                 // worker threads; 0: one for each processor
    int abi_trim;             // reload only the volatile registers after a call (recomp_w32.py --abi-trim)
    FILE *log;                // the messages of recomp_w32.py (function counts, unsupported instructions), or NULL
    // optional: called by the worker threads after each output file (done of total)
    void (*progress)(void *ctx, int done, int total);
    void *progress_ctx;
} W32Translate;

// Translates t->exe into t->outdir. 1 on success; else 0, and why has the reason.
int w32_translate(const W32Translate *t, char *why, size_t n);
