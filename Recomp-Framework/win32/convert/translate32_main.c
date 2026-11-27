// translate32: the translator in C (translate32.c) as a command-line tool, with the options of tools/recomp_w32.py:
//   translate32 EXE OUTDIR --provided FILE [--patches FILE] [--probes FILE] [--native FILE] [--only A,B,...]
//               [--jobs N] [--abi-trim]
#include "translate32.h"
#include <stdlib.h>
#include <string.h>

static const char *opt(int argc, char **argv, const char *name) {
    for (int i = 3; i + 1 < argc; i++) if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: translate32 EXE OUTDIR --provided FILE [--patches FILE] [--probes FILE] [--native FILE] "
                        "[--only A,B,...] [--jobs N] [--abi-trim]\n");
        return 2;
    }
    W32Translate t = {0};
    t.exe = argv[1];
    t.outdir = argv[2];
    t.provided = opt(argc, argv, "--provided");
    t.patches = opt(argc, argv, "--patches");
    t.probes = opt(argc, argv, "--probes");
    t.native = opt(argc, argv, "--native");
    t.log = stdout;
    for (int i = 3; i < argc; i++) if (strcmp(argv[i], "--abi-trim") == 0) t.abi_trim = 1;
    const char *jobs = opt(argc, argv, "--jobs");
    if (jobs) {
        char *end;
        long v = strtol(jobs, &end, 10);
        if (*end || v < 1 || v > 64) { fprintf(stderr, "translate32: --jobs must be 1 to 64\n"); return 2; }
        t.jobs = (int)v;
    }
    uint32_t *only = NULL;
    const char *ol = opt(argc, argv, "--only");
    if (ol) {
        size_t n = 1;
        for (const char *p = ol; *p; p++) n += *p == ',';
        only = calloc(n, sizeof *only);
        if (!only) { fprintf(stderr, "translate32: out of memory\n"); return 1; }
        size_t k = 0;
        for (const char *p = ol; *p;) {
            char *end;
            unsigned long long v = strtoull(p, &end, 16);
            if (end == p || v > UINT32_MAX || (*end && *end != ',')) { fprintf(stderr, "translate32: --only: not a list of hex addresses\n"); free(only); return 2; }
            only[k++] = (uint32_t)v;
            p = *end ? end + 1 : end;
        }
        t.only = only; t.nonly = k;
    }
    if (!t.provided) { fprintf(stderr, "translate32: --provided FILE (the I_ functions of the runtime, one name on each line) is required\n"); free(only); return 2; }
    char why[1024];
    int ok = w32_translate(&t, why, sizeof why);
    free(only);
    if (!ok) { fprintf(stderr, "translate32: %s\n", why); return 1; }
    return 0;
}
