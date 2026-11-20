// DirectShow (quartz.dll) for 32-bit guests: the Filter Graph Manager, the WM ASF Reader source filter on FFmpeg,
// a native audio renderer on CoreAudio, and the memory allocator. The game's own filters (built from the DirectShow
// base classes in its exe) join the graph through their COM interfaces, exactly as they do on Windows.
//
// The ASF Reader's output pins deliver decoded media: video as RGB32 (VIDEOINFOHEADER, bottom-up), audio as 16-bit
// stereo PCM. On Windows, IGraphBuilder::Render inserts the WMV/WMA decoder DMOs between the reader and the
// renderers; here the reader decodes, so Render connects its pins straight to the renderer the graph holds (video)
// or to the audio renderer it creates (audio). The streaming thread runs the guest renderer's IMemInputPin::Receive
// as guest code, paces frames on the host clock, and signals end of stream; the renderers' EC_COMPLETE notifications
// reach the application through IMediaEventEx when every renderer has finished.
//
// Not implemented (each logs and returns E_NOTIMPL): IGraphBuilder::RenderFile/AddSourceFilter/Connect/Reconnect/
// SetLogFile, IMediaControl::RenderFile/AddSourceFilter and the collections, the IDispatch methods, playback rates
// other than 1.0, a reference clock (SetDefaultSyncSource), and connecting the reader's audio pin to a filter other
// than the graph's audio renderer.
#include "com32.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

RtThread *rt_thread_new(uint32_t stack_size);
void rt_thread_attach(RtThread *t);
uint32_t rt_event_new(int manual, int initial);
int rt_event_set(uint32_t h, int pulse);
int rt_event_reset(uint32_t h);
void rt_event_close(uint32_t h);
void rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_ABORT 0x80004004u
#define E_FAIL 0x80004005u
#define E_UNEXPECTED 0x8000FFFFu
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define CLASS_E_NOAGGREGATION 0x80040110u
#define HR_FILE_NOT_FOUND 0x80070002u
#define VFW_E_ALREADY_CONNECTED 0x80040204u
#define VFW_E_INVALID_DIRECTION 0x80040208u
#define VFW_E_NOT_CONNECTED 0x80040209u
#define VFW_E_NO_ALLOCATOR 0x8004020Au
#define VFW_E_BADALIGN 0x8004020Eu
#define VFW_E_ALREADY_COMMITTED 0x8004020Fu
#define VFW_E_BUFFERS_OUTSTANDING 0x80040210u
#define VFW_E_NOT_COMMITTED 0x80040211u
#define VFW_E_SIZENOTSET 0x80040212u
#define VFW_E_NOT_FOUND 0x80040216u
#define VFW_E_CANNOT_CONNECT 0x80040217u
#define VFW_E_CANNOT_RENDER 0x80040218u
#define VFW_E_NOT_STOPPED 0x80040224u
#define VFW_E_WRONG_STATE 0x80040227u
#define VFW_E_TYPE_NOT_ACCEPTED 0x8004022Au
#define VFW_E_TIMEOUT 0x8004022Eu
#define VFW_E_UNKNOWN_FILE_TYPE 0x80040240u
#define VFW_E_SAMPLE_TIME_NOT_SET 0x80040249u
#define VFW_E_MEDIA_TIME_NOT_SET 0x80040251u

#define EC_COMPLETE 0x01
#define EC_ERRORABORT 0x03
#define EC_REPAINT 0x05
#define EC_CLOCK_CHANGED 0x0D
#define EC_ACTIVATE 0x13
#define EC_NEED_RESTART 0x14
#define EC_DISPLAY_CHANGED 0x16

enum { ST_STOPPED = 0, ST_PAUSED = 1, ST_RUNNING = 2 };   // FILTER_STATE / OAFilterState

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
#define QZ(x) {x, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}}
static const Guid IID_IUnknown = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const Guid IID_IPersist = {0x0000010C, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const Guid IID_IPin = QZ(0x56A86891), IID_IEnumPins = QZ(0x56A86892), IID_IEnumFilters = QZ(0x56A86893);
static const Guid IID_IBaseFilter = QZ(0x56A86895), IID_IMediaFilter = QZ(0x56A86899), IID_IMediaSample = QZ(0x56A8689A);
static const Guid IID_IMemAllocator = QZ(0x56A8689C), IID_IMemInputPin = QZ(0x56A8689D), IID_IFilterGraph = QZ(0x56A8689F);
static const Guid IID_IMediaEventSink = QZ(0x56A868A2), IID_IFileSourceFilter = QZ(0x56A868A6), IID_IGraphBuilder = QZ(0x56A868A9);
static const Guid IID_IMediaControl = QZ(0x56A868B1), IID_IMediaPosition = QZ(0x56A868B2), IID_IBasicAudio = QZ(0x56A868B3);
static const Guid IID_IMediaEvent = QZ(0x56A868B6), IID_IMediaEventEx = QZ(0x56A868C0);
static const Guid IID_IEnumMediaTypes = {0x89C31040, 0x846B, 0x11CE, {0x97, 0xD3, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};
static const Guid IID_IObjectWithSite = {0xFC4801A3, 0x2BA9, 0x11CF, {0xA2, 0x29, 0x00, 0xAA, 0x00, 0x3D, 0x73, 0x52}};
static const Guid CLSID_FilterGraph = {0xE436EBB3, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
static const Guid CLSID_FilterGraphNoThread = {0xE436EBB8, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
static const Guid CLSID_MemoryAllocator = {0x1E651CC0, 0xB199, 0x11D0, {0x82, 0x12, 0x00, 0xC0, 0x4F, 0xC3, 0x2C, 0x45}};
static const Guid CLSID_WMAsfReader = {0x187463A0, 0x5BB7, 0x11D3, {0xAC, 0xBE, 0x00, 0x80, 0xC7, 0x5E, 0x24, 0x6E}};
static const Guid CLSID_DSoundRender = {0x79376820, 0x07D0, 0x11CF, {0xA2, 0x4D, 0x00, 0x20, 0xAF, 0xD7, 0x97, 0x67}};
static const Guid MT_Video = {0x73646976, 0, 0x10, {0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
static const Guid MT_Audio = {0x73647561, 0, 0x10, {0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
static const Guid MT_Stream = {0xE436EB83, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
static const Guid ST_RGB32 = {0xE436EB7E, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
static const Guid ST_PCM = {0x00000001, 0, 0x10, {0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
static const Guid ST_Asf = {0x3DB80F90, 0x9412, 0x11D1, {0xAD, 0xED, 0x00, 0x00, 0xF8, 0x75, 0x4B, 0x99}};
static const Guid FT_VideoInfo = {0x05589F80, 0xC356, 0x11CE, {0xBF, 0x01, 0, 0xAA, 0, 0x55, 0x59, 0x5A}};
static const Guid FT_WaveFormatEx = {0x05589F81, 0xC356, 0x11CE, {0xBF, 0x01, 0, 0xAA, 0, 0x55, 0x59, 0x5A}};

static int trace = -1;
#define T(...) do { if (trace < 0) trace = getenv("RT_TRACE_DSHOW") != NULL; if (trace) rt_log("[dshow] " __VA_ARGS__); } while (0)

static int guid_at(uint32_t a, const Guid *g) { return a && !memcmp(G2H(a), g, 16); }
static void guid_log(const char *what, const Guid *g) {
    rt_log("[dshow] %s {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", what, g->d1, g->d2, g->d3,
           g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6], g->d4[7]);
}
static void wput(uint32_t dst, const char *s, uint32_t cap) {   // UTF-16 copy of an ASCII string, cap chars incl. NUL
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) wr16(dst + 2 * i, (uint8_t)s[i]);
    wr16(dst + 2 * i, 0);
}
static void wcopy(uint32_t dst, const uint16_t *s, uint32_t cap) {
    uint32_t i = 0;
    if (s) for (; s[i] && i + 1 < cap; i++) wr16(dst + 2 * i, s[i]);
    wr16(dst + 2 * i, 0);
}
static void wr_double(uint32_t a, double d) { uint64_t u; memcpy(&u, &d, 8); wr64(a, u); }
static int64_t host_now_100ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100; }

// guest COM call: slot `slot` of the object's vtable, n further 32-bit arguments
static uint32_t vc(CPU *c, uint32_t obj, int slot, int n, ...) {
    uint32_t a[10];
    a[0] = obj;
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 9; i++) a[i + 1] = va_arg(ap, uint32_t);
    va_end(ap);
    return guest_call(c, rd32(rd32(obj) + 4u * (uint32_t)slot), n + 1, a);
}
#define G_ADDREF(c, o) vc((c), (o), 1, 0)
#define G_RELEASE(c, o) vc((c), (o), 2, 0)
static uint32_t g_qi(CPU *c, uint32_t obj, const Guid *iid, uint32_t *out) {
    uint32_t tmp = gcalloc(1, 24);
    memcpy(G2H(tmp), iid, 16);
    uint32_t hr = vc(c, obj, 0, 2, tmp, tmp + 16);
    *out = (int32_t)hr >= 0 ? rd32(tmp + 16) : 0;
    gfree(tmp);
    return hr;
}

// ---------------- objects with several interfaces ----------------
// An Owner holds the reference count; each interface is a Facet (its own guest object) that forwards AddRef/Release
// to the owner and QueryInterface to the owner's table.
typedef struct Owner {
    int refs;
    void (*destroy)(struct Owner *o);
    int (*qi)(struct Owner *o, const Guid *iid, uint32_t *out);   // the guest interface, or 0
    const char *name;
} Owner;
typedef struct Facet Facet;
struct Facet {
    ComObj base; Owner *o;
    int (*qi)(Facet *f, const Guid *iid, uint32_t *out);   // set for interfaces with their own identity (pins)
};
static uint32_t own_addref(Owner *o) { return (uint32_t)__atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST); }
static uint32_t own_release(Owner *o) {
    int r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    if (r == 0) o->destroy(o);
    return (uint32_t)(r < 0 ? 0 : r);
}
static uint32_t fac_QueryInterface(CPU *c, ComObj *s, const ComMethod *m) {
    Owner *o = ((Facet *)s)->o;
    if (!M(2)) return E_POINTER;
    if (!M(1)) { wr32(M(2), 0); return E_POINTER; }
    Guid iid; memcpy(&iid, G2H(M(1)), 16);
    uint32_t out = 0;
    Facet *f = (Facet *)s;
    if (!(f->qi ? f->qi(f, &iid, &out) : o->qi(o, &iid, &out)) || !out) {
        wr32(M(2), 0);
        if (rt_verbose) { char w[96]; snprintf(w, sizeof w, "%s: no interface", o->name); guid_log(w, &iid); }
        return E_NOINTERFACE;
    }
    own_addref(o);
    wr32(M(2), out);
    return S_OK;
}
static uint32_t fac_AddRef(CPU *c, ComObj *s, const ComMethod *m) { return own_addref(((Facet *)s)->o); }
static uint32_t fac_Release(CPU *c, ComObj *s, const ComMethod *m) { return own_release(((Facet *)s)->o); }
#define FAC_IUNKNOWN {"QueryInterface", 3, fac_QueryInterface, 0, 0, 0}, {"AddRef", 1, fac_AddRef, 0, 0, 0}, {"Release", 1, fac_Release, 0, 0, 0}
#define IDISPATCH {"GetTypeInfoCount", 2, NULL, E_NOTIMPL, 0, 0}, {"GetTypeInfo", 4, NULL, E_NOTIMPL, 0, 0}, \
                  {"GetIDsOfNames", 6, NULL, E_NOTIMPL, 0, 0}, {"Invoke", 9, NULL, E_NOTIMPL, 0, 0}
static uint32_t facet_wrap(Facet *f, Owner *o, const ComClass *cls) { f->o = o; return com_wrap(&f->base, cls); }
// a pin is a COM object of its own whose reference count is its filter's
static int pin_qi(Facet *f, const Guid *iid, uint32_t *out) {
    if (!memcmp(iid, &IID_IUnknown, 16) || !memcmp(iid, &IID_IPin, 16)) { *out = f->base.guest; return 1; }
    return 0;
}
static void facet_free(Facet *f) { if (f->base.guest) { com_unwrap_free(&f->base); f->base.guest = 0; } }
#define OWNER_OF(T, s) ((T *)((Facet *)(s))->o)

// ---------------- media types ----------------
// AM_MEDIA_TYPE (72 bytes): majortype 0, subtype 16, bFixedSizeSamples 32, bTemporalCompression 36, lSampleSize 40,
// formattype 44, pUnk 60, cbFormat 64, pbFormat 68
typedef struct { int video; int width, height; int64_t frame_time; int rate; } Format;
static void mt_fill(uint32_t mt, const Format *f) {   // fills mt; pbFormat is a new CoTaskMemAlloc block
    memset(G2H(mt), 0, 72);
    if (f->video) {
        uint32_t img = (uint32_t)f->width * (uint32_t)f->height * 4u, v = gcalloc(1, 88);
        memcpy(G2H(mt), &MT_Video, 16); memcpy(G2H(mt + 16), &ST_RGB32, 16);
        wr32(mt + 32, 1); wr32(mt + 40, img); memcpy(G2H(mt + 44), &FT_VideoInfo, 16);
        wr32(mt + 64, 88); wr32(mt + 68, v);
        wr32(v + 8, (uint32_t)f->width); wr32(v + 12, (uint32_t)f->height);     // rcSource
        wr32(v + 24, (uint32_t)f->width); wr32(v + 28, (uint32_t)f->height);    // rcTarget
        wr64(v + 40, (uint64_t)f->frame_time);                                  // AvgTimePerFrame
        uint32_t bi = v + 48;                                                   // BITMAPINFOHEADER: bottom-up BI_RGB
        wr32(bi, 40); wr32(bi + 4, (uint32_t)f->width); wr32(bi + 8, (uint32_t)f->height);
        wr16(bi + 12, 1); wr16(bi + 14, 32); wr32(bi + 20, img);
    } else {
        uint32_t w = gcalloc(1, 20);
        memcpy(G2H(mt), &MT_Audio, 16); memcpy(G2H(mt + 16), &ST_PCM, 16);
        wr32(mt + 32, 1); wr32(mt + 40, 4); memcpy(G2H(mt + 44), &FT_WaveFormatEx, 16);
        wr32(mt + 64, 18); wr32(mt + 68, w);
        wr16(w, 1); wr16(w + 2, 2); wr32(w + 4, (uint32_t)f->rate); wr32(w + 8, (uint32_t)f->rate * 4u);
        wr16(w + 12, 4); wr16(w + 14, 16); wr16(w + 16, 0);
    }
}
static void mt_free_format(uint32_t mt) { uint32_t p = rd32(mt + 68); if (p) gfree(p); wr32(mt + 68, 0); wr32(mt + 64, 0); }
static int mt_matches(uint32_t mt, const Format *f) {   // QueryAccept: the major type, subtype and format type
    if (!mt) return 0;
    if (f->video) return guid_at(mt, &MT_Video) && guid_at(mt + 16, &ST_RGB32) && guid_at(mt + 44, &FT_VideoInfo);
    return guid_at(mt, &MT_Audio) && guid_at(mt + 16, &ST_PCM) && guid_at(mt + 44, &FT_WaveFormatEx);
}

// ---------------- enumerators (pins, filters, media types) ----------------
typedef struct Enum {
    Owner own; Facet f;
    Owner *src;                    // the object enumerated (kept referenced)
    int pos;
    void *ctx;                     // item-specific (the pin whose media types are listed)
    int (*item)(struct Enum *e, CPU *c, int i, uint32_t *out);   // 0 past the end; *out is AddRef'd / allocated
    const ComClass *cls;
} Enum;
static void enum_destroy(Owner *o) { Enum *e = (Enum *)o; own_release(e->src); facet_free(&e->f); free(e); }
static ComClass enumpins_cls, enumfilters_cls, enumtypes_cls;
static int enum_qi(Owner *o, const Guid *iid, uint32_t *out) {
    Enum *e = (Enum *)o;
    const Guid *own = e->cls == &enumpins_cls ? &IID_IEnumPins : e->cls == &enumfilters_cls ? &IID_IEnumFilters : &IID_IEnumMediaTypes;
    if (!memcmp(iid, &IID_IUnknown, 16) || !memcmp(iid, own, 16)) { *out = e->f.base.guest; return 1; }
    return 0;
}
static uint32_t enum_new(Owner *src, int (*item)(Enum *, CPU *, int, uint32_t *), const ComClass *cls, int pos, void *ctx) {
    Enum *e = calloc(1, sizeof *e);
    if (!e) return 0;
    e->own = (Owner){1, enum_destroy, enum_qi, cls->name};
    e->src = src; own_addref(src);
    e->item = item; e->cls = cls; e->pos = pos; e->ctx = ctx;
    return facet_wrap(&e->f, &e->own, cls);
}
// Next(ULONG n, T **items, ULONG *fetched)
static uint32_t en_Next(CPU *c, ComObj *s, const ComMethod *m) {
    Enum *e = OWNER_OF(Enum, s);
    uint32_t n = M(1), arr = M(2), fetched = M(3), got = 0;
    if (!arr || (n > 1 && !fetched)) return E_POINTER;
    while (got < n) {
        uint32_t v = 0;
        if (!e->item(e, c, e->pos, &v)) break;
        wr32(arr + 4 * got, v);
        got++; e->pos++;
    }
    if (fetched) wr32(fetched, got);
    return got == n ? S_OK : S_FALSE;
}
static uint32_t en_Skip(CPU *c, ComObj *s, const ComMethod *m) {
    Enum *e = OWNER_OF(Enum, s);
    for (uint32_t i = 0; i < M(1); i++) {
        uint32_t v = 0;
        if (!e->item(e, NULL, e->pos, &v)) return S_FALSE;   // a NULL CPU only tests that the item exists
        e->pos++;
    }
    return S_OK;
}
static uint32_t en_Reset(CPU *c, ComObj *s, const ComMethod *m) { OWNER_OF(Enum, s)->pos = 0; return S_OK; }
static uint32_t en_Clone(CPU *c, ComObj *s, const ComMethod *m) {
    Enum *e = OWNER_OF(Enum, s);
    if (!M(1)) return E_POINTER;
    uint32_t g = enum_new(e->src, e->item, e->cls, e->pos, e->ctx);
    wr32(M(1), g);
    return g ? S_OK : E_OUTOFMEMORY;
}
static const ComMethod enum_m[] = { FAC_IUNKNOWN,
    {"Next", 4, en_Next, 0, 0, 0}, {"Skip", 2, en_Skip, 0, 0, 0}, {"Reset", 1, en_Reset, 0, 0, 0}, {"Clone", 2, en_Clone, 0, 0, 0} };
static ComClass enumpins_cls = {"IEnumPins", 7, enum_m, 0, NULL};
static ComClass enumfilters_cls = {"IEnumFilters", 7, enum_m, 0, NULL};
static ComClass enumtypes_cls = {"IEnumMediaTypes", 7, enum_m, 0, NULL};

// ---------------- memory allocator and media samples ----------------
typedef struct Alloc Alloc;
typedef struct Sample {
    Owner own; Facet f;
    Alloc *a;
    uint32_t mem, buf, size;       // the allocation, the data pointer (after prefix/alignment), cbBuffer
    int32_t actual;
    int64_t t0, t1; int tvalid;    // 0 unset, 1 start only, 2 start and stop
    int64_t m0, m1; int mvalid;
    int sync, preroll, disc;
    uint32_t mt;                   // AM_MEDIA_TYPE * set by SetMediaType (CoTaskMem), handed out once by GetMediaType
    int out;                       // handed out by GetBuffer
} Sample;
#define MAXBUF 16
struct Alloc {
    Owner own; Facet f;
    pthread_mutex_t mu; pthread_cond_t cv;
    int set, committed;
    int32_t count, size, align, prefix;
    Sample *s[MAXBUF]; int ns;
    int outstanding;
};
static void sample_reset(Sample *s) {
    s->actual = s->size ? (int32_t)s->size : 0;
    s->tvalid = s->mvalid = 0; s->sync = s->preroll = s->disc = 0;
    if (s->mt) { mt_free_format(s->mt); gfree(s->mt); s->mt = 0; }
}
// a sample whose last reference goes returns to its allocator (IMemAllocator::ReleaseBuffer)
static void sample_return(Owner *o) {
    Sample *s = (Sample *)o;
    Alloc *a = s->a;
    pthread_mutex_lock(&a->mu);
    if (s->out) { s->out = 0; a->outstanding--; }
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    own_release(&a->own);   // the reference GetBuffer took for this sample
}
static int sample_qi(Owner *o, const Guid *iid, uint32_t *out) {
    // IMediaSample2 is optional: CBaseInputPin::Receive reads the properties through IMediaSample when it is absent
    if (!memcmp(iid, &IID_IUnknown, 16) || !memcmp(iid, &IID_IMediaSample, 16)) { *out = ((Sample *)o)->f.base.guest; return 1; }
    return 0;
}
#define SMP(s) OWNER_OF(Sample, s)
static uint32_t ms_GetPointer(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; wr32(M(1), SMP(s)->buf); return S_OK; }
static uint32_t ms_GetSize(CPU *c, ComObj *s, const ComMethod *m) { return SMP(s)->size; }
static uint32_t ms_GetTime(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *x = SMP(s);
    if (!x->tvalid) return VFW_E_SAMPLE_TIME_NOT_SET;
    if (M(1)) wr64(M(1), (uint64_t)x->t0);
    if (M(2)) wr64(M(2), (uint64_t)(x->tvalid == 2 ? x->t1 : x->t0 + 1));
    return x->tvalid == 2 ? S_OK : 0x00040270u;   // VFW_S_NO_STOP_TIME
}
static uint32_t ms_SetTime(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *x = SMP(s);
    if (!M(1)) { x->tvalid = 0; return S_OK; }
    x->t0 = (int64_t)rd64(M(1));
    if (M(2)) { x->t1 = (int64_t)rd64(M(2)); x->tvalid = 2; } else x->tvalid = 1;
    return S_OK;
}
static uint32_t ms_IsSyncPoint(CPU *c, ComObj *s, const ComMethod *m) { return SMP(s)->sync ? S_OK : S_FALSE; }
static uint32_t ms_SetSyncPoint(CPU *c, ComObj *s, const ComMethod *m) { SMP(s)->sync = M(1) != 0; return S_OK; }
static uint32_t ms_IsPreroll(CPU *c, ComObj *s, const ComMethod *m) { return SMP(s)->preroll ? S_OK : S_FALSE; }
static uint32_t ms_SetPreroll(CPU *c, ComObj *s, const ComMethod *m) { SMP(s)->preroll = M(1) != 0; return S_OK; }
static uint32_t ms_GetActualDataLength(CPU *c, ComObj *s, const ComMethod *m) { return (uint32_t)SMP(s)->actual; }
static uint32_t ms_SetActualDataLength(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *x = SMP(s);
    if ((int32_t)M(1) < 0 || M(1) > x->size) return 0x8004020Du;   // VFW_E_BUFFER_OVERFLOW
    x->actual = (int32_t)M(1);
    return S_OK;
}
static uint32_t ms_GetMediaType(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *x = SMP(s);
    if (!M(1)) return E_POINTER;
    wr32(M(1), x->mt);   // ownership passes to the caller (DeleteMediaType)
    uint32_t r = x->mt ? S_OK : S_FALSE;
    x->mt = 0;
    return r;
}
static uint32_t ms_SetMediaType(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *x = SMP(s);
    if (x->mt) { mt_free_format(x->mt); gfree(x->mt); x->mt = 0; }
    if (!M(1)) return S_OK;
    uint32_t mt = gmalloc(72);
    if (!mt) return E_OUTOFMEMORY;
    memcpy(G2H(mt), G2H(M(1)), 72);
    uint32_t cb = rd32(M(1) + 64), pb = rd32(M(1) + 68);
    if (cb && pb) {
        if (cb > (1u << 20)) { gfree(mt); return E_INVALIDARG; }
        uint32_t f = gmalloc(cb);
        if (!f) { gfree(mt); return E_OUTOFMEMORY; }
        memcpy(G2H(f), G2H(pb), cb);
        wr32(mt + 68, f);
    } else { wr32(mt + 64, 0); wr32(mt + 68, 0); }
    wr32(mt + 60, 0);   // pUnk is not carried
    x->mt = mt;
    return S_OK;
}
static uint32_t ms_IsDiscontinuity(CPU *c, ComObj *s, const ComMethod *m) { return SMP(s)->disc ? S_OK : S_FALSE; }
static uint32_t ms_SetDiscontinuity(CPU *c, ComObj *s, const ComMethod *m) { SMP(s)->disc = M(1) != 0; return S_OK; }
static uint32_t ms_GetMediaTime(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *x = SMP(s);
    if (!x->mvalid) return VFW_E_MEDIA_TIME_NOT_SET;
    if (M(1)) wr64(M(1), (uint64_t)x->m0);
    if (M(2)) wr64(M(2), (uint64_t)x->m1);
    return S_OK;
}
static uint32_t ms_SetMediaTime(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *x = SMP(s);
    if (!M(1)) { x->mvalid = 0; return S_OK; }
    if (!M(2)) return E_POINTER;
    x->m0 = (int64_t)rd64(M(1)); x->m1 = (int64_t)rd64(M(2)); x->mvalid = 1;
    return S_OK;
}
static const ComMethod sample_m[] = { FAC_IUNKNOWN,
    {"GetPointer", 2, ms_GetPointer, 0, 0, 0}, {"GetSize", 1, ms_GetSize, 0, 0, 0}, {"GetTime", 3, ms_GetTime, 0, 0, 0},
    {"SetTime", 3, ms_SetTime, 0, 0, 0}, {"IsSyncPoint", 1, ms_IsSyncPoint, 0, 0, 0}, {"SetSyncPoint", 2, ms_SetSyncPoint, 0, 0, 0},
    {"IsPreroll", 1, ms_IsPreroll, 0, 0, 0}, {"SetPreroll", 2, ms_SetPreroll, 0, 0, 0},
    {"GetActualDataLength", 1, ms_GetActualDataLength, 0, 0, 0}, {"SetActualDataLength", 2, ms_SetActualDataLength, 0, 0, 0},
    {"GetMediaType", 2, ms_GetMediaType, 0, 0, 0}, {"SetMediaType", 2, ms_SetMediaType, 0, 0, 0},
    {"IsDiscontinuity", 1, ms_IsDiscontinuity, 0, 0, 0}, {"SetDiscontinuity", 2, ms_SetDiscontinuity, 0, 0, 0},
    {"GetMediaTime", 3, ms_GetMediaTime, 0, 0, 0}, {"SetMediaTime", 3, ms_SetMediaTime, 0, 0, 0} };
static ComClass sample_cls = {"IMediaSample", sizeof sample_m / sizeof *sample_m, sample_m, 0, NULL};

static void alloc_free_buffers(Alloc *a) {   // with no buffer outstanding
    for (int i = 0; i < a->ns; i++) { Sample *s = a->s[i]; sample_reset(s); gfree(s->mem); facet_free(&s->f); free(s); }
    a->ns = 0;
}
static void alloc_destroy(Owner *o) {
    Alloc *a = (Alloc *)o;
    alloc_free_buffers(a);   // outstanding samples hold a reference: none are out when the count reaches zero
    facet_free(&a->f);
    pthread_mutex_destroy(&a->mu); pthread_cond_destroy(&a->cv);
    free(a);
}
static int alloc_qi(Owner *o, const Guid *iid, uint32_t *out) {
    if (!memcmp(iid, &IID_IUnknown, 16) || !memcmp(iid, &IID_IMemAllocator, 16)) { *out = ((Alloc *)o)->f.base.guest; return 1; }
    return 0;
}
static uint32_t alloc_set(Alloc *a, int32_t count, int32_t size, int32_t align, int32_t prefix) {
    if (align <= 0 || (align & (align - 1))) return VFW_E_BADALIGN;
    if (count <= 0 || size <= 0 || prefix < 0 || size > (64 << 20) || prefix > 4096 || align > 4096) return E_INVALIDARG;
    pthread_mutex_lock(&a->mu);
    uint32_t r = S_OK;
    if (a->committed) r = VFW_E_ALREADY_COMMITTED;
    else if (a->outstanding) r = VFW_E_BUFFERS_OUTSTANDING;
    else {
        if (count > MAXBUF) count = MAXBUF;
        if (a->ns && (a->size != size || a->align != align || a->prefix != prefix || a->count != count)) alloc_free_buffers(a);
        a->count = count; a->size = size; a->align = align; a->prefix = prefix; a->set = 1;
    }
    pthread_mutex_unlock(&a->mu);
    return r;
}
static uint32_t alloc_commit(Alloc *a) {
    pthread_mutex_lock(&a->mu);
    uint32_t r = S_OK;
    if (!a->set) r = VFW_E_SIZENOTSET;
    else {
        while (a->ns < a->count) {
            Sample *s = calloc(1, sizeof *s);
            uint32_t total = (uint32_t)a->size + (uint32_t)a->prefix + (uint32_t)a->align;
            uint32_t mem = s ? gmalloc(total) : 0;
            if (!mem) { free(s); r = E_OUTOFMEMORY; break; }
            s->own = (Owner){0, sample_return, sample_qi, "IMediaSample"};
            s->a = a; s->mem = mem; s->size = (uint32_t)a->size;
            s->buf = ((mem + (uint32_t)a->prefix + (uint32_t)a->align - 1) & ~((uint32_t)a->align - 1));
            if (!facet_wrap(&s->f, &s->own, &sample_cls)) { gfree(mem); free(s); r = E_OUTOFMEMORY; break; }
            a->s[a->ns++] = s;
        }
        if (r == S_OK) a->committed = 1;
    }
    pthread_mutex_unlock(&a->mu);
    return r;
}
static void alloc_decommit(Alloc *a) {
    pthread_mutex_lock(&a->mu);
    a->committed = 0;
    pthread_cond_broadcast(&a->cv);   // GetBuffer waiters fail with VFW_E_NOT_COMMITTED
    pthread_mutex_unlock(&a->mu);
}
// a free sample (one reference), waiting while none is free; `cancel` (optional) ends the wait
static uint32_t alloc_get(Alloc *a, int nowait, Sample **out, volatile int *cancel) {
    *out = NULL;
    pthread_mutex_lock(&a->mu);
    for (;;) {
        if (!a->committed) { pthread_mutex_unlock(&a->mu); return VFW_E_NOT_COMMITTED; }
        for (int i = 0; i < a->ns; i++) if (!a->s[i]->out) {
            Sample *s = a->s[i];
            s->out = 1; a->outstanding++;
            sample_reset(s);
            s->own.refs = 1;
            pthread_mutex_unlock(&a->mu);
            own_addref(&a->own);
            *out = s;
            return S_OK;
        }
        if (nowait) { pthread_mutex_unlock(&a->mu); return VFW_E_TIMEOUT; }
        if (cancel && *cancel) { pthread_mutex_unlock(&a->mu); return E_ABORT; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&a->cv, &a->mu, &ts);   // bounded so `cancel` is seen
    }
}
#define ALC(s) OWNER_OF(Alloc, s)
// SetProperties(ALLOCATOR_PROPERTIES *request, ALLOCATOR_PROPERTIES *actual): cBuffers, cbBuffer, cbAlign, cbPrefix
static uint32_t ma_SetProperties(CPU *c, ComObj *s, const ComMethod *m) {
    Alloc *a = ALC(s);
    if (!M(1) || !M(2)) return E_POINTER;
    uint32_t r = alloc_set(a, (int32_t)rd32(M(1)), (int32_t)rd32(M(1) + 4), (int32_t)rd32(M(1) + 8), (int32_t)rd32(M(1) + 12));
    if (r == S_OK) { wr32(M(2), (uint32_t)a->count); wr32(M(2) + 4, (uint32_t)a->size); wr32(M(2) + 8, (uint32_t)a->align); wr32(M(2) + 12, (uint32_t)a->prefix); }
    return r;
}
static uint32_t ma_GetProperties(CPU *c, ComObj *s, const ComMethod *m) {
    Alloc *a = ALC(s);
    if (!M(1)) return E_POINTER;
    wr32(M(1), (uint32_t)a->count); wr32(M(1) + 4, (uint32_t)a->size); wr32(M(1) + 8, (uint32_t)a->align); wr32(M(1) + 12, (uint32_t)a->prefix);
    return S_OK;
}
static uint32_t ma_Commit(CPU *c, ComObj *s, const ComMethod *m) { return alloc_commit(ALC(s)); }
static uint32_t ma_Decommit(CPU *c, ComObj *s, const ComMethod *m) { alloc_decommit(ALC(s)); return S_OK; }
// GetBuffer(IMediaSample **, REFERENCE_TIME *start, REFERENCE_TIME *end, DWORD flags): AM_GBF_NOWAIT = 4
static uint32_t ma_GetBuffer(CPU *c, ComObj *s, const ComMethod *m) {
    if (!M(1)) return E_POINTER;
    Sample *x;
    uint32_t r = alloc_get(ALC(s), (M(4) & 4) != 0, &x, NULL);
    wr32(M(1), r == S_OK ? x->f.base.guest : 0);
    if (r == S_OK && M(2)) { x->t0 = (int64_t)rd64(M(2)); x->tvalid = 1; if (M(3)) { x->t1 = (int64_t)rd64(M(3)); x->tvalid = 2; } }
    return r;
}
static uint32_t ma_ReleaseBuffer(CPU *c, ComObj *s, const ComMethod *m) {
    // our samples come back to the allocator through their own Release; this only checks the sample is ours
    Facet *f = (Facet *)com_host(M(1));
    if (!f || f->base.cls != &sample_cls) return E_INVALIDARG;
    Sample *x = (Sample *)f->o;
    return x->a == ALC(s) ? S_OK : E_INVALIDARG;
}
static const ComMethod alloc_m[] = { FAC_IUNKNOWN,
    {"SetProperties", 3, ma_SetProperties, 0, 0, 0}, {"GetProperties", 2, ma_GetProperties, 0, 0, 0},
    {"Commit", 1, ma_Commit, 0, 0, 0}, {"Decommit", 1, ma_Decommit, 0, 0, 0},
    {"GetBuffer", 5, ma_GetBuffer, 0, 0, 0}, {"ReleaseBuffer", 2, ma_ReleaseBuffer, 0, 0, 0} };
static ComClass alloc_cls = {"IMemAllocator", sizeof alloc_m / sizeof *alloc_m, alloc_m, 0, NULL};
static Alloc *alloc_new(void) {
    Alloc *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->own = (Owner){1, alloc_destroy, alloc_qi, "MemoryAllocator"};
    pthread_mutex_init(&a->mu, NULL); pthread_cond_init(&a->cv, NULL);
    if (!facet_wrap(&a->f, &a->own, &alloc_cls)) { free(a); return NULL; }
    return a;
}

// ---------------- the filters ----------------
typedef struct Graph Graph;
typedef struct Reader Reader;
typedef struct ARend ARend;
// IBaseFilter state shared by the native filters
typedef struct {
    int state;
    uint32_t graph;          // IFilterGraph * from JoinFilterGraph (not referenced, as in CBaseFilter)
    uint16_t name[128];
    uint32_t clock;          // IReferenceClock * (referenced)
} FilterBase;
static uint32_t fb_QueryFilterInfo(CPU *c, FilterBase *fb, uint32_t info) {   // FILTER_INFO: WCHAR achName[128], IFilterGraph *
    if (!info) return E_POINTER;
    wcopy(info, fb->name, 128);
    wr32(info + 256, fb->graph);
    if (fb->graph) G_ADDREF(c, fb->graph);
    return S_OK;
}
static uint32_t fb_Join(FilterBase *fb, uint32_t graph, uint32_t name) {
    fb->graph = graph;
    if (name) { const uint16_t *w = gp(name); size_t i = 0; for (; w[i] && i < 127; i++) fb->name[i] = w[i]; fb->name[i] = 0; }
    else fb->name[0] = 0;
    return S_OK;
}
static uint32_t fb_SetSyncSource(CPU *c, FilterBase *fb, uint32_t clk) {
    if (clk) G_ADDREF(c, clk);
    if (fb->clock) G_RELEASE(c, fb->clock);
    fb->clock = clk;
    return S_OK;
}
static uint32_t fb_GetSyncSource(CPU *c, FilterBase *fb, uint32_t out) {
    if (!out) return E_POINTER;
    wr32(out, fb->clock);
    if (fb->clock) G_ADDREF(c, fb->clock);
    return S_OK;
}
// IMediaEventSink::Notify through the graph the filter joined
static void fb_notify(CPU *c, FilterBase *fb, uint32_t code, uint32_t p1, uint32_t p2) {
    if (!fb->graph) return;
    uint32_t sink = 0;
    if ((int32_t)g_qi(c, fb->graph, &IID_IMediaEventSink, &sink) < 0 || !sink) return;
    vc(c, sink, 3, 3, code, p1, p2);
    G_RELEASE(c, sink);
}

// ---- audio renderer: plays the reader's PCM on CoreAudio ----
#define RING_SECONDS 2
struct ARend {
    Owner own; Facet bf;
    struct APin { Facet f; ARend *a; } in;
    FilterBase fb;
    uint32_t peer;           // the connected output pin (a reader audio pin), referenced
    int rate;
    AudioUnit unit;
    int16_t *ring; uint32_t cap;   // stereo frames
    uint64_t rd, wr;               // frames consumed / written (rd by the audio thread)
    int32_t gain_l, gain_r;        // Q15
    int32_t volume, balance;       // IBasicAudio: hundredths of a dB
};
static OSStatus ar_render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)fl; (void)ts; (void)bus;
    ARend *a = ref;
    int16_t *out = io->mBuffers[0].mData;
    uint64_t rd = __atomic_load_n(&a->rd, __ATOMIC_RELAXED), wr = __atomic_load_n(&a->wr, __ATOMIC_ACQUIRE);
    uint64_t avail = wr - rd, take = avail < n ? avail : n;
    int32_t gl = __atomic_load_n(&a->gain_l, __ATOMIC_RELAXED), gr = __atomic_load_n(&a->gain_r, __ATOMIC_RELAXED);
    for (uint64_t i = 0; i < take; i++) {
        const int16_t *s = a->ring + ((rd + i) % a->cap) * 2;
        out[i * 2] = (int16_t)((s[0] * gl) >> 15);
        out[i * 2 + 1] = (int16_t)((s[1] * gr) >> 15);
    }
    memset(out + take * 2, 0, (size_t)(n - take) * 4);
    __atomic_store_n(&a->rd, rd + take, __ATOMIC_RELEASE);
    return noErr;
}
static void ar_gains(ARend *a) {
    double v = pow(10.0, a->volume / 2000.0);
    double l = v * (a->balance > 0 ? pow(10.0, -a->balance / 2000.0) : 1.0);
    double r = v * (a->balance < 0 ? pow(10.0, a->balance / 2000.0) : 1.0);
    __atomic_store_n(&a->gain_l, (int32_t)(l * 32768.0 > 32768.0 ? 32768.0 : l * 32768.0), __ATOMIC_RELAXED);
    __atomic_store_n(&a->gain_r, (int32_t)(r * 32768.0 > 32768.0 ? 32768.0 : r * 32768.0), __ATOMIC_RELAXED);
}
static uint32_t ar_open(ARend *a, int rate) {
    a->rate = rate;
    a->cap = (uint32_t)rate * RING_SECONDS;
    a->ring = calloc((size_t)a->cap * 2, sizeof(int16_t));
    if (!a->ring) return E_OUTOFMEMORY;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &a->unit)) { rt_log("[dshow] audio renderer: no audio output device"); a->unit = NULL; return E_FAIL; }
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2; f.mFramesPerPacket = 1; f.mBitsPerChannel = 16; f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
    AURenderCallbackStruct cb = {ar_render, a};
    OSStatus e;
    if ((e = AudioUnitSetProperty(a->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) ||
        (e = AudioUnitSetProperty(a->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb)) ||
        (e = AudioUnitInitialize(a->unit)) ||
        (rt_mute && (e = AudioUnitSetParameter(a->unit, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, 0.0f, 0)))) {
        rt_log("[dshow] audio renderer: output setup failed (OSStatus %d)", (int)e);
        AudioComponentInstanceDispose(a->unit); a->unit = NULL;
        return E_FAIL;
    }
    return S_OK;
}
static void ar_flush(ARend *a) { __atomic_store_n(&a->rd, __atomic_load_n(&a->wr, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE); }
static uint32_t ar_free_frames(ARend *a) { return a->cap - (uint32_t)(__atomic_load_n(&a->wr, __ATOMIC_RELAXED) - __atomic_load_n(&a->rd, __ATOMIC_ACQUIRE)); }
static uint32_t ar_queued(ARend *a) { return (uint32_t)(__atomic_load_n(&a->wr, __ATOMIC_RELAXED) - __atomic_load_n(&a->rd, __ATOMIC_ACQUIRE)); }
static void ar_write(ARend *a, const int16_t *s, uint32_t n) {   // n <= ar_free_frames
    uint64_t wr = __atomic_load_n(&a->wr, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; i++) { int16_t *d = a->ring + ((wr + i) % a->cap) * 2; d[0] = s[i * 2]; d[1] = s[i * 2 + 1]; }
    __atomic_store_n(&a->wr, wr + n, __ATOMIC_RELEASE);
}
static void ar_set_state(ARend *a, int st) {
    if (a->unit) {
        if (st == ST_RUNNING && a->fb.state != ST_RUNNING) AudioOutputUnitStart(a->unit);
        else if (st != ST_RUNNING && a->fb.state == ST_RUNNING) AudioOutputUnitStop(a->unit);
    }
    if (st == ST_STOPPED) ar_flush(a);
    a->fb.state = st;
}
static void ar_destroy(Owner *o) {
    ARend *a = (ARend *)o;
    if (a->unit) { AudioOutputUnitStop(a->unit); AudioUnitUninitialize(a->unit); AudioComponentInstanceDispose(a->unit); }
    CPU *c = rt_cpu();
    if (a->fb.clock) G_RELEASE(c, a->fb.clock);
    if (a->peer) G_RELEASE(c, a->peer);
    free(a->ring);
    facet_free(&a->bf); facet_free(&a->in.f);
    free(a);
}
static int ar_qi(Owner *o, const Guid *iid, uint32_t *out) {
    ARend *a = (ARend *)o;
    if (!memcmp(iid, &IID_IUnknown, 16) || !memcmp(iid, &IID_IPersist, 16) || !memcmp(iid, &IID_IMediaFilter, 16) ||
        !memcmp(iid, &IID_IBaseFilter, 16)) { *out = a->bf.base.guest; return 1; }
    return 0;
}

// ---- WM ASF Reader ----
typedef struct Pin {
    Facet f;
    Reader *r;
    int video;
    uint32_t peer;           // connected input pin (IPin, referenced)
    uint32_t mem;            // its IMemInputPin (referenced; video)
    Alloc *alloc;            // the allocator agreed with it (video)
    ARend *arend;            // the audio renderer it feeds (audio)
} Pin;
typedef struct VFrame { struct VFrame *next; uint8_t *bgra; int64_t t, dur; int key; } VFrame;
struct Reader {
    Owner own; Facet bf, fsf;
    Pin pin[2]; int npins;
    FilterBase fb;
    uint16_t wpath[260];
    FILE *file;
    AVIOContext *avio;
    AVFormatContext *fmt;
    int vs, as;                    // FFmpeg stream indices, -1 when absent
    AVCodecContext *vdec, *adec;
    struct SwsContext *sws;
    SwrContext *swr;
    Format vf, af;
    int64_t duration, start100;    // 100 ns
    // streaming
    pthread_mutex_t mu; pthread_cond_t cv;
    pthread_t th; int th_live; RtThread *th_rt;
    volatile int stop_req;
    volatile int quiet;            // the graph is stopping: delivery failures are expected
    int seek_req; int64_t seek_to;
    int64_t pos;                   // stream position when not streaming (100 ns)
    int64_t stop_time;             // IMediaPosition stop time (100 ns)
    int64_t seg_base, run_t0;      // stream time = seg_base + (host - run_t0) while running
    VFrame *vq, *vq_tail; int vq_n; size_t vq_bytes;
    int64_t audio_through;         // stream time decoded audio reaches
    int eof, eos_sent, complete_sent, v_done;
    int64_t demux_at;              // stream time the demuxer was last positioned at (-1: unknown)
};
#define PIN_OF(s) ((Pin *)(s))

// ---------------- the Filter Graph Manager ----------------
#define MAXF 16
#define MAXEV 64
struct Graph {
    Owner own;
    Facet gb, mc, me, mp, ba, sink, ows;
    pthread_mutex_t mu; pthread_cond_t cv;
    struct { uint32_t filter; uint16_t name[128]; } f[MAXF]; int nf;
    int state;
    struct { uint32_t code, p1, p2; } ev[MAXEV]; int ev_head, ev_n;
    uint32_t ev_handle;
    uint32_t notify_hwnd, notify_msg, notify_inst, notify_flags;
    int default_complete;
    int renderers, completed;
    uint32_t site;
    Reader *src;             // the ASF reader in the graph (IMediaPosition)
    ARend *arend;            // the audio renderer the graph made (IBasicAudio)
};

static Reader *as_reader(uint32_t filter);
static ARend *as_arend(uint32_t filter);

static void graph_post(Graph *g, uint32_t code, uint32_t p1, uint32_t p2) {   // with g->mu held
    if (g->ev_n == MAXEV) { rt_log("[dshow] event queue full: event 0x%x dropped", code); return; }
    int i = (g->ev_head + g->ev_n++) % MAXEV;
    g->ev[i].code = code; g->ev[i].p1 = p1; g->ev[i].p2 = p2;
    if (g->ev_handle) rt_event_set(g->ev_handle, 0);
    pthread_cond_broadcast(&g->cv);
    if (g->notify_hwnd && !(g->notify_flags & 1)) rt_post_message(g->notify_hwnd, g->notify_msg, 0, g->notify_inst);
    T("event 0x%x (0x%x, 0x%x)", code, p1, p2);
}
static void graph_notify(Graph *g, uint32_t code, uint32_t p1, uint32_t p2) {
    pthread_mutex_lock(&g->mu);
    if (code == EC_COMPLETE && g->default_complete) {
        // the graph reports completion once every renderer has finished
        if (++g->completed >= g->renderers) graph_post(g, EC_COMPLETE, p1, 0);
    } else if (code == EC_REPAINT || code == EC_CLOCK_CHANGED || code == EC_ACTIVATE || code == EC_NEED_RESTART || code == EC_DISPLAY_CHANGED) {
        T("event 0x%x handled by the graph", code);   // default handling: nothing for the application
    } else graph_post(g, code, p1, p2);
    pthread_mutex_unlock(&g->mu);
}

static int graph_qi(Owner *o, const Guid *iid, uint32_t *out) {
    Graph *g = (Graph *)o;
    if (!memcmp(iid, &IID_IUnknown, 16) || !memcmp(iid, &IID_IFilterGraph, 16) || !memcmp(iid, &IID_IGraphBuilder, 16)) *out = g->gb.base.guest;
    else if (!memcmp(iid, &IID_IMediaControl, 16)) *out = g->mc.base.guest;
    else if (!memcmp(iid, &IID_IMediaEvent, 16) || !memcmp(iid, &IID_IMediaEventEx, 16)) *out = g->me.base.guest;
    else if (!memcmp(iid, &IID_IMediaPosition, 16)) *out = g->mp.base.guest;
    else if (!memcmp(iid, &IID_IBasicAudio, 16)) *out = g->ba.base.guest;
    else if (!memcmp(iid, &IID_IMediaEventSink, 16)) *out = g->sink.base.guest;
    else if (!memcmp(iid, &IID_IObjectWithSite, 16)) *out = g->ows.base.guest;
    else return 0;
    return 1;
}
#define GR(s) OWNER_OF(Graph, s)

// state changes: renderers first, then the source (DirectShow's upstream order)
static uint32_t filter_state(CPU *c, uint32_t f, int st) {
    if (st == ST_STOPPED) return vc(c, f, 4, 0);
    if (st == ST_PAUSED) return vc(c, f, 5, 0);
    return vc(c, f, 6, 2, 0u, 0u);   // Run(tStart = 0): there is no reference clock
}
static uint32_t graph_set_state(CPU *c, Graph *g, int st) {
    if (st == ST_RUNNING && g->state == ST_STOPPED) { uint32_t r = graph_set_state(c, g, ST_PAUSED); if ((int32_t)r < 0) return r; }
    if (st == ST_RUNNING) {
        pthread_mutex_lock(&g->mu);
        if (g->state != ST_RUNNING) g->completed = 0;
        int none = g->renderers == 0;
        pthread_mutex_unlock(&g->mu);
        if (none) graph_notify(g, EC_COMPLETE, S_OK, 0);
    }
    if (st == ST_STOPPED && g->src) { pthread_mutex_lock(&g->src->mu); g->src->quiet = 1; pthread_mutex_unlock(&g->src->mu); }
    uint32_t result = S_OK;
    for (int pass = 0; pass < 2; pass++)
        for (int i = g->nf - 1; i >= 0; i--) {
            int is_src = as_reader(g->f[i].filter) != NULL;
            if (is_src != (pass == 1)) continue;
            uint32_t r = filter_state(c, g->f[i].filter, st);
            if ((int32_t)r < 0) { rt_log("[dshow] filter %d: state %d failed (0x%x)", i, st, r); result = r; }
        }
    g->state = st;
    T("graph state %d", st);
    return result;
}

static void graph_destroy(Owner *o) {
    Graph *g = (Graph *)o;
    CPU *c = rt_cpu();
    if (g->state != ST_STOPPED) graph_set_state(c, g, ST_STOPPED);
    // RemoveFilter for each filter: disconnect its pins, leave the graph, release it
    for (int i = g->nf - 1; i >= 0; i--) {
        uint32_t f = g->f[i].filter, tmp = gcalloc(1, 16), en = 0;
        if ((int32_t)vc(c, f, 10, 1, tmp) >= 0 && (en = rd32(tmp))) {
            while (vc(c, en, 3, 3, 1u, tmp + 4, tmp + 8) == S_OK) {
                uint32_t pin = rd32(tmp + 4);
                wr32(tmp + 12, 0);
                if (vc(c, pin, 6, 1, tmp + 12) == S_OK && rd32(tmp + 12)) {
                    uint32_t peer = rd32(tmp + 12);
                    vc(c, peer, 5, 0); vc(c, pin, 5, 0);
                    G_RELEASE(c, peer);
                }
                G_RELEASE(c, pin);
            }
            G_RELEASE(c, en);
        }
        gfree(tmp);
    }
    for (int i = g->nf - 1; i >= 0; i--) { vc(c, g->f[i].filter, 13, 2, 0u, 0u); G_RELEASE(c, g->f[i].filter); }
    if (g->site) G_RELEASE(c, g->site);
    if (g->ev_handle) rt_event_close(g->ev_handle);
    facet_free(&g->gb); facet_free(&g->mc); facet_free(&g->me); facet_free(&g->mp); facet_free(&g->ba); facet_free(&g->sink); facet_free(&g->ows);
    pthread_mutex_destroy(&g->mu); pthread_cond_destroy(&g->cv);
    free(g);
}

static uint32_t graph_add(CPU *c, Graph *g, uint32_t filter, uint32_t name) {
    if (!filter) return E_POINTER;
    for (int i = 0; i < g->nf; i++) if (g->f[i].filter == filter) return E_INVALIDARG;   // already in the graph
    if (g->nf == MAXF) { rt_log("[dshow] more than %d filters in a graph", MAXF); return E_OUTOFMEMORY; }
    int i = g->nf;
    g->f[i].filter = filter;
    if (name) { const uint16_t *w = gp(name); size_t k = 0; for (; w[k] && k < 127; k++) g->f[i].name[k] = w[k]; g->f[i].name[k] = 0; }
    else g->f[i].name[0] = 0;
    G_ADDREF(c, filter);
    uint32_t wname = gcalloc(128, 2);
    wcopy(wname, g->f[i].name, 128);
    uint32_t r = vc(c, filter, 13, 2, g->gb.base.guest, wname);   // JoinFilterGraph
    gfree(wname);
    if ((int32_t)r < 0) { G_RELEASE(c, filter); return r; }
    g->nf++;
    Reader *rd = as_reader(filter);
    if (rd) g->src = rd;
    if (rt_verbose) { char n[128]; w_to_utf8(g->f[i].name, n, sizeof n); rt_log("[dshow] AddFilter \"%s\"%s", n, rd ? " (ASF reader)" : ""); }
    return S_OK;
}
// IFilterGraph
static uint32_t gb_AddFilter(CPU *c, ComObj *s, const ComMethod *m) { return graph_add(c, GR(s), M(1), M(2)); }
static uint32_t gb_RemoveFilter(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    for (int i = 0; i < g->nf; i++) if (g->f[i].filter == M(1)) {
        if (g->state != ST_STOPPED) return VFW_E_NOT_STOPPED;
        uint32_t f = g->f[i].filter;
        if (g->src == as_reader(f)) g->src = NULL;
        if (g->arend == as_arend(f)) g->arend = NULL;
        memmove(&g->f[i], &g->f[i + 1], sizeof g->f[0] * (size_t)(g->nf - i - 1));
        g->nf--;
        vc(c, f, 13, 2, 0u, 0u);
        G_RELEASE(c, f);
        return S_OK;
    }
    return E_INVALIDARG;
}
static int filters_item(Enum *e, CPU *c, int i, uint32_t *out) {
    Graph *g = (Graph *)e->src;
    if (i >= g->nf) return 0;
    *out = g->f[i].filter;
    if (c) G_ADDREF(c, *out);
    return 1;
}
static uint32_t gb_EnumFilters(CPU *c, ComObj *s, const ComMethod *m) {
    if (!M(1)) return E_POINTER;
    uint32_t e = enum_new(&GR(s)->own, filters_item, &enumfilters_cls, 0, NULL);
    wr32(M(1), e);
    return e ? S_OK : E_OUTOFMEMORY;
}
static uint32_t gb_FindFilterByName(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(1) || !M(2)) return E_POINTER;
    const uint16_t *w = gp(M(1));
    for (int i = 0; i < g->nf; i++) {
        size_t k = 0; while (w[k] && w[k] == g->f[i].name[k]) k++;
        if (!w[k] && !g->f[i].name[k]) { wr32(M(2), g->f[i].filter); G_ADDREF(c, g->f[i].filter); return S_OK; }
    }
    wr32(M(2), 0);
    return VFW_E_NOT_FOUND;
}
// ConnectDirect(IPin *out, IPin *in, const AM_MEDIA_TYPE *): the output pin connects itself
static uint32_t gb_ConnectDirect(CPU *c, ComObj *s, const ComMethod *m) {
    if (!M(1) || !M(2)) return E_POINTER;
    return vc(c, M(1), 3, 2, M(2), M(3));
}
static uint32_t gb_Disconnect(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; return vc(c, M(1), 5, 0); }

static uint32_t pin_connect_video(CPU *c, Pin *p, uint32_t in);
static uint32_t pin_connect_audio(CPU *c, Graph *g, Pin *p);
// Render(IPin *out): the reader's video pin to a renderer in the graph that accepts RGB32, its audio pin to a new
// audio renderer
static uint32_t gb_Render(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    Facet *f = (Facet *)com_host(M(1));
    Pin *p = NULL;
    if (f && g->src) for (int i = 0; i < g->src->npins; i++) if (&g->src->pin[i].f == f) p = &g->src->pin[i];
    if (!p) { rt_log("[dshow] Render: pin 0x%x is not an ASF reader output pin in this graph", M(1)); return VFW_E_CANNOT_RENDER; }
    if (p->peer) return VFW_E_ALREADY_CONNECTED;
    if (!p->video) {
        uint32_t r = pin_connect_audio(c, g, p);
        if (r == S_OK) { pthread_mutex_lock(&g->mu); g->renderers++; pthread_mutex_unlock(&g->mu); }
        return r;
    }
    uint32_t tmp = gcalloc(1, 16);
    uint32_t result = VFW_E_CANNOT_RENDER;
    for (int i = 0; i < g->nf && result != S_OK; i++) {
        uint32_t flt = g->f[i].filter, en;
        if (as_reader(flt) || as_arend(flt)) continue;
        if ((int32_t)vc(c, flt, 10, 1, tmp) < 0 || !(en = rd32(tmp))) continue;
        while (result != S_OK && vc(c, en, 3, 3, 1u, tmp + 4, tmp + 8) == S_OK) {
            uint32_t pin = rd32(tmp + 4);
            uint32_t r = pin_connect_video(c, p, pin);
            T("Render: video to filter %d pin 0x%x -> 0x%x", i, pin, r);
            if (r == S_OK) result = S_OK;
            G_RELEASE(c, pin);
        }
        G_RELEASE(c, en);
    }
    gfree(tmp);
    if (result == S_OK) { pthread_mutex_lock(&g->mu); g->renderers++; pthread_mutex_unlock(&g->mu); }
    else rt_log("[dshow] Render: no filter in the graph accepts RGB32 video");
    return result;
}
static uint32_t gb_notimpl(CPU *c, ComObj *s, const ComMethod *m) { rt_log("[dshow] IGraphBuilder::%s is not implemented", m->name); return E_NOTIMPL; }
static const ComMethod gb_m[] = { FAC_IUNKNOWN,
    {"AddFilter", 3, gb_AddFilter, 0, 0, 0}, {"RemoveFilter", 2, gb_RemoveFilter, 0, 0, 0}, {"EnumFilters", 2, gb_EnumFilters, 0, 0, 0},
    {"FindFilterByName", 3, gb_FindFilterByName, 0, 0, 0}, {"ConnectDirect", 4, gb_ConnectDirect, 0, 0, 0},
    {"Reconnect", 2, gb_notimpl, 0, 0, 0}, {"Disconnect", 2, gb_Disconnect, 0, 0, 0}, {"SetDefaultSyncSource", 1, gb_notimpl, 0, 0, 0},
    {"Connect", 3, gb_notimpl, 0, 0, 0}, {"Render", 2, gb_Render, 0, 0, 0}, {"RenderFile", 3, gb_notimpl, 0, 0, 0},
    {"AddSourceFilter", 4, gb_notimpl, 0, 0, 0}, {"SetLogFile", 2, gb_notimpl, 0, 0, 0},
    {"Abort", 1, gb_notimpl, 0, 0, 0}, {"ShouldOperationContinue", 1, gb_notimpl, 0, 0, 0} };
static ComClass gb_cls = {"IGraphBuilder", sizeof gb_m / sizeof *gb_m, gb_m, 0, NULL};

// IMediaControl
static uint32_t mc_Run(CPU *c, ComObj *s, const ComMethod *m) { return graph_set_state(c, GR(s), ST_RUNNING); }
static uint32_t mc_Pause(CPU *c, ComObj *s, const ComMethod *m) { return graph_set_state(c, GR(s), ST_PAUSED); }
static uint32_t mc_Stop(CPU *c, ComObj *s, const ComMethod *m) { return graph_set_state(c, GR(s), ST_STOPPED); }
static uint32_t mc_GetState(CPU *c, ComObj *s, const ComMethod *m) { if (!M(2)) return E_POINTER; wr32(M(2), (uint32_t)GR(s)->state); return S_OK; }
static uint32_t mc_StopWhenReady(CPU *c, ComObj *s, const ComMethod *m) {
    uint32_t r = graph_set_state(c, GR(s), ST_PAUSED);
    if ((int32_t)r < 0) return r;
    return graph_set_state(c, GR(s), ST_STOPPED);
}
static uint32_t mc_notimpl(CPU *c, ComObj *s, const ComMethod *m) { rt_log("[dshow] IMediaControl::%s is not implemented", m->name); return E_NOTIMPL; }
static const ComMethod mc_m[] = { FAC_IUNKNOWN, IDISPATCH,
    {"Run", 1, mc_Run, 0, 0, 0}, {"Pause", 1, mc_Pause, 0, 0, 0}, {"Stop", 1, mc_Stop, 0, 0, 0}, {"GetState", 3, mc_GetState, 0, 0, 0},
    {"RenderFile", 2, mc_notimpl, 0, 0, 0}, {"AddSourceFilter", 3, mc_notimpl, 0, 0, 0}, {"get_FilterCollection", 2, mc_notimpl, 0, 0, 0},
    {"get_RegFilterCollection", 2, mc_notimpl, 0, 0, 0}, {"StopWhenReady", 1, mc_StopWhenReady, 0, 0, 0} };
static ComClass mc_cls = {"IMediaControl", sizeof mc_m / sizeof *mc_m, mc_m, 0, NULL};

// IMediaEventEx
static uint32_t me_GetEventHandle(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(1)) return E_POINTER;
    pthread_mutex_lock(&g->mu);
    if (!g->ev_handle) { g->ev_handle = rt_event_new(1, g->ev_n > 0); }
    uint32_t h = g->ev_handle;
    pthread_mutex_unlock(&g->mu);
    if (!h) return E_OUTOFMEMORY;
    wr32(M(1), h);
    return S_OK;
}
static int ev_wait(Graph *g, uint32_t ms) {   // with g->mu held: 1 when an event is queued
    if (g->ev_n) return 1;
    if (ms == 0) return 0;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    if (ms != 0xFFFFFFFFu) { ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } }
    while (!g->ev_n) {
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&g->cv, &g->mu);
        else if (pthread_cond_timedwait(&g->cv, &g->mu, &ts) == ETIMEDOUT) break;
    }
    return g->ev_n > 0;
}
// GetEvent(long *code, LONG_PTR *p1, LONG_PTR *p2, long msTimeout)
static uint32_t me_GetEvent(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(1) || !M(2) || !M(3)) return E_POINTER;
    pthread_mutex_lock(&g->mu);
    if (!ev_wait(g, M(4))) { pthread_mutex_unlock(&g->mu); return E_ABORT; }
    int i = g->ev_head;
    wr32(M(1), g->ev[i].code); wr32(M(2), g->ev[i].p1); wr32(M(3), g->ev[i].p2);
    g->ev_head = (g->ev_head + 1) % MAXEV; g->ev_n--;
    if (!g->ev_n && g->ev_handle) rt_event_reset(g->ev_handle);
    pthread_mutex_unlock(&g->mu);
    return S_OK;
}
// WaitForCompletion(long msTimeout, long *code): EC_COMPLETE, EC_ERRORABORT or EC_USERABORT
static uint32_t me_WaitForCompletion(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(2)) return E_POINTER;
    wr32(M(2), 0);
    if (g->state != ST_RUNNING) return VFW_E_WRONG_STATE;
    uint32_t ms = M(1);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    if (ms != 0xFFFFFFFFu) { ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } }
    pthread_mutex_lock(&g->mu);
    for (;;) {
        for (int k = 0; k < g->ev_n; k++) {
            int i = (g->ev_head + k) % MAXEV;
            uint32_t code = g->ev[i].code;
            if (code == EC_COMPLETE || code == EC_ERRORABORT || code == 2) {   // EC_USERABORT
                // the event is consumed, as DirectShow does
                for (int j = k; j < g->ev_n - 1; j++) g->ev[(g->ev_head + j) % MAXEV] = g->ev[(g->ev_head + j + 1) % MAXEV];
                g->ev_n--;
                if (!g->ev_n && g->ev_handle) rt_event_reset(g->ev_handle);
                pthread_mutex_unlock(&g->mu);
                wr32(M(2), code);
                return S_OK;
            }
        }
        if (ms == 0) break;
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&g->cv, &g->mu);
        else if (pthread_cond_timedwait(&g->cv, &g->mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g->mu);
    return E_ABORT;
}
static uint32_t me_CancelDefaultHandling(CPU *c, ComObj *s, const ComMethod *m) {
    if (M(1) == EC_COMPLETE) { GR(s)->default_complete = 0; return S_OK; }
    return E_INVALIDARG;   // only EC_COMPLETE's default handling can be changed here
}
static uint32_t me_RestoreDefaultHandling(CPU *c, ComObj *s, const ComMethod *m) {
    if (M(1) == EC_COMPLETE) { GR(s)->default_complete = 1; return S_OK; }
    return E_INVALIDARG;
}
static uint32_t me_FreeEventParams(CPU *c, ComObj *s, const ComMethod *m) { return S_OK; }   // no event this graph sends carries a BSTR
static uint32_t me_SetNotifyWindow(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    pthread_mutex_lock(&g->mu); g->notify_hwnd = M(1); g->notify_msg = M(2); g->notify_inst = M(3); pthread_mutex_unlock(&g->mu);
    return S_OK;
}
static uint32_t me_SetNotifyFlags(CPU *c, ComObj *s, const ComMethod *m) {
    if (M(1) > 1) return E_INVALIDARG;
    GR(s)->notify_flags = M(1);
    return S_OK;
}
static uint32_t me_GetNotifyFlags(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; wr32(M(1), GR(s)->notify_flags); return S_OK; }
static const ComMethod me_m[] = { FAC_IUNKNOWN, IDISPATCH,
    {"GetEventHandle", 2, me_GetEventHandle, 0, 0, 0}, {"GetEvent", 5, me_GetEvent, 0, 0, 0},
    {"WaitForCompletion", 3, me_WaitForCompletion, 0, 0, 0}, {"CancelDefaultHandling", 2, me_CancelDefaultHandling, 0, 0, 0},
    {"RestoreDefaultHandling", 2, me_RestoreDefaultHandling, 0, 0, 0}, {"FreeEventParams", 4, me_FreeEventParams, 0, 0, 0},
    {"SetNotifyWindow", 4, me_SetNotifyWindow, 0, 0, 0}, {"SetNotifyFlags", 2, me_SetNotifyFlags, 0, 0, 0},
    {"GetNotifyFlags", 2, me_GetNotifyFlags, 0, 0, 0} };
static ComClass me_cls = {"IMediaEventEx", sizeof me_m / sizeof *me_m, me_m, 0, NULL};

// IMediaPosition (REFTIME: seconds as a double)
static int64_t reader_position(Reader *r);
static void reader_seek(Reader *r, int64_t t);
static uint32_t mp_get_Duration(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(1)) return E_POINTER;
    if (!g->src) return E_NOTIMPL;
    wr_double(M(1), (double)g->src->duration / 1e7);
    return S_OK;
}
static uint32_t mp_put_CurrentPosition(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!g->src) return E_NOTIMPL;
    double t = ARGD(1);
    if (!(t >= 0)) t = 0;
    int64_t v = (int64_t)(t * 1e7);
    if (v > g->src->duration) v = g->src->duration;
    pthread_mutex_lock(&g->mu); g->completed = 0; pthread_mutex_unlock(&g->mu);
    reader_seek(g->src, v);
    return S_OK;
}
static uint32_t mp_get_CurrentPosition(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(1)) return E_POINTER;
    if (!g->src) return E_NOTIMPL;
    wr_double(M(1), (double)reader_position(g->src) / 1e7);
    return S_OK;
}
static uint32_t mp_get_StopTime(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(1)) return E_POINTER;
    if (!g->src) return E_NOTIMPL;
    wr_double(M(1), (double)g->src->stop_time / 1e7);
    return S_OK;
}
static uint32_t mp_put_StopTime(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!g->src) return E_NOTIMPL;
    double t = ARGD(1);
    int64_t v = t >= 0 ? (int64_t)(t * 1e7) : 0;
    pthread_mutex_lock(&g->src->mu);
    g->src->stop_time = v < g->src->duration ? v : g->src->duration;
    pthread_cond_broadcast(&g->src->cv);
    pthread_mutex_unlock(&g->src->mu);
    return S_OK;
}
static uint32_t mp_get_PrerollTime(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; wr_double(M(1), 0); return S_OK; }
static uint32_t mp_put_PrerollTime(CPU *c, ComObj *s, const ComMethod *m) { return ARGD(1) == 0 ? S_OK : E_NOTIMPL; }
static uint32_t mp_put_Rate(CPU *c, ComObj *s, const ComMethod *m) {
    if (ARGD(1) == 1.0) return S_OK;
    rt_log("[dshow] IMediaPosition::put_Rate(%g): only 1.0 is implemented", ARGD(1));
    return E_NOTIMPL;
}
static uint32_t mp_get_Rate(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; wr_double(M(1), 1.0); return S_OK; }
static uint32_t mp_Can(CPU *c, ComObj *s, const ComMethod *m) {
    if (!M(1)) return E_POINTER;
    wr32(M(1), GR(s)->src ? 0xFFFFFFFFu : 0);   // OATRUE / OAFALSE
    return S_OK;
}
static const ComMethod mp_m[] = { FAC_IUNKNOWN, IDISPATCH,
    {"get_Duration", 2, mp_get_Duration, 0, 0, 0}, {"put_CurrentPosition", 3, mp_put_CurrentPosition, 0, 0, 0},
    {"get_CurrentPosition", 2, mp_get_CurrentPosition, 0, 0, 0}, {"get_StopTime", 2, mp_get_StopTime, 0, 0, 0},
    {"put_StopTime", 3, mp_put_StopTime, 0, 0, 0}, {"get_PrerollTime", 2, mp_get_PrerollTime, 0, 0, 0},
    {"put_PrerollTime", 3, mp_put_PrerollTime, 0, 0, 0}, {"put_Rate", 3, mp_put_Rate, 0, 0, 0}, {"get_Rate", 2, mp_get_Rate, 0, 0, 0},
    {"CanSeekForward", 2, mp_Can, 0, 0, 0}, {"CanSeekBackward", 2, mp_Can, 0, 0, 0} };
static ComClass mp_cls = {"IMediaPosition", sizeof mp_m / sizeof *mp_m, mp_m, 0, NULL};

// IBasicAudio: through the graph's audio renderer (E_NOTIMPL without one, as the graph manager answers)
static uint32_t ba_put_Volume(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = GR(s)->arend;
    if (!a) return E_NOTIMPL;
    int32_t v = (int32_t)M(1);
    if (v < -10000 || v > 0) return E_INVALIDARG;
    a->volume = v; ar_gains(a);
    return S_OK;
}
static uint32_t ba_get_Volume(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = GR(s)->arend;
    if (!a) return E_NOTIMPL;
    if (!M(1)) return E_POINTER;
    wr32(M(1), (uint32_t)a->volume);
    return S_OK;
}
static uint32_t ba_put_Balance(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = GR(s)->arend;
    if (!a) return E_NOTIMPL;
    int32_t v = (int32_t)M(1);
    if (v < -10000 || v > 10000) return E_INVALIDARG;
    a->balance = v; ar_gains(a);
    return S_OK;
}
static uint32_t ba_get_Balance(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = GR(s)->arend;
    if (!a) return E_NOTIMPL;
    if (!M(1)) return E_POINTER;
    wr32(M(1), (uint32_t)a->balance);
    return S_OK;
}
static const ComMethod ba_m[] = { FAC_IUNKNOWN, IDISPATCH,
    {"put_Volume", 2, ba_put_Volume, 0, 0, 0}, {"get_Volume", 2, ba_get_Volume, 0, 0, 0},
    {"put_Balance", 2, ba_put_Balance, 0, 0, 0}, {"get_Balance", 2, ba_get_Balance, 0, 0, 0} };
static ComClass ba_cls = {"IBasicAudio", sizeof ba_m / sizeof *ba_m, ba_m, 0, NULL};

// IMediaEventSink: the filters' notifications
static uint32_t sink_Notify(CPU *c, ComObj *s, const ComMethod *m) { graph_notify(GR(s), M(1), M(2), M(3)); return S_OK; }
static const ComMethod sink_m[] = { FAC_IUNKNOWN, {"Notify", 4, sink_Notify, 0, 0, 0} };
static ComClass sink_cls = {"IMediaEventSink", sizeof sink_m / sizeof *sink_m, sink_m, 0, NULL};

// IObjectWithSite: the application's site (for the ASF reader's certificate on Windows)
static uint32_t ows_SetSite(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (M(1)) G_ADDREF(c, M(1));
    if (g->site) G_RELEASE(c, g->site);
    g->site = M(1);
    return S_OK;
}
static uint32_t ows_GetSite(CPU *c, ComObj *s, const ComMethod *m) {
    Graph *g = GR(s);
    if (!M(2)) return E_POINTER;
    wr32(M(2), 0);
    if (!g->site) return E_FAIL;
    uint32_t tmp = gcalloc(1, 4);
    uint32_t r = vc(c, g->site, 0, 2, M(1), tmp);
    wr32(M(2), rd32(tmp));
    gfree(tmp);
    return r;
}
static const ComMethod ows_m[] = { FAC_IUNKNOWN, {"SetSite", 2, ows_SetSite, 0, 0, 0}, {"GetSite", 3, ows_GetSite, 0, 0, 0} };
static ComClass ows_cls = {"IObjectWithSite", sizeof ows_m / sizeof *ows_m, ows_m, 0, NULL};

static Graph *graph_new(void) {
    Graph *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->own = (Owner){1, graph_destroy, graph_qi, "FilterGraph"};
    pthread_mutex_init(&g->mu, NULL); pthread_cond_init(&g->cv, NULL);
    g->default_complete = 1;
    facet_wrap(&g->gb, &g->own, &gb_cls); facet_wrap(&g->mc, &g->own, &mc_cls); facet_wrap(&g->me, &g->own, &me_cls);
    facet_wrap(&g->mp, &g->own, &mp_cls); facet_wrap(&g->ba, &g->own, &ba_cls); facet_wrap(&g->sink, &g->own, &sink_cls);
    facet_wrap(&g->ows, &g->own, &ows_cls);
    return g;
}

// ---------------- the audio renderer's COM side ----------------
#define AR(s) OWNER_OF(ARend, s)
static ComClass ar_bf_cls, ar_pin_cls;
static ARend *as_arend(uint32_t filter) {
    Facet *f = (Facet *)com_host(filter);
    return f && f->base.cls == &ar_bf_cls ? (ARend *)f->o : NULL;
}
static uint32_t arf_GetClassID(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; memcpy(G2H(M(1)), &CLSID_DSoundRender, 16); return S_OK; }
static uint32_t arf_Stop(CPU *c, ComObj *s, const ComMethod *m) { ar_set_state(AR(s), ST_STOPPED); return S_OK; }
static uint32_t arf_Pause(CPU *c, ComObj *s, const ComMethod *m) { ar_set_state(AR(s), ST_PAUSED); return S_OK; }
static uint32_t arf_Run(CPU *c, ComObj *s, const ComMethod *m) { ARend *a = AR(s); if (a->fb.state == ST_STOPPED) ar_set_state(a, ST_PAUSED); ar_set_state(a, ST_RUNNING); return S_OK; }
static uint32_t arf_GetState(CPU *c, ComObj *s, const ComMethod *m) { if (!M(2)) return E_POINTER; wr32(M(2), (uint32_t)AR(s)->fb.state); return S_OK; }
static uint32_t arf_SetSyncSource(CPU *c, ComObj *s, const ComMethod *m) { return fb_SetSyncSource(c, &AR(s)->fb, M(1)); }
static uint32_t arf_GetSyncSource(CPU *c, ComObj *s, const ComMethod *m) { return fb_GetSyncSource(c, &AR(s)->fb, M(1)); }
static int arpins_item(Enum *e, CPU *c, int i, uint32_t *out) {
    ARend *a = (ARend *)e->src;
    if (i > 0) return 0;
    *out = a->in.f.base.guest;
    if (c) own_addref(&a->own);
    return 1;
}
static uint32_t arf_EnumPins(CPU *c, ComObj *s, const ComMethod *m) {
    if (!M(1)) return E_POINTER;
    uint32_t e = enum_new(&AR(s)->own, arpins_item, &enumpins_cls, 0, NULL);
    wr32(M(1), e);
    return e ? S_OK : E_OUTOFMEMORY;
}
static const char ar_pin_name[] = "Audio Input pin (rendered)";
static int wname_is(uint32_t w, const char *s) {
    if (!w) return 0;
    size_t i = 0;
    for (; s[i]; i++) if (rd16(w + 2 * (uint32_t)i) != (uint8_t)s[i]) return 0;
    return rd16(w + 2 * (uint32_t)i) == 0;
}
static uint32_t arf_FindPin(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = AR(s);
    if (!M(2)) return E_POINTER;
    if (wname_is(M(1), ar_pin_name)) { wr32(M(2), a->in.f.base.guest); own_addref(&a->own); return S_OK; }
    wr32(M(2), 0);
    return VFW_E_NOT_FOUND;
}
static uint32_t arf_QueryFilterInfo(CPU *c, ComObj *s, const ComMethod *m) { return fb_QueryFilterInfo(c, &AR(s)->fb, M(1)); }
static uint32_t arf_JoinFilterGraph(CPU *c, ComObj *s, const ComMethod *m) { return fb_Join(&AR(s)->fb, M(1), M(2)); }
static const ComMethod ar_bf_m[] = { FAC_IUNKNOWN,
    {"GetClassID", 2, arf_GetClassID, 0, 0, 0}, {"Stop", 1, arf_Stop, 0, 0, 0}, {"Pause", 1, arf_Pause, 0, 0, 0},
    {"Run", 3, arf_Run, 0, 0, 0}, {"GetState", 3, arf_GetState, 0, 0, 0}, {"SetSyncSource", 2, arf_SetSyncSource, 0, 0, 0},
    {"GetSyncSource", 2, arf_GetSyncSource, 0, 0, 0}, {"EnumPins", 2, arf_EnumPins, 0, 0, 0}, {"FindPin", 3, arf_FindPin, 0, 0, 0},
    {"QueryFilterInfo", 2, arf_QueryFilterInfo, 0, 0, 0}, {"JoinFilterGraph", 3, arf_JoinFilterGraph, 0, 0, 0},
    {"QueryVendorInfo", 2, NULL, E_NOTIMPL, 0, 0} };
static ComClass ar_bf_cls = {"IBaseFilter (audio renderer)", sizeof ar_bf_m / sizeof *ar_bf_m, ar_bf_m, 0, NULL};
// the input pin
#define APIN(s) (((struct APin *)(s))->a)
static uint32_t arp_Connect(CPU *c, ComObj *s, const ComMethod *m) { return VFW_E_INVALID_DIRECTION; }   // input pins don't initiate
static uint32_t arp_ReceiveConnection(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = APIN(s);
    if (a->peer) return VFW_E_ALREADY_CONNECTED;
    return VFW_E_TYPE_NOT_ACCEPTED;   // it is connected natively to the ASF reader's audio pin (pin_connect_audio)
}
static uint32_t arp_Disconnect(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = APIN(s);
    if (!a->peer) return S_FALSE;
    if (a->fb.state != ST_STOPPED) return VFW_E_NOT_STOPPED;
    G_RELEASE(c, a->peer); a->peer = 0;
    return S_OK;
}
static uint32_t arp_ConnectedTo(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = APIN(s);
    if (!M(1)) return E_POINTER;
    wr32(M(1), a->peer);
    if (!a->peer) return VFW_E_NOT_CONNECTED;
    G_ADDREF(c, a->peer);
    return S_OK;
}
static uint32_t arp_ConnectionMediaType(CPU *c, ComObj *s, const ComMethod *m) {
    ARend *a = APIN(s);
    if (!M(1)) return E_POINTER;
    if (!a->peer) { memset(G2H(M(1)), 0, 72); return VFW_E_NOT_CONNECTED; }
    Format f = {0, 0, 0, 0, a->rate};
    mt_fill(M(1), &f);
    return S_OK;
}
static uint32_t arp_QueryPinInfo(CPU *c, ComObj *s, const ComMethod *m) {   // PIN_INFO: IBaseFilter *, PIN_DIRECTION, WCHAR[128]
    ARend *a = APIN(s);
    if (!M(1)) return E_POINTER;
    wr32(M(1), a->bf.base.guest); own_addref(&a->own);
    wr32(M(1) + 4, 0);   // PINDIR_INPUT
    wput(M(1) + 8, ar_pin_name, 128);
    return S_OK;
}
static uint32_t arp_QueryDirection(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; wr32(M(1), 0); return S_OK; }
static uint32_t pin_query_id(uint32_t out, const char *name) {
    if (!out) return E_POINTER;
    uint32_t n = (uint32_t)strlen(name) + 1, w = gmalloc(n * 2);
    if (!w) return E_OUTOFMEMORY;
    wput(w, name, n);
    wr32(out, w);
    return S_OK;
}
static uint32_t arp_QueryId(CPU *c, ComObj *s, const ComMethod *m) { return pin_query_id(M(1), ar_pin_name); }
static uint32_t arp_QueryAccept(CPU *c, ComObj *s, const ComMethod *m) { Format f = {0, 0, 0, 0, APIN(s)->rate}; return mt_matches(M(1), &f) ? S_OK : S_FALSE; }
static int none_item(Enum *e, CPU *c, int i, uint32_t *out) { return 0; }
static uint32_t arp_EnumMediaTypes(CPU *c, ComObj *s, const ComMethod *m) {   // a renderer's input pin prefers no types
    if (!M(1)) return E_POINTER;
    uint32_t e = enum_new(&APIN(s)->own, none_item, &enumtypes_cls, 0, NULL);
    wr32(M(1), e);
    return e ? S_OK : E_OUTOFMEMORY;
}
static uint32_t arp_ok(CPU *c, ComObj *s, const ComMethod *m) { return S_OK; }   // stream control arrives natively
static const ComMethod ar_pin_m[] = { FAC_IUNKNOWN,
    {"Connect", 3, arp_Connect, 0, 0, 0}, {"ReceiveConnection", 3, arp_ReceiveConnection, 0, 0, 0}, {"Disconnect", 1, arp_Disconnect, 0, 0, 0},
    {"ConnectedTo", 2, arp_ConnectedTo, 0, 0, 0}, {"ConnectionMediaType", 2, arp_ConnectionMediaType, 0, 0, 0},
    {"QueryPinInfo", 2, arp_QueryPinInfo, 0, 0, 0}, {"QueryDirection", 2, arp_QueryDirection, 0, 0, 0}, {"QueryId", 2, arp_QueryId, 0, 0, 0},
    {"QueryAccept", 2, arp_QueryAccept, 0, 0, 0}, {"EnumMediaTypes", 2, arp_EnumMediaTypes, 0, 0, 0},
    {"QueryInternalConnections", 3, NULL, E_NOTIMPL, 0, 0}, {"EndOfStream", 1, arp_ok, 0, 0, 0}, {"BeginFlush", 1, arp_ok, 0, 0, 0},
    {"EndFlush", 1, arp_ok, 0, 0, 0}, {"NewSegment", 7, arp_ok, 0, 0, 0} };
static ComClass ar_pin_cls = {"IPin (audio renderer)", sizeof ar_pin_m / sizeof *ar_pin_m, ar_pin_m, 0, NULL};

static uint32_t pin_connect_audio(CPU *c, Graph *g, Pin *p) {
    if (g->arend) { rt_log("[dshow] Render: the graph already has an audio renderer"); return VFW_E_CANNOT_RENDER; }
    ARend *a = calloc(1, sizeof *a);
    if (!a) return E_OUTOFMEMORY;
    a->own = (Owner){1, ar_destroy, ar_qi, "AudioRenderer"};
    a->in.a = a;
    a->volume = 0; a->balance = 0; ar_gains(a);
    a->in.f.qi = pin_qi;
    facet_wrap(&a->bf, &a->own, &ar_bf_cls); facet_wrap(&a->in.f, &a->own, &ar_pin_cls);
    uint32_t r = ar_open(a, p->r->af.rate);
    if (r != S_OK) { own_release(&a->own); return r; }
    uint32_t name = gcalloc(64, 2);
    wput(name, "Default DirectSound Device", 64);
    r = graph_add(c, g, a->bf.base.guest, name);
    gfree(name);
    own_release(&a->own);   // the graph holds it
    if (r != S_OK) return r;
    g->arend = a;
    a->peer = p->f.base.guest; G_ADDREF(c, a->peer);
    p->peer = a->in.f.base.guest; G_ADDREF(c, p->peer);
    p->arend = a;
    T("Render: audio to the audio renderer (%d Hz)", a->rate);
    return S_OK;
}

// ---------------- the ASF reader: decoding ----------------
#define VQ_MAX 24
#define VQ_BYTES (96u << 20)
static int io_read(void *opaque, uint8_t *buf, int size) {
    Reader *r = opaque;
    size_t n = fread(buf, 1, (size_t)size, r->file);
    return n ? (int)n : AVERROR_EOF;
}
static int64_t io_seek(void *opaque, int64_t off, int whence) {
    Reader *r = opaque;
    if (whence == AVSEEK_SIZE) { off_t cur = ftello(r->file); fseeko(r->file, 0, SEEK_END); off_t sz = ftello(r->file); fseeko(r->file, cur, SEEK_SET); return sz; }
    return fseeko(r->file, off, whence & 3) ? -1 : ftello(r->file);
}
static int64_t to100(int64_t ts, AVRational tb) { return ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, tb, (AVRational){1, 10000000}); }
static void vq_clear(Reader *r) {
    while (r->vq) { VFrame *f = r->vq; r->vq = f->next; free(f->bgra); free(f); }
    r->vq_tail = NULL; r->vq_n = 0; r->vq_bytes = 0;
}
static void reader_close_media(Reader *r) {
    vq_clear(r);
    avcodec_free_context(&r->vdec); avcodec_free_context(&r->adec);
    if (r->sws) { sws_freeContext(r->sws); r->sws = NULL; }
    swr_free(&r->swr);
    if (r->fmt) avformat_close_input(&r->fmt);
    if (r->avio) { av_freep(&r->avio->buffer); avio_context_free(&r->avio); }
    if (r->file) { fclose(r->file); r->file = NULL; }
}
static AVCodecContext *open_decoder(AVStream *st) {
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { rt_log("[dshow] no decoder for %s", avcodec_get_name(st->codecpar->codec_id)); return NULL; }
    AVCodecContext *d = avcodec_alloc_context3(codec);
    if (!d) return NULL;
    if (avcodec_parameters_to_context(d, st->codecpar) < 0 || avcodec_open2(d, codec, NULL) < 0) {
        rt_log("[dshow] can't open the %s decoder", codec->name);
        avcodec_free_context(&d);
        return NULL;
    }
    return d;
}
static uint32_t reader_load(Reader *r, const char *host) {
    r->file = fopen(host, "rb");
    if (!r->file) { rt_log("[dshow] ASF reader: can't open %s", host); return HR_FILE_NOT_FOUND; }
    unsigned char *b = av_malloc(65536);
    r->avio = b ? avio_alloc_context(b, 65536, 0, r, io_read, NULL, io_seek) : NULL;
    if (!r->avio) { av_free(b); reader_close_media(r); return E_OUTOFMEMORY; }
    r->fmt = avformat_alloc_context();
    if (!r->fmt) { reader_close_media(r); return E_OUTOFMEMORY; }
    r->fmt->pb = r->avio;
    if (avformat_open_input(&r->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(r->fmt, NULL) < 0) {
        rt_log("[dshow] ASF reader: %s is not a readable ASF file", host);
        reader_close_media(r);
        return VFW_E_UNKNOWN_FILE_TYPE;
    }
    r->vs = r->as = -1;
    for (unsigned i = 0; i < r->fmt->nb_streams; i++) {
        AVStream *st = r->fmt->streams[i];
        enum AVMediaType t = st->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && r->vs < 0) {
            int w = st->codecpar->width, h = st->codecpar->height;
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { rt_log("[dshow] video stream %u: size %dx%d is out of range", i, w, h); st->discard = AVDISCARD_ALL; continue; }
            if (!(r->vdec = open_decoder(st))) { st->discard = AVDISCARD_ALL; continue; }
            r->vs = (int)i;
            AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            r->vf = (Format){1, w, h, fr.num ? (int64_t)(10000000.0 * fr.den / fr.num) : 333333, 0};
        } else if (t == AVMEDIA_TYPE_AUDIO && r->as < 0) {
            int rate = st->codecpar->sample_rate;
            if (rate < 8000 || rate > 192000) { rt_log("[dshow] audio stream %u: rate %d is out of range", i, rate); st->discard = AVDISCARD_ALL; continue; }
            if (!(r->adec = open_decoder(st))) { st->discard = AVDISCARD_ALL; continue; }
            r->as = (int)i;
            r->af = (Format){0, 0, 0, 0, rate};
        } else st->discard = AVDISCARD_ALL;
    }
    if (r->vs < 0 && r->as < 0) { rt_log("[dshow] ASF reader: %s has no stream that can be decoded", host); reader_close_media(r); return VFW_E_UNKNOWN_FILE_TYPE; }
    r->start100 = r->fmt->start_time != AV_NOPTS_VALUE ? r->fmt->start_time * 10 : 0;
    r->duration = r->fmt->duration > 0 ? r->fmt->duration * 10 : 0;
    r->stop_time = r->duration;
    r->demux_at = 0;
    rt_log("[dshow] ASF reader: %s: %s%s%dx%d, %s %d Hz, %.2f s", host, r->vs >= 0 ? avcodec_get_name(r->vdec->codec_id) : "no video", r->vs >= 0 ? " " : "",
           r->vf.width, r->vf.height, r->as >= 0 ? avcodec_get_name(r->adec->codec_id) : "no audio", r->af.rate, (double)r->duration / 1e7);
    return S_OK;
}
static void push_video(Reader *r, AVFrame *f) {
    int64_t t = to100(f->best_effort_timestamp, r->fmt->streams[r->vs]->time_base);
    if (t == AV_NOPTS_VALUE) t = r->vq_tail ? r->vq_tail->t + r->vf.frame_time : 0;
    else t -= r->start100;
    int w = r->vf.width, h = r->vf.height;
    size_t bytes = (size_t)w * (size_t)h * 4;
    VFrame *v = calloc(1, sizeof *v);
    uint8_t *px = v ? malloc(bytes) : NULL;
    if (!px) { free(v); rt_log("[dshow] out of memory for a %dx%d frame", w, h); return; }
    r->sws = sws_getCachedContext(r->sws, f->width, f->height, f->format, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!r->sws) { free(px); free(v); rt_log("[dshow] no converter from pixel format %d", f->format); return; }
    uint8_t *planes[1] = {px + (size_t)(h - 1) * (size_t)w * 4};   // bottom-up, like a DIB
    int strides[1] = {-w * 4};
    sws_scale(r->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, planes, strides);
    v->bgra = px; v->t = t;
    v->dur = f->duration > 0 ? to100(f->duration, r->fmt->streams[r->vs]->time_base) : r->vf.frame_time;
    v->key = (f->flags & AV_FRAME_FLAG_KEY) != 0;
    if (r->vq_tail) r->vq_tail->next = v; else r->vq = v;
    r->vq_tail = v; r->vq_n++; r->vq_bytes += bytes;
}
// audio into the renderer's ring; waits (bounded steps) while the ring is full
static void push_audio(Reader *r, ARend *a, AVFrame *f, int64_t min_t) {
    int64_t t = to100(f->best_effort_timestamp, r->fmt->streams[r->as]->time_base);
    if (t != AV_NOPTS_VALUE) t -= r->start100;
    if (!r->swr) {
        AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&r->swr, &st, AV_SAMPLE_FMT_S16, r->af.rate, &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(r->swr) < 0) {
            rt_log("[dshow] no audio converter for format %d, %d Hz", f->format, f->sample_rate);
            swr_free(&r->swr);
            return;
        }
    }
    int max = swr_get_out_samples(r->swr, f->nb_samples);
    if (max <= 0) return;
    int16_t *pcm = malloc((size_t)max * 4);
    if (!pcm) return;
    uint8_t *dst[1] = {(uint8_t *)pcm};
    int n = swr_convert(r->swr, dst, max, (const uint8_t **)f->extended_data, f->nb_samples);
    if (n > 0) {
        int skip = 0;
        if (t != AV_NOPTS_VALUE && t < min_t) {   // after a seek: drop what lies before the target
            int64_t drop = (min_t - t) * r->af.rate / 10000000;
            skip = drop >= n ? n : (int)drop;
        }
        if (t != AV_NOPTS_VALUE) r->audio_through = t + (int64_t)n * 10000000 / r->af.rate;
        uint32_t done = (uint32_t)skip;
        while (a && done < (uint32_t)n && !r->stop_req) {
            uint32_t room = ar_free_frames(a), k = (uint32_t)n - done;
            if (k > room) k = room;
            if (k) { ar_write(a, pcm + done * 2, k); done += k; continue; }
            pthread_mutex_lock(&r->mu);
            if (r->seek_req || r->stop_req) { pthread_mutex_unlock(&r->mu); break; }
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&r->cv, &r->mu, &ts);
            pthread_mutex_unlock(&r->mu);
        }
    }
    free(pcm);
}
static void decode_packet(Reader *r, AVCodecContext *d, AVPacket *pkt, int video, ARend *a, int64_t min_t, AVFrame *f) {
    if (avcodec_send_packet(d, pkt) < 0 && pkt) return;
    while (avcodec_receive_frame(d, f) == 0) {
        if (video) {
            int64_t t = to100(f->best_effort_timestamp, r->fmt->streams[r->vs]->time_base);
            if (t == AV_NOPTS_VALUE || t - r->start100 >= min_t - r->vf.frame_time / 2) push_video(r, f);
        } else push_audio(r, a, f, min_t);
        av_frame_unref(f);
    }
}

// ---------------- the ASF reader: streaming thread ----------------
#define NPOOL 8
static RtThread *tpool[NPOOL]; static int tpool_n, tpool_made;
static pthread_mutex_t tpool_mu = PTHREAD_MUTEX_INITIALIZER;
static RtThread *thread_get(void) {   // guest thread contexts are reused: their stacks and TEBs are not freed
    pthread_mutex_lock(&tpool_mu);
    RtThread *t = tpool_n ? tpool[--tpool_n] : NULL;
    if (!t && tpool_made < NPOOL) { tpool_made++; pthread_mutex_unlock(&tpool_mu); return rt_thread_new(0x100000); }
    pthread_mutex_unlock(&tpool_mu);
    return t;
}
static void thread_put(RtThread *t) { pthread_mutex_lock(&tpool_mu); tpool[tpool_n++] = t; pthread_mutex_unlock(&tpool_mu); }

static int64_t clock_now(Reader *r) {   // with r->mu held
    if (r->fb.state == ST_RUNNING) return r->seg_base + (host_now_100ns() - r->run_t0);
    return r->seg_base;
}
static int64_t reader_position(Reader *r) {
    pthread_mutex_lock(&r->mu);
    int64_t t = r->th_live ? clock_now(r) : r->pos;
    pthread_mutex_unlock(&r->mu);
    if (t > r->duration) t = r->duration;
    return t < 0 ? 0 : t;
}
static void reader_seek(Reader *r, int64_t t) {
    pthread_mutex_lock(&r->mu);
    if (r->th_live) { r->seek_req = 1; r->seek_to = t; pthread_cond_broadcast(&r->cv); }
    else r->pos = t;
    pthread_mutex_unlock(&r->mu);
}
static uint32_t pin_new_segment(CPU *c, Pin *p, int64_t start, int64_t stop) {
    uint64_t one; double rate = 1.0; memcpy(&one, &rate, 8);
    return vc(c, p->peer, 17, 6, (uint32_t)start, (uint32_t)((uint64_t)start >> 32), (uint32_t)stop, (uint32_t)((uint64_t)stop >> 32),
              (uint32_t)one, (uint32_t)(one >> 32));
}
// wait until stream time `t`, a stop, a seek or a state change; 1 when the time came
static int wait_until(Reader *r, int64_t t) {
    pthread_mutex_lock(&r->mu);
    for (;;) {
        if (r->stop_req || r->seek_req || r->fb.state != ST_RUNNING) { pthread_mutex_unlock(&r->mu); return 0; }
        int64_t now = clock_now(r);
        if (now >= t) { pthread_mutex_unlock(&r->mu); return 1; }
        int64_t wait = t - now; if (wait > 1000000) wait = 1000000;   // at most 100 ms, then look again
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        long ns = ts.tv_nsec + (long)(wait * 100);
        ts.tv_sec += ns / 1000000000; ts.tv_nsec = ns % 1000000000;
        pthread_cond_timedwait(&r->cv, &r->mu, &ts);
    }
}
static void *stream_main(void *arg) {
    Reader *r = arg;
    RtThread *t = r->th_rt;
    rt_thread_attach(t);
    CPU *c = t->cpu;
    Pin *vp = NULL, *ap = NULL;
    for (int i = 0; i < r->npins; i++) { if (r->pin[i].video && r->pin[i].peer) vp = &r->pin[i]; if (!r->pin[i].video && r->pin[i].peer) ap = &r->pin[i]; }
    ARend *a = ap ? ap->arend : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    if (!pkt || !fr) { rt_log("[dshow] streaming: out of memory"); goto out; }
    int64_t min_t = 0;
    int newseg = 1, need_seek = 1, first = 1;
    int64_t seek_t;
    pthread_mutex_lock(&r->mu); seek_t = r->pos; pthread_mutex_unlock(&r->mu);
    for (;;) {
        pthread_mutex_lock(&r->mu);
        while (!r->stop_req && !r->seek_req && r->fb.state != ST_RUNNING) pthread_cond_wait(&r->cv, &r->mu);
        if (r->stop_req) { pthread_mutex_unlock(&r->mu); break; }
        if (r->seek_req) { r->seek_req = 0; seek_t = r->seek_to; need_seek = 1; }
        pthread_mutex_unlock(&r->mu);
        if (need_seek) {
            need_seek = 0;
            if (vp && !first) { vc(c, vp->peer, 15, 0); vc(c, vp->peer, 16, 0); }   // BeginFlush, EndFlush
            first = 0;
            if (a) ar_flush(a);
            vq_clear(r);
            if (r->vdec) avcodec_flush_buffers(r->vdec);
            if (r->adec) avcodec_flush_buffers(r->adec);
            if (seek_t != r->demux_at) av_seek_frame(r->fmt, -1, (seek_t + r->start100) / 10, AVSEEK_FLAG_BACKWARD);
            r->demux_at = -1;
            r->eof = r->eos_sent = r->complete_sent = r->v_done = 0;
            r->audio_through = seek_t;
            min_t = seek_t;
            pthread_mutex_lock(&r->mu); r->seg_base = seek_t; r->run_t0 = host_now_100ns(); pthread_mutex_unlock(&r->mu);
            newseg = 1;
        }
        if (newseg && vp) { pin_new_segment(c, vp, 0, r->stop_time - min_t); newseg = 0; }
        // read ahead: a frame to show, and audio a quarter second past it
        int64_t head = r->vq ? r->vq->t : INT64_MAX;
        while (!r->eof && !r->stop_req && !r->seek_req &&
               (!r->vq || (r->as >= 0 && a && r->audio_through < head + 2500000 && r->vq_n < VQ_MAX && r->vq_bytes < VQ_BYTES))) {
            int e = av_read_frame(r->fmt, pkt);
            if (e < 0) {
                r->eof = 1;
                if (r->vdec) decode_packet(r, r->vdec, NULL, 1, a, min_t, fr);
                if (r->adec) decode_packet(r, r->adec, NULL, 0, a, min_t, fr);
                break;
            }
            if (pkt->stream_index == r->vs && vp && !r->v_done) decode_packet(r, r->vdec, pkt, 1, a, min_t, fr);
            else if (pkt->stream_index == r->as) decode_packet(r, r->adec, pkt, 0, a, min_t, fr);
            av_packet_unref(pkt);
            head = r->vq ? r->vq->t : INT64_MAX;
        }
        if (r->seek_req || r->stop_req) continue;
        // the stop time ends the stream like the end of the file
        if (r->vq && r->vq->t >= r->stop_time) { vq_clear(r); r->eof = 1; }
        if (r->vq && vp && !r->v_done) {
            VFrame *v = r->vq;
            if (!wait_until(r, v->t)) continue;
            r->vq = v->next; if (!r->vq) r->vq_tail = NULL; r->vq_n--; r->vq_bytes -= (size_t)r->vf.width * (size_t)r->vf.height * 4;
            Sample *smp;
            uint32_t hr = alloc_get(vp->alloc, 0, &smp, &r->stop_req);
            if (hr == S_OK) {
                size_t bytes = (size_t)r->vf.width * (size_t)r->vf.height * 4;
                memcpy(G2H(smp->buf), v->bgra, bytes < smp->size ? bytes : smp->size);
                smp->actual = (int32_t)(bytes < smp->size ? bytes : smp->size);
                smp->t0 = v->t - min_t; smp->t1 = v->t - min_t + v->dur; smp->tvalid = 2;
                smp->sync = v->key;
                hr = vc(c, vp->mem, 6, 1, smp->f.base.guest);   // IMemInputPin::Receive
                own_release(&smp->own);
            }
            free(v->bgra); free(v);
            if (hr == S_FALSE) { r->v_done = 1; vc(c, vp->peer, 14, 0); }   // the renderer wants no more: end its stream
            else if ((int32_t)hr < 0) {
                pthread_mutex_lock(&r->mu);
                int quiet = r->stop_req || r->seek_req || r->quiet || r->fb.state == ST_STOPPED;
                while (r->quiet && !r->stop_req) pthread_cond_wait(&r->cv, &r->mu);   // the reader's own Stop follows
                pthread_mutex_unlock(&r->mu);
                if (!quiet) { rt_log("[dshow] video delivery failed (0x%x)", hr); fb_notify(c, &r->fb, EC_ERRORABORT, hr, 0); r->v_done = 1; }
            }
            continue;
        }
        if (!r->vq && r->eof && !r->eos_sent) {
            r->eos_sent = 1;
            if (vp && !r->v_done) vc(c, vp->peer, 14, 0);   // EndOfStream: the renderer reports EC_COMPLETE
        }
        if (r->eos_sent && !r->complete_sent && (!a || ar_queued(a) == 0)) {
            r->complete_sent = 1;
            if (a) fb_notify(c, &a->fb, EC_COMPLETE, S_OK, a->bf.base.guest);   // the audio renderer has played everything
        }
        // nothing to do until the audio drains, a seek, a stop or a state change
        pthread_mutex_lock(&r->mu);
        if (!r->stop_req && !r->seek_req) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            if (r->complete_sent) pthread_cond_wait(&r->cv, &r->mu);
            else pthread_cond_timedwait(&r->cv, &r->mu, &ts);
        }
        pthread_mutex_unlock(&r->mu);
    }
out:
    av_packet_free(&pkt);
    av_frame_free(&fr);
    pthread_mutex_lock(&r->mu);
    r->pos = r->seg_base + (r->fb.state == ST_RUNNING ? host_now_100ns() - r->run_t0 : 0);
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

// ---------------- the ASF reader's COM side ----------------
#define RD(s) OWNER_OF(Reader, s)
static ComClass rd_bf_cls, rd_fsf_cls, rd_pin_cls;
static Reader *as_reader(uint32_t filter) {
    Facet *f = (Facet *)com_host(filter);
    return f && f->base.cls == &rd_bf_cls ? (Reader *)f->o : NULL;
}
static void reader_stop_thread(Reader *r) {
    pthread_mutex_lock(&r->mu);
    int live = r->th_live;
    r->stop_req = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    if (!live) return;
    pthread_join(r->th, NULL);
    thread_put(r->th_rt);
    pthread_mutex_lock(&r->mu); r->th_live = 0; r->stop_req = 0; r->seek_req = 0; pthread_mutex_unlock(&r->mu);
    // the demuxer is wherever the thread left it: the next start seeks to r->pos
    r->demux_at = -1;
}
static uint32_t reader_start_thread(Reader *r) {
    if (r->th_live) return S_OK;
    RtThread *t = thread_get();
    if (!t) { rt_log("[dshow] more than %d videos streaming at once", NPOOL); return E_OUTOFMEMORY; }
    r->th_rt = t;
    r->stop_req = 0; r->quiet = 0;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);   // translated guest frames are big
    int e = pthread_create(&r->th, &at, stream_main, r);
    pthread_attr_destroy(&at);
    if (e) { thread_put(t); rt_log("[dshow] can't start the streaming thread (%d)", e); return E_FAIL; }
    r->th_live = 1;
    return S_OK;
}
static void reader_destroy(Owner *o) {
    Reader *r = (Reader *)o;
    reader_stop_thread(r);
    CPU *c = rt_cpu();
    for (int i = 0; i < r->npins; i++) {
        Pin *p = &r->pin[i];
        if (p->mem) G_RELEASE(c, p->mem);
        if (p->peer) G_RELEASE(c, p->peer);
        if (p->alloc) own_release(&p->alloc->own);
        facet_free(&p->f);
    }
    if (r->fb.clock) G_RELEASE(c, r->fb.clock);
    reader_close_media(r);
    facet_free(&r->bf); facet_free(&r->fsf);
    pthread_mutex_destroy(&r->mu); pthread_cond_destroy(&r->cv);
    free(r);
}
static int reader_qi(Owner *o, const Guid *iid, uint32_t *out) {
    Reader *r = (Reader *)o;
    if (!memcmp(iid, &IID_IUnknown, 16) || !memcmp(iid, &IID_IPersist, 16) || !memcmp(iid, &IID_IMediaFilter, 16) ||
        !memcmp(iid, &IID_IBaseFilter, 16)) *out = r->bf.base.guest;
    else if (!memcmp(iid, &IID_IFileSourceFilter, 16)) *out = r->fsf.base.guest;
    else return 0;
    return 1;
}
static uint32_t rdf_GetClassID(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; memcpy(G2H(M(1)), &CLSID_WMAsfReader, 16); return S_OK; }
static uint32_t rdf_Stop(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = RD(s);
    pthread_mutex_lock(&r->mu);
    if (r->fb.state == ST_RUNNING) { r->seg_base = clock_now(r); }
    r->fb.state = ST_STOPPED;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    reader_stop_thread(r);
    for (int i = 0; i < r->npins; i++) if (r->pin[i].alloc) alloc_decommit(r->pin[i].alloc);
    return S_OK;
}
static uint32_t reader_pause(Reader *r) {
    if (r->fb.state == ST_STOPPED) {
        if (!r->fmt) return VFW_E_WRONG_STATE;   // nothing loaded
        for (int i = 0; i < r->npins; i++) if (r->pin[i].alloc) { uint32_t e = alloc_commit(r->pin[i].alloc); if (e != S_OK) return e; }
        pthread_mutex_lock(&r->mu); r->seg_base = r->pos; pthread_mutex_unlock(&r->mu);
        uint32_t e = reader_start_thread(r);
        if (e != S_OK) return e;
    }
    pthread_mutex_lock(&r->mu);
    if (r->fb.state == ST_RUNNING) r->seg_base = clock_now(r);
    r->fb.state = ST_PAUSED;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    return S_OK;
}
static uint32_t rdf_Pause(CPU *c, ComObj *s, const ComMethod *m) { return reader_pause(RD(s)); }
static uint32_t rdf_Run(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = RD(s);
    if (r->fb.state == ST_STOPPED) { uint32_t e = reader_pause(r); if ((int32_t)e < 0) return e; }
    pthread_mutex_lock(&r->mu);
    if (r->fb.state != ST_RUNNING) { r->run_t0 = host_now_100ns(); r->fb.state = ST_RUNNING; }
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    return S_OK;
}
static uint32_t rdf_GetState(CPU *c, ComObj *s, const ComMethod *m) { if (!M(2)) return E_POINTER; wr32(M(2), (uint32_t)RD(s)->fb.state); return S_OK; }
static uint32_t rdf_SetSyncSource(CPU *c, ComObj *s, const ComMethod *m) { return fb_SetSyncSource(c, &RD(s)->fb, M(1)); }
static uint32_t rdf_GetSyncSource(CPU *c, ComObj *s, const ComMethod *m) { return fb_GetSyncSource(c, &RD(s)->fb, M(1)); }
static int rdpins_item(Enum *e, CPU *c, int i, uint32_t *out) {
    Reader *r = (Reader *)e->src;
    if (i >= r->npins) return 0;
    *out = r->pin[i].f.base.guest;
    if (c) own_addref(&r->own);
    return 1;
}
static uint32_t rdf_EnumPins(CPU *c, ComObj *s, const ComMethod *m) {
    if (!M(1)) return E_POINTER;
    uint32_t e = enum_new(&RD(s)->own, rdpins_item, &enumpins_cls, 0, NULL);
    wr32(M(1), e);
    return e ? S_OK : E_OUTOFMEMORY;
}
static const char *pin_name(const Pin *p) { return p->video ? "Raw Video 0" : "Raw Audio 1"; }
static uint32_t rdf_FindPin(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = RD(s);
    if (!M(2)) return E_POINTER;
    for (int i = 0; i < r->npins; i++) if (wname_is(M(1), pin_name(&r->pin[i]))) { wr32(M(2), r->pin[i].f.base.guest); own_addref(&r->own); return S_OK; }
    wr32(M(2), 0);
    return VFW_E_NOT_FOUND;
}
static uint32_t rdf_QueryFilterInfo(CPU *c, ComObj *s, const ComMethod *m) { return fb_QueryFilterInfo(c, &RD(s)->fb, M(1)); }
static uint32_t rdf_JoinFilterGraph(CPU *c, ComObj *s, const ComMethod *m) { return fb_Join(&RD(s)->fb, M(1), M(2)); }
static const ComMethod rd_bf_m[] = { FAC_IUNKNOWN,
    {"GetClassID", 2, rdf_GetClassID, 0, 0, 0}, {"Stop", 1, rdf_Stop, 0, 0, 0}, {"Pause", 1, rdf_Pause, 0, 0, 0},
    {"Run", 3, rdf_Run, 0, 0, 0}, {"GetState", 3, rdf_GetState, 0, 0, 0}, {"SetSyncSource", 2, rdf_SetSyncSource, 0, 0, 0},
    {"GetSyncSource", 2, rdf_GetSyncSource, 0, 0, 0}, {"EnumPins", 2, rdf_EnumPins, 0, 0, 0}, {"FindPin", 3, rdf_FindPin, 0, 0, 0},
    {"QueryFilterInfo", 2, rdf_QueryFilterInfo, 0, 0, 0}, {"JoinFilterGraph", 3, rdf_JoinFilterGraph, 0, 0, 0},
    {"QueryVendorInfo", 2, NULL, E_NOTIMPL, 0, 0} };
static ComClass rd_bf_cls = {"IBaseFilter (WM ASF Reader)", sizeof rd_bf_m / sizeof *rd_bf_m, rd_bf_m, 0, NULL};

// IFileSourceFilter::Load(LPCOLESTR, const AM_MEDIA_TYPE *)
static uint32_t fsf_Load(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = RD(s);
    if (!M(1)) return E_POINTER;
    if (r->fmt) return E_UNEXPECTED;   // one file per reader
    const uint16_t *w = gp(M(1));
    size_t n = 0; for (; w[n] && n < 259; n++) r->wpath[n] = w[n];
    r->wpath[n] = 0;
    char win[1100], host[1200];
    w_to_utf8(r->wpath, win, sizeof win);
    uint32_t e = reader_load(r, rt_host_path(win, host, sizeof host));
    if (e != S_OK) return e;
    r->npins = 0;
    if (r->vs >= 0) { Pin *p = &r->pin[r->npins++]; p->r = r; p->video = 1; p->f.qi = pin_qi; facet_wrap(&p->f, &r->own, &rd_pin_cls); }
    if (r->as >= 0) { Pin *p = &r->pin[r->npins++]; p->r = r; p->video = 0; p->f.qi = pin_qi; facet_wrap(&p->f, &r->own, &rd_pin_cls); }
    return S_OK;
}
// GetCurFile(LPOLESTR *, AM_MEDIA_TYPE *)
static uint32_t fsf_GetCurFile(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = RD(s);
    if (!M(1)) return E_POINTER;
    if (!r->fmt) { wr32(M(1), 0); return E_FAIL; }
    size_t n = w_len(r->wpath);
    uint32_t w = gmalloc((uint32_t)(n + 1) * 2);
    if (!w) return E_OUTOFMEMORY;
    wcopy(w, r->wpath, (uint32_t)n + 1);
    wr32(M(1), w);
    if (M(2)) { memset(G2H(M(2)), 0, 72); memcpy(G2H(M(2)), &MT_Stream, 16); memcpy(G2H(M(2) + 16), &ST_Asf, 16); }
    return S_OK;
}
static const ComMethod rd_fsf_m[] = { FAC_IUNKNOWN, {"Load", 3, fsf_Load, 0, 0, 0}, {"GetCurFile", 3, fsf_GetCurFile, 0, 0, 0} };
static ComClass rd_fsf_cls = {"IFileSourceFilter", sizeof rd_fsf_m / sizeof *rd_fsf_m, rd_fsf_m, 0, NULL};

// output pins
static const Format *pin_format(const Pin *p) { return p->video ? &p->r->vf : &p->r->af; }
static void pin_drop(CPU *c, Pin *p) {
    if (p->mem) { G_RELEASE(c, p->mem); p->mem = 0; }
    if (p->peer) { G_RELEASE(c, p->peer); p->peer = 0; }
    if (p->alloc) { alloc_decommit(p->alloc); own_release(&p->alloc->own); p->alloc = NULL; }
    p->arend = NULL;
}
static uint32_t pin_connect_video(CPU *c, Pin *p, uint32_t in) {
    if (p->peer) return VFW_E_ALREADY_CONNECTED;
    if (p->r->fb.state != ST_STOPPED) return VFW_E_NOT_STOPPED;
    uint32_t tmp = gcalloc(1, 96), mt = tmp + 16;
    uint32_t r = vc(c, in, 9, 1, tmp);   // QueryDirection
    if ((int32_t)r < 0 || rd32(tmp) != 0) { gfree(tmp); return VFW_E_INVALID_DIRECTION; }
    wr32(tmp, 0);
    if (vc(c, in, 6, 1, tmp) == S_OK) { if (rd32(tmp)) G_RELEASE(c, rd32(tmp)); gfree(tmp); return VFW_E_ALREADY_CONNECTED; }
    // CBasePin::AttemptConnection: the output pin counts as connected while the input pin decides
    p->peer = in; G_ADDREF(c, in);
    mt_fill(mt, &p->r->vf);
    r = vc(c, in, 4, 2, p->f.base.guest, mt);   // ReceiveConnection (it copies the type)
    mt_free_format(mt);
    if (r != S_OK) { G_RELEASE(c, in); p->peer = 0; gfree(tmp); return (int32_t)r < 0 ? r : VFW_E_TYPE_NOT_ACCEPTED; }
    // CBaseOutputPin::DecideAllocator: the input pin's requirements, then our allocator
    uint32_t mem = 0;
    r = g_qi(c, in, &IID_IMemInputPin, &mem);
    Alloc *a = (int32_t)r >= 0 ? alloc_new() : NULL;
    if (a) {
        int32_t count = 2, size = (int32_t)((uint32_t)p->r->vf.width * (uint32_t)p->r->vf.height * 4u), align = 1, prefix = 0;
        memset(G2H(tmp), 0, 16);
        if (vc(c, mem, 5, 1, tmp) == S_OK) {   // GetAllocatorRequirements
            if ((int32_t)rd32(tmp) > count) count = (int32_t)rd32(tmp);
            if ((int32_t)rd32(tmp + 8) > align) align = (int32_t)rd32(tmp + 8);
            if ((int32_t)rd32(tmp + 12) > prefix) prefix = (int32_t)rd32(tmp + 12);
        }
        r = alloc_set(a, count, size, align, prefix);
        if (r == S_OK) r = vc(c, mem, 4, 2, a->f.base.guest, 0u);   // NotifyAllocator(alloc, bReadOnly = FALSE)
    } else if ((int32_t)r >= 0) r = E_OUTOFMEMORY;
    gfree(tmp);
    if ((int32_t)r < 0) {
        rt_log("[dshow] video connection: allocator negotiation failed (0x%x)", r);
        if (a) own_release(&a->own);
        if (mem) G_RELEASE(c, mem);
        vc(c, in, 5, 0);   // Disconnect the input side
        G_RELEASE(c, in); p->peer = 0;
        return r;
    }
    p->mem = mem; p->alloc = a;
    return S_OK;
}
static uint32_t pn_Connect(CPU *c, ComObj *s, const ComMethod *m) {
    Pin *p = PIN_OF(s);
    if (!M(1)) return E_POINTER;
    if (M(2) && !mt_matches(M(2), pin_format(p))) return VFW_E_TYPE_NOT_ACCEPTED;
    if (!p->video) { rt_log("[dshow] the ASF reader's audio pin connects only to the graph's audio renderer (IGraphBuilder::Render)"); return VFW_E_CANNOT_CONNECT; }
    return pin_connect_video(c, p, M(1));
}
static uint32_t pn_ReceiveConnection(CPU *c, ComObj *s, const ComMethod *m) { return VFW_E_INVALID_DIRECTION; }   // an output pin
static uint32_t pn_Disconnect(CPU *c, ComObj *s, const ComMethod *m) {
    Pin *p = PIN_OF(s);
    if (!p->peer) return S_FALSE;
    if (p->r->fb.state != ST_STOPPED) return VFW_E_NOT_STOPPED;
    pin_drop(c, p);
    return S_OK;
}
static uint32_t pn_ConnectedTo(CPU *c, ComObj *s, const ComMethod *m) {
    Pin *p = PIN_OF(s);
    if (!M(1)) return E_POINTER;
    wr32(M(1), p->peer);
    if (!p->peer) return VFW_E_NOT_CONNECTED;
    G_ADDREF(c, p->peer);
    return S_OK;
}
static uint32_t pn_ConnectionMediaType(CPU *c, ComObj *s, const ComMethod *m) {
    Pin *p = PIN_OF(s);
    if (!M(1)) return E_POINTER;
    if (!p->peer) { memset(G2H(M(1)), 0, 72); return VFW_E_NOT_CONNECTED; }
    mt_fill(M(1), pin_format(p));
    return S_OK;
}
static uint32_t pn_QueryPinInfo(CPU *c, ComObj *s, const ComMethod *m) {
    Pin *p = PIN_OF(s);
    if (!M(1)) return E_POINTER;
    wr32(M(1), p->r->bf.base.guest); own_addref(&p->r->own);
    wr32(M(1) + 4, 1);   // PINDIR_OUTPUT
    wput(M(1) + 8, pin_name(p), 128);
    return S_OK;
}
static uint32_t pn_QueryDirection(CPU *c, ComObj *s, const ComMethod *m) { if (!M(1)) return E_POINTER; wr32(M(1), 1); return S_OK; }
static uint32_t pn_QueryId(CPU *c, ComObj *s, const ComMethod *m) { return pin_query_id(M(1), pin_name(PIN_OF(s))); }
static uint32_t pn_QueryAccept(CPU *c, ComObj *s, const ComMethod *m) { return mt_matches(M(1), pin_format(PIN_OF(s))) ? S_OK : S_FALSE; }
static int ptypes_item(Enum *e, CPU *c, int i, uint32_t *out) {   // the pin's one media type, CoTaskMemAlloc'd
    Pin *p = e->ctx;
    if (i > 0) return 0;
    if (!c) return 1;
    uint32_t mt = gmalloc(72);
    if (!mt) return 0;
    mt_fill(mt, pin_format(p));
    *out = mt;
    return 1;
}
static uint32_t pn_EnumMediaTypes(CPU *c, ComObj *s, const ComMethod *m) {
    Pin *p = PIN_OF(s);
    if (!M(1)) return E_POINTER;
    uint32_t e = enum_new(&p->r->own, ptypes_item, &enumtypes_cls, 0, p);
    wr32(M(1), e);
    return e ? S_OK : E_OUTOFMEMORY;
}
static uint32_t pn_unexpected(CPU *c, ComObj *s, const ComMethod *m) { return E_UNEXPECTED; }   // stream calls go downstream only
static const ComMethod rd_pin_m[] = { FAC_IUNKNOWN,
    {"Connect", 3, pn_Connect, 0, 0, 0}, {"ReceiveConnection", 3, pn_ReceiveConnection, 0, 0, 0}, {"Disconnect", 1, pn_Disconnect, 0, 0, 0},
    {"ConnectedTo", 2, pn_ConnectedTo, 0, 0, 0}, {"ConnectionMediaType", 2, pn_ConnectionMediaType, 0, 0, 0},
    {"QueryPinInfo", 2, pn_QueryPinInfo, 0, 0, 0}, {"QueryDirection", 2, pn_QueryDirection, 0, 0, 0}, {"QueryId", 2, pn_QueryId, 0, 0, 0},
    {"QueryAccept", 2, pn_QueryAccept, 0, 0, 0}, {"EnumMediaTypes", 2, pn_EnumMediaTypes, 0, 0, 0},
    {"QueryInternalConnections", 3, NULL, E_NOTIMPL, 0, 0}, {"EndOfStream", 1, pn_unexpected, 0, 0, 0},
    {"BeginFlush", 1, pn_unexpected, 0, 0, 0}, {"EndFlush", 1, pn_unexpected, 0, 0, 0}, {"NewSegment", 7, pn_unexpected, 0, 0, 0} };
static ComClass rd_pin_cls = {"IPin (WM ASF Reader)", sizeof rd_pin_m / sizeof *rd_pin_m, rd_pin_m, 0, NULL};

static Reader *reader_new(void) {
    Reader *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->own = (Owner){1, reader_destroy, reader_qi, "WMAsfReader"};
    pthread_mutex_init(&r->mu, NULL); pthread_cond_init(&r->cv, NULL);
    r->vs = r->as = -1;
    facet_wrap(&r->bf, &r->own, &rd_bf_cls); facet_wrap(&r->fsf, &r->own, &rd_fsf_cls);
    return r;
}

// ---------------- CoCreateInstance ----------------
// 1 and *hr when the class is one of DirectShow's, 0 otherwise
int dshow_create(uint32_t clsid, uint32_t outer, uint32_t iid, uint32_t out, uint32_t *hr) {
    Owner *o = NULL;
    if (guid_at(clsid, &CLSID_FilterGraph) || guid_at(clsid, &CLSID_FilterGraphNoThread)) { Graph *g = outer ? NULL : graph_new(); o = g ? &g->own : NULL; }
    else if (guid_at(clsid, &CLSID_WMAsfReader)) { Reader *r = outer ? NULL : reader_new(); o = r ? &r->own : NULL; }
    else if (guid_at(clsid, &CLSID_MemoryAllocator)) { Alloc *a = outer ? NULL : alloc_new(); o = a ? &a->own : NULL; }
    else return 0;
    if (outer) { *hr = CLASS_E_NOAGGREGATION; return 1; }
    if (!o) { *hr = E_OUTOFMEMORY; return 1; }
    Guid g; memcpy(&g, G2H(iid), 16);
    uint32_t itf = 0;
    if (!o->qi(o, &g, &itf) || !itf) { guid_log("CoCreateInstance: no interface", &g); own_release(o); *hr = E_NOINTERFACE; return 1; }
    wr32(out, itf);   // the creation reference becomes the caller's
    T("CoCreateInstance %s -> 0x%x", o->name, itf);
    *hr = S_OK;
    return 1;
}
