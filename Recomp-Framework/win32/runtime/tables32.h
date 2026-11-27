// Tables the win32 translator writes (gen/tables.c).
//
// The translated game code (gen/*.c) is a separate bundle that the app opens at the start (rt_load_game, rt32.c). The
// runtime is compiled with RT_GAME_LIBRARY: it reads the tables through rt_gt, which rt_load_game fills from the
// symbols of the bundle. gen/tables.c is compiled without RT_GAME_LIBRARY and defines the tables.
#pragma once
#include "cpu32.h"
typedef struct { uint32_t addr; gfn fn; } GFunc;
typedef struct { uint32_t slot; const char *name; gfn fn; } GImport;   // IAT slot, "dll!name", shim
typedef struct { const char *name; uint32_t va, vsize, foff, fsize, flags; } GSection;
#ifdef RT_GAME_LIBRARY
typedef struct {
    const GFunc *funcs; const GImport *imports; const GSection *sections;
    int nfuncs, nimports, nsections;
    uint32_t image_base, image_size, entry;
} RtGameTables;
extern RtGameTables rt_gt;
#define gfuncs (rt_gt.funcs)
#define ngfuncs (rt_gt.nfuncs)
#define gimports (rt_gt.imports)
#define ngimports (rt_gt.nimports)
#define gsections (rt_gt.sections)
#define ngsections (rt_gt.nsections)
#define gimage_base (rt_gt.image_base)
#define gimage_size (rt_gt.image_size)
#define gentry (rt_gt.entry)
#else
extern const GFunc gfuncs[]; extern const int ngfuncs;
extern const GImport gimports[]; extern const int ngimports;
extern const GSection gsections[]; extern const int ngsections;
extern const uint32_t gimage_base, gimage_size, gentry;
#endif
