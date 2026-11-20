// The small DLLs: winmm, advapi32, shell32, ole32/oleaut32, gdi32, imm32, iphlpapi, ws2_32, XInput, DirectInput,
// Steam (offline), and the not-yet-implemented media and graphics entry points (they fail cleanly and log).
#include "rt32.h"
#include <time.h>
#include <sys/stat.h>
#include <mach/mach_time.h>

// ---- winmm ----
SHIM(winmm_timeGetTime, 0) {
    return (uint32_t)(rt_clock_ns() / 1000000ull);
}
SHIM(winmm_timeBeginPeriod, 1) { return 0; }
SHIM(winmm_timeEndPeriod, 1) { return 0; }

// ---- advapi32: the registry is registry.c ----
SHIM(advapi32_GetUserNameA, 2) {
    const char *n = "Player";
    uint32_t cap = rd32(ARG(1)), need = (uint32_t)strlen(n) + 1;
    wr32(ARG(1), need);
    if (cap < need) return 0;   // ERROR_INSUFFICIENT_BUFFER (GetLastError is not set from here)
    memcpy(G2H(ARG(0)), n, need);
    return 1;
}

// ---- winmm multimedia timers: one host thread per timer, callbacks run as guest code on that thread ----
RtThread *rt_thread_new(uint32_t stack_size);
void rt_thread_attach(RtThread *t);
int rt_event_set(uint32_t h, int pulse);
#define NTIMERS 16
typedef struct { int used, stop, done; uint32_t delay, flags, cb, user; pthread_t t; pthread_mutex_t mu; pthread_cond_t cv; } MmTimer;
static MmTimer timers[NTIMERS];
static pthread_mutex_t timers_mu = PTHREAD_MUTEX_INITIALIZER;
static void *timer_main(void *p) {
    MmTimer *tm = p;
    uint32_t id = (uint32_t)(tm - timers) + 1;
    RtThread *t = rt_thread_new(0x40000);
    rt_thread_attach(t);
    struct timespec next; clock_gettime(CLOCK_REALTIME, &next);
    for (;;) {
        uint64_t ns = (uint64_t)next.tv_nsec + (uint64_t)tm->delay * 1000000ull;
        next.tv_sec += (time_t)(ns / 1000000000ull); next.tv_nsec = (long)(ns % 1000000000ull);
        pthread_mutex_lock(&tm->mu);
        while (!tm->stop && pthread_cond_timedwait(&tm->cv, &tm->mu, &next) == 0) { }
        int stop = tm->stop;
        pthread_mutex_unlock(&tm->mu);
        if (stop) break;
        if (tm->flags & 0x30) rt_event_set(tm->cb, (tm->flags & 0x20) != 0);   // TIME_CALLBACK_EVENT_SET / _PULSE
        else { uint32_t a[5] = {id, 0, tm->user, 0, 0}; guest_call(t->cpu, tm->cb, 5, a); }
        if (!(tm->flags & 1)) break;   // TIME_ONESHOT
        // a periodic timer that fell behind (the callback ran long) restarts its schedule rather than firing a burst
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now;
    }
    // a one-shot timer is gone once it fired: its slot is joined and reused by the next timeSetEvent
    pthread_mutex_lock(&timers_mu); tm->done = 1; pthread_mutex_unlock(&timers_mu);
    return NULL;
}
SHIM(winmm_timeSetEvent, 5) {
    uint32_t delay = ARG(0), cb = ARG(2), user = ARG(3), flags = ARG(4);
    if (!delay || !cb) return 0;
    pthread_mutex_lock(&timers_mu);
    for (int k = 0; k < NTIMERS; k++) if (timers[k].used && timers[k].done && !(timers[k].flags & 1)) { pthread_join(timers[k].t, NULL); timers[k].used = 0; }
    int i = 0; while (i < NTIMERS && timers[i].used) i++;
    if (i == NTIMERS) { pthread_mutex_unlock(&timers_mu); rt_log("[winmm] timeSetEvent: more than %d timers", NTIMERS); return 0; }
    MmTimer *tm = &timers[i];
    tm->used = 1; tm->stop = 0; tm->done = 0; tm->delay = delay; tm->flags = flags; tm->cb = cb; tm->user = user;
    pthread_mutex_init(&tm->mu, NULL); pthread_cond_init(&tm->cv, NULL);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 16u << 20);
    int err = pthread_create(&tm->t, &a, timer_main, tm);
    pthread_attr_destroy(&a);
    if (err) { tm->used = 0; pthread_mutex_unlock(&timers_mu); rt_log("[winmm] timeSetEvent: can't start the timer thread (%d)", err); return 0; }
    pthread_mutex_unlock(&timers_mu);
    if (rt_verbose) rt_log("[winmm] timeSetEvent(%u ms, %s) -> %d", delay, (flags & 1) ? "periodic" : "one-shot", i + 1);
    return (uint32_t)i + 1;
}
SHIM(winmm_timeKillEvent, 1) {
    uint32_t id = ARG(0);
    if (!id || id > NTIMERS) return 97;   // MMSYSERR_INVALPARAM
    MmTimer *tm = &timers[id - 1];
    pthread_mutex_lock(&timers_mu);
    int used = tm->used && !(tm->done && !(tm->flags & 1));   // a fired one-shot timer no longer exists
    pthread_mutex_unlock(&timers_mu);
    if (!used) return 97;
    pthread_mutex_lock(&tm->mu); tm->stop = 1; pthread_cond_signal(&tm->cv); pthread_mutex_unlock(&tm->mu);
    // the game kills timers from its own threads, never from the callback: wait so no callback runs afterwards
    if (!pthread_equal(pthread_self(), tm->t)) pthread_join(tm->t, NULL); else pthread_detach(tm->t);
    pthread_mutex_lock(&timers_mu); tm->used = 0; pthread_mutex_unlock(&timers_mu);
    return 0;
}

// ---- shell32 ----
SHIM(shell32_SHGetFolderPathA, 5) {
    // CSIDL_PERSONAL (5), CSIDL_APPDATA (0x1A), CSIDL_LOCAL_APPDATA (0x1C)...: all under the app's data folder,
    // which rt_host_path maps back (rt_game.docs_dir -> rt_data_dir)
    const char *p = rt_game.docs_dir;
    snprintf(G2H(ARG(4)), 260, "%s", p);
    mkdir(rt_data_dir, 0755);
    return 0;
}
SHIM(shell32_DragAcceptFiles, 2) { return 0; }

// ---- ole32 / oleaut32 ----
SHIM(ole32_CoInitialize, 1) { return 0; }
SHIM(ole32_CoUninitialize, 0) { return 0; }
int dshow_create(uint32_t clsid, uint32_t outer, uint32_t iid, uint32_t out, uint32_t *hr);   // dshow.c
SHIM(ole32_CoCreateInstance, 5) {   // the in-process servers the runtime provides; others: REGDB_E_CLASSNOTREG
    uint32_t hr;
    if (ARG(0) && ARG(3) && ARG(4) && dshow_create(ARG(0), ARG(1), ARG(3), ARG(4), &hr)) { if (hr) wr32(ARG(4), 0); return hr; }
    const uint8_t *g = gp(ARG(0));
    if (g) rt_log("[ole32] CoCreateInstance {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} from 0x%x -> not registered",
                  *(uint32_t *)g, *(uint16_t *)(g + 4), *(uint16_t *)(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15], rd32(c->esp));
    if (ARG(4)) wr32(ARG(4), 0);
    return 0x80040154u;
}
SHIM(ole32_CoSetProxyBlanket, 8) { return 0; }
SHIM(ole32_CoTaskMemAlloc, 1) { return gmalloc(ARG(0)); }
SHIM(ole32_CoTaskMemFree, 1) { gfree(ARG(0)); return 0; }
SHIM(oleaut32_ord2, 1) {   // SysAllocString: length-prefixed UTF-16
    size_t n = w_len(WSTR(0));
    uint32_t p = gmalloc((uint32_t)(n * 2 + 6));
    wr32(p, (uint32_t)(n * 2)); memcpy(G2H(p + 4), WSTR(0), n * 2); wr16(p + 4 + (uint32_t)n * 2, 0);
    return p + 4;
}
SHIM(oleaut32_ord6, 1) { if (ARG(0)) gfree(ARG(0) - 4); return 0; }   // SysFreeString

// ---- gdi32 / imm32 ----
SHIM(gdi32_GetStockObject, 1) { return 0x50000u + ARG(0); }
SHIM(imm32_ImmGetContext, 1) { return 0; }
SHIM(imm32_ImmReleaseContext, 2) { return 1; }
SHIM(imm32_ImmGetCompositionStringA, 4) { return 0; }
SHIM(imm32_ImmGetDefaultIMEWnd, 1) { return 0; }

// ---- networking: the game only checks for an adapter ----
SHIM(ws2_32_ord115, 2) { if (ARG(1)) { memset(G2H(ARG(1)), 0, 400); wr16(ARG(1), 0x0202); wr16(ARG(1) + 2, 0x0202); } return 0; }   // WSAStartup
SHIM(ws2_32_ord116, 0) { return 0; }                                                                                              // WSACleanup
SHIM(iphlpapi_GetAdaptersAddresses, 5) { if (ARG(4)) wr32(ARG(4), 0); return 232; }   // ERROR_NO_DATA
SHIM(iphlpapi_GetIfEntry, 1) { return 50; }   // ERROR_NOT_SUPPORTED

// ---- XInput 1.4 (ordinals): no controller yet (GameController bridge comes with the input layer) ----
SHIM(xinput1_4_ord2, 2) { return 1167; }   // XInputGetState -> ERROR_DEVICE_NOT_CONNECTED
SHIM(xinput1_4_ord3, 2) { return 1167; }   // XInputSetState
SHIM(xinput1_4_ord4, 3) { return 1167; }   // XInputGetCapabilities
SHIM(xinput1_4_ord5, 1) { return 0; }      // XInputEnable

// ---- not yet implemented: fail the way a missing component does on Windows, and say so once ----
#define NOTYET(sym, n, ret, what) SHIM(sym, n) { static int once; if (!once++) rt_log("[todo] %s: %s", #sym, what); return ret; }
NOTYET(wmvcore_WMCreateWriter, 2, 0x80004005u, "WMCreateWriter")
NOTYET(wmvcore_WMCreateProfileManager, 1, 0x80004005u, "WMCreateProfileManager")
NOTYET(avifil32_AVIFileInit, 0, 0, "AVIFileInit")
NOTYET(avifil32_AVIFileExit, 0, 0, "AVIFileExit")
NOTYET(gdiplus_GdiplusStartup, 3, 1, "GDI+ (screenshot encoding) -> GenericError")
NOTYET(gdiplus_GdiplusShutdown, 1, 0, "GdiplusShutdown")
// d3d9: d3d9_bridge.cpp (host d3d9 backends such as DXVK)

// ---- Steam: steam32.c

// ---- shell32 / ole32 (Call of Cthulhu DCoTE) ----
SHIM(shell32_SHGetSpecialFolderPathA, 4) {
    // every CSIDL maps to the documents folder, as SHGetFolderPathA does (rt_host_path: docs_dir -> rt_data_dir)
    snprintf(G2H(ARG(1)), 260, "%s", rt_game.docs_dir);
    mkdir(rt_data_dir, 0755);
    return 1;
}
SHIM(ole32_CoFreeUnusedLibraries, 0) { return 0; }

// ---- version.dll: the VS_VERSIONINFO resource of a PE file on the host ----
// GetFileVersionInfoSize/GetFileVersionInfo return the raw resource followed by an equally large area where
// VerQueryValueA puts the ANSI copies of string values, as Windows' own A functions do.
static uint8_t *read_file_all(const char *win, uint32_t *len) {
    char p[1024]; rt_host_path(win, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || n > (64l << 20)) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);
    *len = (uint32_t)n;
    return d;
}
static int in_file(uint32_t len, uint32_t off, uint32_t n) { return off <= len && n <= len - off; }
static uint32_t rd_le32(const uint8_t *d) { return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24; }
static uint16_t rd_le16(const uint8_t *d) { return (uint16_t)(d[0] | d[1] << 8); }
// the first RT_VERSION (16) resource: returns its file offset and size
static int find_version_resource(const uint8_t *d, uint32_t len, uint32_t *off, uint32_t *size) {
    if (len < 0x40 || d[0] != 'M' || d[1] != 'Z') return 0;
    uint32_t pe = rd_le32(d + 0x3C);
    if (!in_file(len, pe, 24) || memcmp(d + pe, "PE\0\0", 4)) return 0;
    uint16_t nsec = rd_le16(d + pe + 6), optsz = rd_le16(d + pe + 20);
    uint32_t opt = pe + 24;
    if (!in_file(len, opt, optsz) || optsz < 96 + 8 * 3) return 0;
    uint16_t magic = rd_le16(d + opt);
    uint32_t ddir = opt + (magic == 0x20B ? 112 : 96);
    if (!in_file(len, ddir, 8 * 3)) return 0;
    uint32_t rsrc_rva = rd_le32(d + ddir + 16);
    if (!rsrc_rva) return 0;
    uint32_t sec = opt + optsz, rsrc_off = 0, rsrc_va = 0, rsrc_raw = 0;
    for (uint16_t i = 0; i < nsec; i++) {
        uint32_t s = sec + 40u * i;
        if (!in_file(len, s, 40)) return 0;
        uint32_t va = rd_le32(d + s + 12), vs = rd_le32(d + s + 8), rs = rd_le32(d + s + 16), ro = rd_le32(d + s + 20);
        if (rsrc_rva >= va && rsrc_rva < va + (vs > rs ? vs : rs)) { rsrc_va = va; rsrc_raw = ro; rsrc_off = ro + (rsrc_rva - va); break; }
    }
    if (!rsrc_off) return 0;
    // three directory levels (type, name, language): take RT_VERSION, then the first entry of each lower level
    uint32_t dir = rsrc_off;
    for (int level = 0; level < 3; level++) {
        if (!in_file(len, dir, 16)) return 0;
        uint32_t n = (uint32_t)rd_le16(d + dir + 12) + rd_le16(d + dir + 14), found = 0, ent = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t e = dir + 16 + 8 * i;
            if (!in_file(len, e, 8)) return 0;
            if (level > 0 || rd_le32(d + e) == 16) { ent = rd_le32(d + e + 4); found = 1; break; }
        }
        if (!found) return 0;
        if (level < 2) { if (!(ent & 0x80000000u)) return 0; dir = rsrc_off + (ent & 0x7FFFFFFFu); }
        else {
            uint32_t de = rsrc_off + ent;
            if (!in_file(len, de, 16)) return 0;
            uint32_t drva = rd_le32(d + de), dsz = rd_le32(d + de + 4);
            *off = rsrc_raw + (drva - rsrc_va); *size = dsz;
            return in_file(len, *off, dsz);
        }
    }
    return 0;
}
SHIM(version_GetFileVersionInfoSizeA, 2) {
    if (ARG(1)) wr32(ARG(1), 0);
    uint32_t len = 0, off, size;
    uint8_t *d = ARG(0) ? read_file_all(STR(0), &len) : NULL;
    if (!d) { rt_log("[version] GetFileVersionInfoSizeA(\"%s\"): file not found", ARG(0) ? STR(0) : ""); return 0; }
    int ok = find_version_resource(d, len, &off, &size);
    free(d);
    if (!ok) { rt_log("[version] %s has no version resource", STR(0)); return 0; }
    return size * 2;
}
SHIM(version_GetFileVersionInfoA, 4) {
    uint32_t len = 0, off, size, cap = ARG(2), out = ARG(3);
    uint8_t *d = ARG(0) ? read_file_all(STR(0), &len) : NULL;
    if (!d) return 0;
    int ok = find_version_resource(d, len, &off, &size);
    if (ok) {
        uint32_t n = size < cap ? size : cap;
        memcpy(G2H(out), d + off, n);
        if (cap > n) memset(G2H(out + n), 0, cap - n);
    }
    free(d);
    return ok;
}
// VS_VERSIONINFO nodes: wLength, wValueLength, wType (1: text), szKey (UTF-16), padding to 4, value, children
SHIM(version_VerQueryValueA, 4) {
    uint32_t block = ARG(0), outp = ARG(2), lenp = ARG(3);
    const char *sub = ARG(1) ? STR(1) : "\\";
    uint32_t node = block, total = rd16(block);
    char part[128];
    const char *s = sub;
    while (*s == '\\') s++;
    while (*s) {   // descend one path component at a time
        size_t n = strcspn(s, "\\"); if (n >= sizeof part) return 0;
        memcpy(part, s, n); part[n] = 0; s += n; while (*s == '\\') s++;
        uint32_t nl = rd16(node), vl = rd16(node + 2), ty = rd16(node + 4);
        uint32_t k = node + 6; while (rd16(k)) k += 2; k += 2;
        uint32_t child = ((k + 3) & ~3u) + (ty == 1 ? vl * 2 : vl);
        child = (child + 3) & ~3u;
        uint32_t end = node + nl, hit = 0;
        while (child < end && child + 6 <= block + total) {
            uint32_t cl = rd16(child); if (!cl) break;
            char key[128]; w_to_utf8(G2H(child + 6), key, sizeof key);
            if (!strcasecmp(key, part)) { hit = child; break; }
            child = (child + cl + 3) & ~3u;
        }
        if (!hit) return 0;
        node = hit;
    }
    uint32_t vl = rd16(node + 2), ty = rd16(node + 4);
    uint32_t k = node + 6; while (rd16(k)) k += 2; k += 2;
    uint32_t val = (k + 3) & ~3u;
    if (!vl) return 0;
    if (ty == 1) {   // text value: an ANSI copy in the second half of the block, at the same offset
        uint32_t a = block + total + (val - block);
        char tmp[512]; w_to_utf8(G2H(val), tmp, sizeof tmp);
        for (uint32_t i = 0; tmp[i] && i < vl; i++) wr8(a + i, (uint8_t)tmp[i]);
        wr8(a + (uint32_t)strnlen(tmp, vl), 0);
        wr32(outp, a); if (lenp) wr32(lenp, (uint32_t)strnlen(tmp, vl) + 1);
    } else { wr32(outp, val); if (lenp) wr32(lenp, vl); }
    return 1;
}
