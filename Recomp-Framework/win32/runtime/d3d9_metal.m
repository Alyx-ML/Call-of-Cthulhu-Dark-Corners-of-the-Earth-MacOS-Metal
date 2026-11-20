// Direct3D 9 on Metal.
//
// The guest's IDirect3D9, IDirect3DDevice9 and every resource interface are implemented here directly on Metal.
// Shaders (shader model 1-3) and the fixed-function pipeline become Metal Shading Language (d3d9_shader.c,
// d3d9_ffp.c), compiled the first time a draw needs them.
//
// Objects: each interface is a COM object (com32.h) whose host struct starts with Obj. Public references are the
// guest's AddRef/Release. Private references are the device's bindings (SetTexture, SetStreamSource, render
// targets...): an object the game released while it is still bound lives until it is unbound, as in Direct3D 9.
// Every object keeps a private reference on its device.
//
// Memory: vertex and index buffers live in guest memory wrapped by no-copy MTLBuffers, so Lock returns the guest
// address and the GPU reads what the game wrote. A lock of a buffer that the GPU can still read gets new storage
// (D3DLOCK_DISCARD: uninitialized; no flags: a copy of the old contents); the old storage is released when the GPU
// has finished with it. Textures are Metal textures: a lock gives the game a staging block in guest memory that
// the unlock copies into the texture, directly when the GPU does not use the texture, else with a blit in command
// order. SYSTEMMEM and SCRATCH resources stay in guest memory.
//
// Submission: one command buffer is recorded at a time and committed at Present (and when the CPU must wait for
// GPU results). Each command buffer has a serial; its completion handler records the last completed serial, which
// decides when memory can be used again. At most three frames are in flight.
//
// Threads: every method holds one recursive mutex.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "com32.h"
#include "d3d9_internal.h"
#include "d3d9_prelude.h"
#include "rt_enh.h"
#include <math.h>
#include <mach/mach_time.h>
#include <string.h>

void *rt_window_layer(uint32_t hwnd);
void rt_window_size(uint32_t hwnd, int *w, int *h);
void rt_cocoa_screen_size(int *w, int *h);
void rt_cocoa_set_size(uint32_t hwnd, int w, int h);
void rt_cocoa_full_screen(uint32_t hwnd, int on);
void rt_cocoa_layer_edr(void *layer, int on, int refresh_only);
float rt_cocoa_edr_headroom(void);
float rt_cocoa_edr_potential(void);
id<MTLDevice> rt_metal_device(void);
extern int rt_mouse_x, rt_mouse_y;

#define D3D_OK                  0u
#define S_FALSE_                1u
#define D3DOK_NOAUTOGEN         0x0876086Fu
#define D3DERR_INVALIDCALL      0x8876086Cu
#define D3DERR_NOTAVAILABLE     0x8876086Au
#define D3DERR_NOTFOUND         0x88760866u
#define D3DERR_MOREDATA         0x88760867u
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define E_NOINTERFACE_          0x80004002u
#define E_POINTER_              0x80004003u
#define E_OUTOFMEMORY_          0x8007000Eu

// D3DFORMAT
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
enum { FMT_UNKNOWN = 0, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24, FMT_A1R5G5B5 = 25,
       FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79,
       FMT_D16 = 80, FMT_D32F_LOCKABLE = 82, FMT_D24FS8 = 83, FMT_VERTEXDATA = 100, FMT_INDEX16 = 101,
       FMT_INDEX32 = 102 };
// D3DPOOL, D3DUSAGE, D3DLOCK, D3DRESOURCETYPE
enum { POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2, POOL_SCRATCH = 3 };
enum { USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2, USAGE_WRITEONLY = 8, USAGE_DYNAMIC = 0x200,
       USAGE_AUTOGENMIPMAP = 0x400, USAGE_DMAP = 0x4000, USAGE_QUERY_LEGACYBUMPMAP = 0x8000,
       USAGE_QUERY_SRGBREAD = 0x10000, USAGE_QUERY_FILTER = 0x20000, USAGE_QUERY_SRGBWRITE = 0x40000,
       USAGE_QUERY_POSTPIXELSHADER_BLENDING = 0x80000, USAGE_QUERY_VERTEXTEXTURE = 0x100000,
       USAGE_QUERY_WRAPANDMIP = 0x200000 };
enum { LOCK_READONLY = 0x10, LOCK_NOOVERWRITE = 0x1000, LOCK_DISCARD = 0x2000 };
enum { RTYPE_SURFACE = 1, RTYPE_VOLUME = 2, RTYPE_TEXTURE = 3, RTYPE_VOLUMETEXTURE = 4, RTYPE_CUBETEXTURE = 5,
       RTYPE_VERTEXBUFFER = 6, RTYPE_INDEXBUFFER = 7 };

static pthread_mutex_t g_mu;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void g_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mu, &a);
    pthread_mutexattr_destroy(&a);
}
// a COM method body that runs under the device mutex and an autorelease pool
#define METHOD(name) static uint32_t name##_(CPU *c, ComObj *self, const ComMethod *m); \
    static uint32_t name(CPU *c, ComObj *self, const ComMethod *m) { \
        pthread_mutex_lock(&g_mu); uint32_t r_; @autoreleasepool { r_ = name##_(c, self, m); } \
        pthread_mutex_unlock(&g_mu); return r_; } \
    static uint32_t name##_(CPU *c, ComObj *self, const ComMethod *m)

// log a message the first time a call site reaches it
#define LOG_ONCE(...) do { static int once_; if (!once_) { once_ = 1; rt_log(__VA_ARGS__); } } while (0)

static float fbits(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static uint32_t ubits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static void color4(uint32_t argb, float *o) {
    o[0] = (float)((argb >> 16) & 0xFF) / 255.0f; o[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(argb & 0xFF) / 255.0f; o[3] = (float)(argb >> 24) / 255.0f;
}

// ---------------- objects ----------------
typedef struct Device Device;
typedef struct Obj Obj;
typedef struct PrivData { uint8_t guid[16]; uint32_t flags, size; uint8_t *data; uint32_t unk; struct PrivData *next; } PrivData;
struct Obj {
    ComObj com;
    int priv;                     // private references
    Obj *container;               // a surface or volume of a texture: references count on the texture
    Device *dev;                  // creating device (NULL for the device itself and IDirect3D9)
    void (*destroy)(Obj *o);      // frees the host object (the guest side is freed before)
    void (*last_public)(Obj *o);  // optional: the last public reference went away
    PrivData *pd;
    int rtype;                    // D3DRESOURCETYPE (0: not a resource)
    uint32_t priority;
};
static void priv_unref(Obj *o);
static void pd_free(PrivData *p);

static void obj_free(Obj *o) {
    while (o->pd) { PrivData *n = o->pd->next; pd_free(o->pd); o->pd = n; }
    Device *d = o->dev;
    if (o->com.guest) com_unwrap_free(&o->com);
    o->destroy(o);
    if (d) priv_unref((Obj *)d);
}
static uint32_t obj_addref(Obj *o) {
    while (o->container) o = o->container;
    return (uint32_t)++o->com.refs;
}
static uint32_t obj_release(Obj *o) {
    while (o->container) o = o->container;
    if (o->com.refs <= 0) { rt_log("[d3d9] Release of %s without a reference", o->com.cls ? o->com.cls->name : "?"); return 0; }
    int r = --o->com.refs;
    if (r == 0) {
        o->priv++;   // keeps the object while it releases what it holds
        if (o->last_public) o->last_public(o);
        priv_unref(o);
    }
    return (uint32_t)r;
}
static void priv_ref(Obj *o) {
    if (!o) return;
    while (o->container) o = o->container;
    o->priv++;
}
static void priv_unref(Obj *o) {
    if (!o) return;
    while (o->container) o = o->container;
    if (--o->priv == 0 && o->com.refs == 0) obj_free(o);
}
// replace a private reference
#define REBIND(slot, obj) do { Obj *n_ = (Obj *)(obj), *o_ = (Obj *)(slot); if (n_ != o_) { priv_ref(n_); (slot) = (void *)n_; priv_unref(o_); } } while (0)

static uint32_t obj_wrap(Obj *o, ComClass *cls, Device *d, void (*destroy)(Obj *)) {
    o->dev = d; o->destroy = destroy;
    if (d) priv_ref((Obj *)d);
    return com_wrap(&o->com, cls);
}
// a host object behind a guest interface pointer, if it is one of `n` classes
static Obj *obj_of(uint32_t g, ComClass *const *cls, int n) {
    ComObj *o = com_host(g);
    if (!o) return NULL;
    for (int i = 0; i < n; i++) if (o->cls == cls[i]) return (Obj *)o;
    return NULL;
}

// ---- private data (IDirect3DResource9::SetPrivateData) ----
#define SPD_IUNKNOWN 1
static void guest_iunknown_call(uint32_t unk, int slot) {
    ComObj *h = com_host(unk);
    if (h) { if (slot == 1) obj_addref((Obj *)h); else obj_release((Obj *)h); return; }
    CPU *c = rt_cpu();
    uint32_t a[1] = {unk};
    guest_call(c, rd32(rd32(unk) + 4u * (uint32_t)slot), 1, a);
}
static void pd_free(PrivData *p) {
    if (p->flags & SPD_IUNKNOWN) guest_iunknown_call(p->unk, 2);
    free(p->data);
    free(p);
}

// ---- interface IDs ----
typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } Guid;
static const Guid IID_Unknown = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const Guid IID_D3D9 = {0x81BDCBCA, 0x64D4, 0x426D, {0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C}};
static const Guid IID_Device = {0xD0223B96, 0xBF7A, 0x43FD, {0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB}};
static const Guid IID_Resource = {0x05EEC05D, 0x8F7D, 0x4362, {0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04}};
static const Guid IID_VB = {0xB64BB1B5, 0xFD70, 0x4DF6, {0xBF, 0x91, 0x19, 0xD0, 0xA1, 0x24, 0x55, 0xE3}};
static const Guid IID_Volume = {0x24F416E6, 0x1F67, 0x4AA7, {0xB8, 0x8E, 0xD3, 0x3F, 0x6F, 0x31, 0x28, 0xA1}};
static const Guid IID_Swap = {0x794950F2, 0xADFC, 0x458A, {0x90, 0x5E, 0x10, 0xA1, 0x0B, 0x0B, 0x50, 0x3B}};
static const Guid IID_Surface = {0x0CFBAF3A, 0x9FF6, 0x429A, {0x99, 0xB3, 0xA2, 0x79, 0x6A, 0xF8, 0xB8, 0x9B}};
static const Guid IID_IB = {0x7C9DD65E, 0xD3F7, 0x4529, {0xAC, 0xEE, 0x78, 0x58, 0x30, 0xAC, 0xDE, 0x35}};
static const Guid IID_BaseTex = {0x580CA87E, 0x1D3C, 0x4D54, {0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE}};
static const Guid IID_Tex = {0x85C31227, 0x3DE5, 0x4F00, {0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5}};
static const Guid IID_Cube = {0xFFF32F81, 0xD953, 0x473A, {0x92, 0x23, 0x93, 0xD6, 0x52, 0xAB, 0xA9, 0x3F}};
static const Guid IID_VolTex = {0x2518526C, 0xE789, 0x4111, {0xA7, 0xB9, 0x47, 0xEF, 0x32, 0x8D, 0x13, 0xE6}};
static const Guid IID_Decl = {0xDD13C59C, 0x36FA, 0x4098, {0xA8, 0xFB, 0xC7, 0xED, 0x39, 0xDC, 0x85, 0x46}};
static const Guid IID_VS = {0xEFC5557E, 0x6265, 0x4613, {0x8A, 0x94, 0x43, 0x85, 0x78, 0x89, 0xEB, 0x36}};
static const Guid IID_PS = {0x6D3BDBDC, 0x5B02, 0x4415, {0xB8, 0x52, 0xCE, 0x5E, 0x8B, 0xCC, 0xB2, 0x89}};
static const Guid IID_SB = {0xB07C4FE5, 0x310D, 0x4BA8, {0xA2, 0x3C, 0x4F, 0x0F, 0x20, 0x6F, 0x21, 0x8B}};
static const Guid IID_Query = {0xD9771460, 0xA695, 0x4F26, {0xBB, 0xD3, 0x27, 0xB8, 0x40, 0xB5, 0x41, 0xCC}};
static int guid_eq(const void *g, const Guid *h) { return memcmp(g, h, 16) == 0; }

// ---------------- formats ----------------
typedef struct {
    uint32_t d3d;
    MTLPixelFormat mtl, srgb;      // srgb: the sRGB variant (D3DSAMP_SRGBTEXTURE, D3DRS_SRGBWRITEENABLE) or Invalid
    uint8_t bpp;                   // bytes per pixel, or per 4x4 block
    uint8_t block;                 // block-compressed
    uint8_t kind;                  // 0 color, 1 depth, 2 depth and stencil
    uint8_t rt;                    // can be a render target
    uint8_t blend;                 // render target blending
    uint8_t alpha;                 // has an alpha channel (else alpha reads 1 and is never written)
    uint8_t bump;                  // signed (D3DUSAGE_QUERY_LEGACYBUMPMAP)
    uint8_t f32;                   // 32-bit float channels (filtering depends on the GPU)
    uint8_t has_swz;
    MTLTextureSwizzleChannels swz; // how D3D9 reads the channels (missing ones are 1, luminance replicates)
} Fmt;
#define SWZ(r, g, b, a) {MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a}
#define NOSWZ 0, SWZ(Red, Green, Blue, Alpha)
#define FMT(d, m, s, bpp, blk, kind, rt, blend, alpha, bump, f32, ...) \
    {d, MTLPixelFormat##m, MTLPixelFormat##s, bpp, blk, kind, rt, blend, alpha, bump, f32, __VA_ARGS__}
static const Fmt fmt_table[] = {
    FMT(21, BGRA8Unorm, BGRA8Unorm_sRGB, 4, 0, 0, 1, 1, 1, 0, 0, NOSWZ),                  // A8R8G8B8
    FMT(22, BGRA8Unorm, BGRA8Unorm_sRGB, 4, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, Blue, One)),   // X8R8G8B8
    FMT(23, B5G6R5Unorm, Invalid, 2, 0, 0, 1, 1, 0, 0, 0, NOSWZ),                          // R5G6B5
    FMT(24, BGR5A1Unorm, Invalid, 2, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, Blue, One)),  // X1R5G5B5
    FMT(25, BGR5A1Unorm, Invalid, 2, 0, 0, 1, 1, 1, 0, 0, NOSWZ),                          // A1R5G5B5
    FMT(26, ABGR4Unorm, Invalid, 2, 0, 0, 0, 0, 1, 0, 0, 1, SWZ(Green, Blue, Alpha, Red)), // A4R4G4B4
    FMT(28, A8Unorm, Invalid, 1, 0, 0, 0, 0, 1, 0, 0, NOSWZ),                              // A8
    FMT(30, ABGR4Unorm, Invalid, 2, 0, 0, 0, 0, 0, 0, 0, 1, SWZ(Green, Blue, Alpha, One)), // X4R4G4B4
    FMT(31, RGB10A2Unorm, Invalid, 4, 0, 0, 1, 1, 1, 0, 0, NOSWZ),                         // A2B10G10R10
    FMT(32, RGBA8Unorm, RGBA8Unorm_sRGB, 4, 0, 0, 1, 1, 1, 0, 0, NOSWZ),                   // A8B8G8R8
    FMT(33, RGBA8Unorm, RGBA8Unorm_sRGB, 4, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, Blue, One)),   // X8B8G8R8
    FMT(34, RG16Unorm, Invalid, 4, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, One, One)),     // G16R16
    FMT(35, BGR10A2Unorm, Invalid, 4, 0, 0, 1, 1, 1, 0, 0, NOSWZ),                         // A2R10G10B10
    FMT(36, RGBA16Unorm, Invalid, 8, 0, 0, 1, 1, 1, 0, 0, NOSWZ),                          // A16B16G16R16
    FMT(50, R8Unorm, Invalid, 1, 0, 0, 0, 0, 0, 0, 0, 1, SWZ(Red, Red, Red, One)),         // L8
    FMT(51, RG8Unorm, Invalid, 2, 0, 0, 0, 0, 1, 0, 0, 1, SWZ(Red, Red, Red, Green)),      // A8L8
    FMT(60, RG8Snorm, Invalid, 2, 0, 0, 0, 0, 0, 1, 0, 1, SWZ(Red, Green, One, One)),      // V8U8
    FMT(63, RGBA8Snorm, Invalid, 4, 0, 0, 0, 0, 1, 1, 0, NOSWZ),                           // Q8W8V8U8
    FMT(64, RG16Snorm, Invalid, 4, 0, 0, 0, 0, 0, 1, 0, 1, SWZ(Red, Green, One, One)),     // V16U16
    FMT(81, R16Unorm, Invalid, 2, 0, 0, 0, 0, 0, 0, 0, 1, SWZ(Red, Red, Red, One)),        // L16
    FMT(110, RGBA16Snorm, Invalid, 8, 0, 0, 0, 0, 1, 1, 0, NOSWZ),                         // Q16W16V16U16
    FMT(111, R16Float, Invalid, 2, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, One, One, One)),       // R16F
    FMT(112, RG16Float, Invalid, 4, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, One, One)),    // G16R16F
    FMT(113, RGBA16Float, Invalid, 8, 0, 0, 1, 1, 1, 0, 0, NOSWZ),                         // A16B16G16R16F
    FMT(114, R32Float, Invalid, 4, 0, 0, 1, 0, 0, 0, 1, 1, SWZ(Red, One, One, One)),       // R32F
    FMT(115, RG32Float, Invalid, 8, 0, 0, 1, 0, 0, 0, 1, 1, SWZ(Red, Green, One, One)),    // G32R32F
    FMT(116, RGBA32Float, Invalid, 16, 0, 0, 1, 0, 1, 0, 1, NOSWZ),                        // A32B32G32R32F
    FMT(FOURCC('D', 'X', 'T', '1'), BC1_RGBA, BC1_RGBA_sRGB, 8, 1, 0, 0, 0, 1, 0, 0, NOSWZ),
    FMT(FOURCC('D', 'X', 'T', '2'), BC2_RGBA, BC2_RGBA_sRGB, 16, 1, 0, 0, 0, 1, 0, 0, NOSWZ),
    FMT(FOURCC('D', 'X', 'T', '3'), BC2_RGBA, BC2_RGBA_sRGB, 16, 1, 0, 0, 0, 1, 0, 0, NOSWZ),
    FMT(FOURCC('D', 'X', 'T', '4'), BC3_RGBA, BC3_RGBA_sRGB, 16, 1, 0, 0, 0, 1, 0, 0, NOSWZ),
    FMT(FOURCC('D', 'X', 'T', '5'), BC3_RGBA, BC3_RGBA_sRGB, 16, 1, 0, 0, 0, 1, 0, 0, NOSWZ),
    // depth formats: 24-bit depth is stored as 32-bit float (Apple GPUs have no 24-bit depth format)
    FMT(FMT_D16_LOCKABLE, Depth16Unorm, Invalid, 2, 0, 1, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D32, Depth32Float, Invalid, 4, 0, 1, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D15S1, Depth32Float_Stencil8, Invalid, 4, 0, 2, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D24S8, Depth32Float_Stencil8, Invalid, 4, 0, 2, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D24X8, Depth32Float, Invalid, 4, 0, 1, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D24X4S4, Depth32Float_Stencil8, Invalid, 4, 0, 2, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D16, Depth16Unorm, Invalid, 2, 0, 1, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D32F_LOCKABLE, Depth32Float, Invalid, 4, 0, 1, 0, 0, 0, 0, 0, NOSWZ),
    FMT(FMT_D24FS8, Depth32Float_Stencil8, Invalid, 4, 0, 2, 0, 0, 0, 0, 0, NOSWZ),
};
static const Fmt *fmt_find(uint32_t d3d) {
    for (size_t i = 0; i < sizeof fmt_table / sizeof fmt_table[0]; i++) if (fmt_table[i].d3d == d3d) return &fmt_table[i];
    return NULL;
}
// Storage that differs from the Direct3D format (rt_enh.h). Reduced banding: 32-bit color render targets keep 16 bits
// per channel; a unorm format keeps the [0, 1] range and the clamping of the 8-bit format. The back buffer of a swap
// chain is half float instead: it also keeps the scene's colors above white (additive blending), which HDR output and
// tone mapping use (tm_pre). Sharp shadows: the shadow targets of d3d9_enh_shadow_target (R5G6B5) keep 8 bits per
// channel. The Direct3D format (Tex.fmt) does not change.
static const Fmt fmt_hp_argb = FMT(21, RGBA16Unorm, Invalid, 8, 0, 0, 1, 1, 1, 0, 0, NOSWZ);
static const Fmt fmt_hp_xrgb = FMT(22, RGBA16Unorm, Invalid, 8, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, Blue, One));
static const Fmt fmt_fp_argb = FMT(21, RGBA16Float, Invalid, 8, 0, 0, 1, 1, 1, 0, 0, NOSWZ);
static const Fmt fmt_fp_xrgb = FMT(22, RGBA16Float, Invalid, 8, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, Blue, One));
static const Fmt fmt_sh_rgb = FMT(23, BGRA8Unorm, Invalid, 4, 0, 0, 1, 1, 0, 0, 0, 1, SWZ(Red, Green, Blue, One));
static struct { uint32_t w, h, fmt, scale; } enh_sh;   // d3d9_enh_shadow_target
void d3d9_enh_shadow_target(uint32_t w, uint32_t h, uint32_t fmt, uint32_t scale) {
    if (!w || !h || fmt != 23 || scale < 2 || scale > 8 || (uint64_t)w * scale > 16384 || (uint64_t)h * scale > 16384)
        rt_fatal("[d3d9] d3d9_enh_shadow_target: bad shadow target %ux%u, format %u, scale %u", w, h, fmt, scale);
    enh_sh.w = w; enh_sh.h = h; enh_sh.fmt = fmt; enh_sh.scale = scale;
}
// bytes per row and rows of a w x h image (block formats: rows of 4x4 blocks)
static uint32_t fmt_pitch(const Fmt *f, uint32_t w) { return f->block ? ((w + 3) / 4) * f->bpp : w * f->bpp; }
static uint32_t fmt_rows(const Fmt *f, uint32_t h) { return f->block ? (h + 3) / 4 : h; }
static uint32_t mip(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }

// ---------------- device structures ----------------
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; uint64_t done; } Sync;
typedef struct { uint64_t serial; int kind; uint32_t a, b; void *ref; } Deferred;   // see process_deferred
// guest-memory storage of a vertex or index buffer: a block of a slab (cls >= 0) or its own region (cls < 0)
typedef struct { uint32_t g, size; int cls; uint32_t slab; void *buf; uint32_t off; } Store;
#define NCLS 9                     // slab block sizes 256 B .. 64 KiB
#define SLAB_SIZE (4u << 20)
typedef struct { void *buf; uint8_t *p; uint32_t used; uint64_t serial; } Chunk;
#define CHUNK_SIZE (4u << 20)
#define RING_SOFT_CAP 64u          // chunks kept before waiting for the GPU to free one
#define VIS_SLOTS 4096u

typedef struct Tex Tex;
typedef struct Surf {
    Obj o;
    Tex *tex;                      // the texture holding the pixels (standalone surfaces own theirs)
    uint32_t face, level, w, h, d; // d > 1 or volume: a level of a volume texture
    int volume;
    int locked; uint32_t lflags, lmem, lpitch, lslice, lx, ly, lz, lw, lh, ld;
    struct Swap *swap;             // back buffers: the swap chain (GetContainer)
} Surf;
struct Tex {
    Obj o;
    uint32_t fmt, usage, pool, w, h, d, levels, mtl_levels, faces, ms, msq;
    uint32_t sc;                   // Metal pixels per Direct3D pixel in x and y (1, or the d3d9_enh_shadow_target scale)
    int cube, volume, lockable, discard;
    const Fmt *f;                  // the storage format (fmt_find(fmt), or an rt_enh.h storage format)
    id<MTLTexture> mtl;            // storage (multisampled for MSAA surfaces)
    id<MTLTexture> resolve;        // MSAA surfaces: the resolved image
    id<MTLTexture> view;           // sampling view with the D3D9 channel mapping (lazily)
    id<MTLTexture> srgb;           // sRGB sampling view (lazily)
    id<MTLTexture> rtsrgb;         // sRGB render target view (lazily)
    uint32_t *sys;                 // SYSTEMMEM / SCRATCH: guest storage of each subresource (face * levels + level)
    uint8_t *init;                 // GPU storage: subresource written at least once
    Surf **subs;                   // surface / volume objects of the subresources (lazily)
    uint64_t last_use, last_write;
    uint32_t lod, filter;
    int mips_dirty, nlocked;
};
typedef struct VB {
    Obj o;
    uint32_t len, usage, fmt, pool, fvf;
    Store st;
    uint64_t last_use;
    int nlocked;
} VB;
typedef struct Decl {
    Obj o;
    uint32_t n;                    // elements without the end marker
    struct { uint16_t stream, offset; uint8_t type, method, usage, index; } el[64];
    uint32_t fvf;
} Decl;
@interface D3D9Fn : NSObject {
@public
    id<MTLFunction> fn;            // nil: the compilation failed (logged once)
    uint64_t fid;
    uint32_t samplers;
    uint8_t stype[16];
}
@end
@implementation D3D9Fn
@end
typedef struct PVar { ShaderVariant key; ShaderInfo info; int ok; D3D9Fn *fn[4]; } PVar;
#define MAX_PVAR 64
#define ENH_LC_MAX 16
typedef struct Shader {
    Obj o;
    int ps;
    uint32_t *tok, ntok;
    ShaderInfo info;               // translation without a variant (vertex shaders, shader model 2-3 pixel shaders)
    NSMutableDictionary *fns;      // vertex shaders: input layout -> D3D9Fn
    PVar *var; uint32_t nvar;      // pixel shaders: per variant
    int lv;                        // vertex shaders: 1 when the constants of d3d9_enh_light_vs are in lv_dir, lv_pos, lv_grad
    uint32_t lv_dir, lv_pos, lv_grad;
    int ui;                        // vertex shaders: 1 for a shader of d3d9_enh_ui_vs (its screen position in c[ui_creg])
    uint32_t ui_creg;
} Shader;
typedef struct Query {
    Obj o;
    uint32_t type;
    int issued;
    uint64_t serial;
    uint32_t *slots; uint32_t nslots, capslots;   // occlusion: visibility result slots not yet counted
    uint64_t acc;                                 // occlusion: samples counted from released slots
    int active;
} Query;
typedef struct Swap {
    Obj o;
    uint32_t pp[14];
    uint32_t hwnd;
    CAMetalLayer *layer;
    Surf *bb;
    id<MTLTexture> front;          // the last presented image (GetFrontBufferData)
    uint16_t ramp[768];
    int implicit;
    int edr;                       // the layer was switched to EDR output (rt_enh.h hdr)
    uint32_t pace_rate;            // frame pacing (pace_*): frames per second, 0 = no pacing
    uint64_t pace_due;             // the time (CLOCK_UPTIME_RAW, ns) at which the next frame starts
} Swap;

typedef struct { uint32_t idx; uint32_t data[26]; uint8_t enabled; } Light;
typedef struct State {
    uint32_t rs[256];
    uint32_t tss[8][33];
    uint32_t samp[20][14];
    Obj *tex[20];
    Shader *vs, *ps;
    Decl *decl;
    uint32_t fvf;
    struct { VB *vb; uint32_t off, stride, freq; } stream[16];
    VB *ib;
    float vsf[256][4]; int32_t vsi[16][4]; uint32_t vsb[16];
    float psf[224][4]; int32_t psi[16][4]; uint32_t psb[16];
    float xf[512][16];
    uint32_t vp[6];
    int32_t scissor[4];
    float material[17];
    float clip[8][4];
    Light *lights; uint32_t nlights, caplights;
    uint32_t active[8]; uint32_t nactive;
    float npatch;
    uint32_t clipstatus[2];
    uint32_t cur_palette;
    int swvp;
} State;
// what a state block holds
typedef struct {
    uint64_t rs[4], tss[8], vsf[4], psf[4], xf[8];
    uint32_t samp[20], tex, streams, freq;
    uint16_t vsi, vsb, psi, psb;
    uint8_t vs, ps, decl, ib, vp, scissor, material, clip, npatch;
    uint8_t lights_all;
    uint32_t *light_idx; uint32_t nlight_idx, caplight_idx;
} Mask;
typedef struct SB { Obj o; uint32_t type; State st; Mask mk; } SB;

struct Device {
    Obj o;
    Obj *d3d;
    uint32_t adapter, devtype, focus, behavior;
    id<MTLDevice> mtl;
    id<MTLCommandQueue> queue;
    id<MTLCommandBuffer> cmd;
    id<MTLRenderCommandEncoder> enc;
    id<MTLBlitCommandEncoder> blit;
    Sync *sy;
    uint64_t serial, submitted;
    Deferred *defq; uint32_t ndef, capdef;
    struct { uint32_t g; void *buf; } *slabs; uint32_t nslabs, capslabs;
    uint64_t *freel[NCLS]; uint32_t nfree[NCLS], capfree[NCLS];
    Chunk *chunks; uint32_t nchunks, capchunks; int curchunk;
    NSMutableDictionary *pso_cache, *dss_cache, *samp_cache, *ff_fns, *util;
    uint64_t next_fid;
    id<MTLLibrary> util_lib;
    id<MTLTexture> dummy[5];       // unbound samplers read (0, 0, 0, 1): [2] 2D, [3] cube, [4] 3D
    id<MTLBuffer> vis; uint64_t vis_serial[VIS_SLOTS]; uint32_t vis_next; struct Query *vis_owner[VIS_SLOTS];
    Swap *swap;
    Surf *rt[4], *ds;
    struct { int active; uint32_t flags; float color[4]; float z; uint32_t stencil; } pend;
    uint32_t pass_w, pass_h, pass_ms;
    State st;
    SB *rec;
    int in_scene;
    struct { uint32_t fvf; Decl *decl; } *fvfd; uint32_t nfvfd, capfvfd;
    Query *occl;
    uint32_t frame; uint64_t frame_serial[3];
    uint64_t stat_draws, stat_skipped;   // RT_VERBOSE frame statistics: draws issued / not drawn since the last report
    // per-encoder state
    __unsafe_unretained id<MTLRenderPipelineState> cur_pso;
    __unsafe_unretained id<MTLDepthStencilState> cur_dss;
    // constant uploads (reused while nothing changed in the same command buffer)
    int vsc_dirty, psc_dirty, ffc_dirty;
    struct { void *buf; uint32_t off; uint64_t serial; } vsc_up, psc_up, ffc_up;
    // cursor (SetCursorProperties)
    id<MTLTexture> cursor; int cursor_show, cur_x, cur_y, hot_x, hot_y, cur_w, cur_h, last_mx, last_my;
    // palettes (stored; paletted formats are not offered)
    struct { uint32_t n; uint8_t e[1024]; } *pal; uint32_t npal, cappal;
    uint32_t caps_ms_counts[4]; uint32_t n_ms;
    id<MTLSamplerState> samp_lin, samp_pt;   // utility copies
    Surf *auto_ds;                 // the automatic depth-stencil surface
    uint32_t pp[14];               // creation / last Reset presentation parameters
    // scene enhancements (rt_enh.h): frame numbers are stored as frame + 1, so 0 means "never"
    uint32_t enh_done;             // the frame in which the passes ran
    uint32_t enh_scene;            // the frame of the captured scene
    uint32_t enh_light_frame;      // the frame of the captured light
    EnhScene enh_sc;
    id<MTLTexture> enh_color, enh_depth;
    // d3d9_enh_light_vs: the lights of this frame's draws (one per world position) and the light of the contact shadows
    struct { float pos[3], dir[3], r; } enh_lc[ENH_LC_MAX];
    uint32_t enh_nlc, enh_lc_frame;
    float enh_lv[4];               // the light (homogeneous world position or direction, as EnhScene.light)
    int enh_lv_ok;                 // enh_lv holds a light
    // d3d9_enh_camera_vs: the cameras of this frame's opaque back-buffer draws and their draw counts
    struct { float view[16], proj[16]; uint32_t n; } enh_cam[4];
    uint32_t enh_ncam, enh_cam_frame;
    int enh_cam_logged;
    // HDR output and tone mapping (rt_enh.h): the scene of the back buffer before the UI (tm_note)
    id<MTLTexture> tm_ref;         // a copy of the back buffer at the last change from scene draws to UI draws
    uint32_t tm_scene;             // the frame of the last scene draw into the back buffer
    uint32_t tm_ref_frame;         // the frame of tm_ref
    uint32_t tm_copies;            // the copies into tm_ref in tm_ref_frame
    int tm_ui;                     // the last draw into the back buffer was a UI draw
    id<MTLTexture> tm_pre;         // the half-float back buffer at the end of the opaque scene, after ambient occlusion,
                                   //   contact shadows and bloom, before the game's post-processing (enh_apply)
    uint32_t tm_pre_frame;         // the frame of tm_pre
    struct { float headroom, potential; int peak, paper; } tm_logged;
    // temporal anti-aliasing (rt_enh.h aa): the 3D scene draws of a frame move by a sub-pixel offset (taa_offset)
    float taa_jit[2];              // the offset (pixels, y down) in the vertex constants uploaded last (pos_fixup)
    float taa_lod;                 // the texture LOD bias of temporal anti-aliasing in the pixel constants uploaded last
                                   //    (ps_consts_header): TAA_LOD_BIAS for a scene draw with an offset, otherwise 0
    int taa_lod_logged;            // the first draw with the LOD bias is in the log
    // the interface in 4:3 (d3d9_enh_ui_vs): the horizontal scale of this draw and of the uploaded vertex constants
    // (pos_fixup), 1 = the full width
    float ui_k, ui_k_up;
    int ui_logged;                 // the first draw in 4:3 is in the log
    uint32_t taa_frame;            // the last frame with anti-aliasing
    float taa_vp[16];              // the view-projection of the camera of the last anti-aliasing frame
    int taa_vp_ok;                 // taa_vp holds a camera
};

// ---------------- submission ----------------
static uint64_t done_serial(Device *d) { return __atomic_load_n(&d->sy->done, __ATOMIC_ACQUIRE); }

static void defer_push(Device *d, Deferred e) {
    if (d->ndef == d->capdef) {
        uint32_t cap = d->capdef ? d->capdef * 2 : 256;
        Deferred *q = realloc(d->defq, cap * sizeof *q);
        if (!q) rt_fatal("[d3d9] out of memory for deferred releases");
        d->defq = q; d->capdef = cap;
    }
    d->defq[d->ndef++] = e;
}
static void slab_block_free(Device *d, uint32_t g, int cls, uint32_t slab) {
    if (d->nfree[cls] == d->capfree[cls]) {
        uint32_t cap = d->capfree[cls] ? d->capfree[cls] * 2 : 64;
        uint64_t *q = realloc(d->freel[cls], cap * sizeof *q);
        if (!q) rt_fatal("[d3d9] out of memory for buffer storage");
        d->freel[cls] = q; d->capfree[cls] = cap;
    }
    d->freel[cls][d->nfree[cls]++] = ((uint64_t)slab << 32) | g;
}
// releases whose command buffers completed: kind 0 slab block (a guest, b class << 24 | slab), kind 1 region
// (a guest, ref the CF-retained MTLBuffer)
static void process_deferred(Device *d) {
    uint64_t done = done_serial(d);
    uint32_t k = 0;
    for (uint32_t i = 0; i < d->ndef; i++) {
        Deferred *e = &d->defq[i];
        if (e->serial > done) { d->defq[k++] = *e; continue; }
        if (e->kind == 0) slab_block_free(d, e->a, (int)(e->b >> 24), e->b & 0xFFFFFF);
        else { CFRelease(e->ref); gvm_free(e->a); }
    }
    d->ndef = k;
}
static void ensure_cmd(Device *d) {
    if (d->cmd) return;
    process_deferred(d);
    d->cmd = [d->queue commandBuffer];
    if (!d->cmd) rt_fatal("[d3d9] Metal did not create a command buffer");
    d->serial = d->submitted + 1;
}
static uint64_t cur_serial(Device *d) { ensure_cmd(d); return d->serial; }
static void begin_pass_flush_clear(Device *d);
static void end_pass(Device *d) {
    if (d->enc) { [d->enc endEncoding]; d->enc = nil; }
}
static void end_encoders(Device *d) {
    end_pass(d);
    if (d->blit) { [d->blit endEncoding]; d->blit = nil; }
}
static void commit(Device *d) {
    if (d->pend.active) begin_pass_flush_clear(d);
    if (!d->cmd) return;
    end_encoders(d);
    uint64_t s = d->serial;
    Sync *sy = d->sy;
    [d->cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.status == MTLCommandBufferStatusError)
            rt_log("[d3d9] GPU command buffer %llu failed: %s", (unsigned long long)s, cb.error.localizedDescription.UTF8String);
        pthread_mutex_lock(&sy->mu);
        if (s > sy->done) __atomic_store_n(&sy->done, s, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&sy->cv);
        pthread_mutex_unlock(&sy->mu);
    }];
    [d->cmd commit];
    d->cmd = nil;
    d->submitted = s;
}
// wait until the GPU completed command buffer `s` (committing it first when it is the one being recorded)
static void wait_serial(Device *d, uint64_t s) {
    if (!s) return;
    if (d->cmd && s >= d->serial) commit(d);
    pthread_mutex_lock(&d->sy->mu);
    while (d->sy->done < s) pthread_cond_wait(&d->sy->cv, &d->sy->mu);
    pthread_mutex_unlock(&d->sy->mu);
}
static id<MTLBlitCommandEncoder> get_blit(Device *d) {
    if (d->pend.active) begin_pass_flush_clear(d);
    ensure_cmd(d);
    end_pass(d);
    if (!d->blit) d->blit = [d->cmd blitCommandEncoder];
    return d->blit;
}

// ---------------- guest-memory buffer storage ----------------
static int store_alloc(Device *d, uint32_t size, Store *out) {
    memset(out, 0, sizeof *out);
    if (!size) size = 1;
    if (size <= (256u << (NCLS - 1))) {
        int cls = 0;
        while ((256u << cls) < size) cls++;
        uint32_t bs = 256u << cls;
        if (!d->nfree[cls]) {
            uint32_t g = gvm_alloc(0, SLAB_SIZE, 1, 4);
            if (!g) return 0;
            id<MTLBuffer> b = [d->mtl newBufferWithBytesNoCopy:G2H(g) length:SLAB_SIZE options:MTLResourceStorageModeShared deallocator:nil];
            if (!b) { gvm_free(g); return 0; }
            if (d->nslabs == d->capslabs) {
                uint32_t cap = d->capslabs ? d->capslabs * 2 : 16;
                void *q = realloc(d->slabs, cap * sizeof *d->slabs);
                if (!q) { gvm_free(g); return 0; }
                d->slabs = q; d->capslabs = cap;
            }
            d->slabs[d->nslabs].g = g; d->slabs[d->nslabs].buf = (void *)CFBridgingRetain(b);
            for (uint32_t o = SLAB_SIZE; o >= bs; o -= bs) slab_block_free(d, g + o - bs, cls, d->nslabs);
            d->nslabs++;
        }
        uint64_t e = d->freel[cls][--d->nfree[cls]];
        out->g = (uint32_t)e; out->slab = (uint32_t)(e >> 32); out->cls = cls; out->size = size;
        out->buf = d->slabs[out->slab].buf; out->off = out->g - d->slabs[out->slab].g;
        return 1;
    }
    uint32_t rs = (uint32_t)(((uint64_t)size + 0xFFFF) & ~0xFFFFull);
    if (rs < size) return 0;
    uint32_t g = gvm_alloc(0, rs, 1, 4);
    if (!g) return 0;
    id<MTLBuffer> b = [d->mtl newBufferWithBytesNoCopy:G2H(g) length:rs options:MTLResourceStorageModeShared deallocator:nil];
    if (!b) { gvm_free(g); return 0; }
    out->g = g; out->size = size; out->cls = -1; out->buf = (void *)CFBridgingRetain(b); out->off = 0;
    return 1;
}
// release storage once the GPU finished command buffer `serial`
static void store_free(Device *d, Store *s, uint64_t serial) {
    if (!s->g) return;
    Deferred e = {serial, s->cls >= 0 ? 0 : 1, s->g, s->cls >= 0 ? ((uint32_t)s->cls << 24 | s->slab) : 0, s->cls >= 0 ? NULL : s->buf};
    if (serial > done_serial(d)) defer_push(d, e);
    else if (e.kind == 0) slab_block_free(d, e.a, s->cls, s->slab);
    else { CFRelease(e.ref); gvm_free(e.a); }
    memset(s, 0, sizeof *s);
}

// ---------------- per-draw data ring ----------------
// `size` bytes for the command buffer being recorded; returns the CPU pointer, the buffer and the offset
static void *ring_alloc(Device *d, uint32_t size, __unsafe_unretained id<MTLBuffer> *buf, uint32_t *off) {
    uint64_t s = cur_serial(d);
    size = (size + 255u) & ~255u;
    if (size > CHUNK_SIZE) {
        // larger than a chunk: its own buffer, kept alive by the command buffer that uses it
        id<MTLBuffer> b = [d->mtl newBufferWithLength:size options:MTLResourceStorageModeShared];
        if (!b) rt_fatal("[d3d9] out of GPU memory (%u bytes)", size);
        CFAutorelease(CFBridgingRetain(b));
        *buf = b; *off = 0;
        return b.contents;
    }
    Chunk *c = d->curchunk >= 0 ? &d->chunks[d->curchunk] : NULL;
    if (!c || c->used + size > CHUNK_SIZE) {
        uint64_t done = done_serial(d);
        int pick = -1;
        uint64_t oldest = UINT64_MAX; int oldest_i = -1;
        for (uint32_t i = 0; i < d->nchunks; i++) {
            if ((int)i == d->curchunk) continue;
            if (d->chunks[i].serial <= done) { pick = (int)i; break; }
            if (d->chunks[i].serial < oldest) { oldest = d->chunks[i].serial; oldest_i = (int)i; }
        }
        if (pick < 0 && d->nchunks >= RING_SOFT_CAP && oldest_i >= 0 && oldest < s) {
            wait_serial(d, oldest);   // submitted earlier: waiting does not end the command buffer being recorded
            pick = oldest_i;
        }
        if (pick < 0) {
            if (d->nchunks == d->capchunks) {
                uint32_t cap = d->capchunks ? d->capchunks * 2 : 8;
                Chunk *q = realloc(d->chunks, cap * sizeof *q);
                if (!q) rt_fatal("[d3d9] out of memory for the draw data ring");
                d->chunks = q; d->capchunks = cap;
            }
            id<MTLBuffer> b = [d->mtl newBufferWithLength:CHUNK_SIZE options:MTLResourceStorageModeShared];
            if (!b) rt_fatal("[d3d9] out of GPU memory for the draw data ring");
            d->chunks[d->nchunks] = (Chunk){(void *)CFBridgingRetain(b), b.contents, 0, 0};
            pick = (int)d->nchunks++;
        }
        d->curchunk = pick;
        c = &d->chunks[pick];
        c->used = 0;
    }
    c->serial = s;
    *buf = (__bridge id<MTLBuffer>)c->buf;
    *off = c->used;
    c->used += size;
    return c->p + *off;
}
// ---------------- textures and surfaces ----------------
static ComClass c_d3d9, c_device, c_texture, c_cube, c_voltex, c_volume, c_surface, c_vb, c_ib, c_decl, c_vs, c_ps,
                c_query, c_sb, c_swap;
typedef struct { int locked; uint32_t flags, mem, pitch, slice, x, y, z, w, h, d; } LockSt;
static LockSt *tex_locks(Tex *t);

static int fmt_filterable(Device *d, const Fmt *f) {
    if (f->kind) return 0;
    if (f->f32) return d->mtl.supports32BitFloatFiltering;
    return 1;
}
static uint32_t full_levels(uint32_t w, uint32_t h, uint32_t dd) {
    uint32_t m = w > h ? w : h; if (dd > m) m = dd;
    uint32_t n = 1; while (m > 1) { m >>= 1; n++; }
    return n;
}
static void tex_free_storage(Tex *t) {
    uint32_t n = t->faces * t->levels;
    if (t->subs) for (uint32_t i = 0; i < n; i++) if (t->subs[i]) {
        Surf *sub = t->subs[i];
        while (sub->o.pd) { PrivData *nx = sub->o.pd->next; pd_free(sub->o.pd); sub->o.pd = nx; }
        if (sub->o.com.guest) com_unwrap_free(&sub->o.com);
        free(sub);
    }
    LockSt *L = tex_locks(t);
    if (L) for (uint32_t i = 0; i < n; i++) if (L[i].locked && L[i].mem && !t->sys) gfree(L[i].mem);
    if (t->sys) for (uint32_t i = 0; i < n; i++) gfree(t->sys[i]);
    free(t->sys); free(t->init); free(t->subs); free(L);
    t->mtl = nil; t->resolve = nil; t->view = nil; t->srgb = nil; t->rtsrgb = nil;
}
// lock state per subresource lives right after the subresource arrays (allocated with them)
typedef struct { Tex t; LockSt *locks; } TexL;
static LockSt *tex_locks(Tex *t) { return ((TexL *)t)->locks; }

// a texture (or the storage of a standalone surface); not yet a COM object
static Tex *tex_new(Device *d, uint32_t w, uint32_t h, uint32_t dd, uint32_t levels, uint32_t usage, uint32_t fmt,
                    uint32_t pool, int cube, int volume, uint32_t ms, uint32_t msq, int standalone, int lockable, int bb, uint32_t *err) {
    *err = D3DERR_INVALIDCALL;
    const Fmt *f = fmt_find(fmt);
    if (!f) { rt_log("[d3d9] format 0x%x is not supported", fmt); return NULL; }
    if (!w || !h || !dd || w > 16384 || h > 16384 || dd > 2048) return NULL;
    if ((usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && pool != POOL_DEFAULT) return NULL;
    if ((usage & USAGE_DYNAMIC) && pool == POOL_MANAGED) return NULL;
    if ((usage & USAGE_RENDERTARGET) && !f->rt) return NULL;
    if ((usage & USAGE_DEPTHSTENCIL) && !f->kind) return NULL;
    if (f->kind && !(usage & USAGE_DEPTHSTENCIL)) return NULL;   // depth formats are depth-stencil surfaces only
    if (f->kind && (!standalone || cube || volume)) return NULL;
    if (volume && (f->block || f->kind)) return NULL;
    uint32_t maxl = full_levels(w, h, volume ? dd : 1);
    if (!levels) levels = maxl;
    if (levels > maxl) return NULL;
    uint32_t mtl_levels = levels;
    if (usage & USAGE_AUTOGENMIPMAP) {
        if (f->rt && fmt_filterable(d, f) && !volume && pool != POOL_SYSTEMMEM && pool != POOL_SCRATCH) { levels = 1; mtl_levels = maxl; }
        else { rt_log("[d3d9] no automatic mipmaps for format 0x%x", fmt); usage &= ~(uint32_t)USAGE_AUTOGENMIPMAP; }
    }
    TexL *tl = calloc(1, sizeof *tl);
    if (!tl) { *err = E_OUTOFMEMORY_; return NULL; }
    Tex *t = &tl->t;
    t->fmt = fmt; t->usage = usage; t->pool = pool; t->w = w; t->h = h; t->d = volume ? dd : 1; t->levels = levels;
    t->mtl_levels = mtl_levels; t->faces = cube ? 6 : 1; t->cube = cube; t->volume = volume; t->f = f;
    t->ms = ms > 1 ? ms : 1; t->msq = msq; t->filter = 2;   // D3DTEXF_LINEAR
    t->sc = 1;
    // rt_enh.h storage of render targets the CPU never reads or writes directly (not lockable)
    if (rt_enh && pool == POOL_DEFAULT && (usage & USAGE_RENDERTARGET) && !lockable && !volume) {
        if (rt_enh->shadows == 1 && enh_sh.w && w == enh_sh.w && h == enh_sh.h && fmt == enh_sh.fmt && !cube && t->ms == 1 && levels == 1) {
            f = &fmt_sh_rgb; t->sc = enh_sh.scale;
            if (rt_verbose) rt_log("[d3d9] shadow target %ux%u (format %u) stored at %ux%u, 8 bits per channel", w, h, fmt, w * t->sc, h * t->sc);
        } else if (rt_enh->banding == 1 && (fmt == 21 || fmt == 22)) {
            f = bb ? (fmt == 21 ? &fmt_fp_argb : &fmt_fp_xrgb) : (fmt == 21 ? &fmt_hp_argb : &fmt_hp_xrgb);
            if (rt_verbose) rt_log("[d3d9] %s %ux%u (format %u) stored with 16 bits per channel%s", bb ? "back buffer" : "render target",
                                   w, h, fmt, bb ? " (half float)" : "");
        }
    }
    t->f = f;
    uint32_t n = t->faces * t->levels;
    t->init = calloc(n, 1); t->subs = calloc(n, sizeof *t->subs); tl->locks = calloc(n, sizeof(LockSt));
    if (!t->init || !t->subs || !tl->locks) { tex_free_storage(t); free(tl); *err = E_OUTOFMEMORY_; return NULL; }
    if (pool == POOL_SYSTEMMEM || pool == POOL_SCRATCH) {
        if (f->kind) { tex_free_storage(t); free(tl); return NULL; }
        t->sys = calloc(n, sizeof *t->sys);
        if (!t->sys) { tex_free_storage(t); free(tl); *err = E_OUTOFMEMORY_; return NULL; }
        for (uint32_t fa = 0; fa < t->faces; fa++) for (uint32_t l = 0; l < levels; l++) {
            uint64_t sz = (uint64_t)fmt_pitch(f, mip(w, l)) * fmt_rows(f, mip(h, l)) * mip(t->d, l);
            uint32_t g = sz <= 0x7FFFFFFFu ? gcalloc(1, (uint32_t)sz) : 0;
            if (!g) { tex_free_storage(t); free(tl); *err = E_OUTOFMEMORY_; return NULL; }
            t->sys[fa * levels + l] = g;
        }
        return t;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = volume ? MTLTextureType3D : cube ? MTLTextureTypeCube : t->ms > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    td.pixelFormat = f->mtl;
    td.width = w * t->sc; td.height = h * t->sc; td.depth = t->d;
    td.mipmapLevelCount = mtl_levels;
    td.sampleCount = t->ms;
    int rt = (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) || (standalone && f->rt && pool == POOL_DEFAULT) ||
             (usage & USAGE_AUTOGENMIPMAP);
    td.usage = MTLTextureUsageShaderRead | (rt ? MTLTextureUsageRenderTarget : 0) |
               ((f->has_swz || f->srgb != MTLPixelFormatInvalid) ? MTLTextureUsagePixelFormatView : 0);
    td.storageMode = (f->kind || t->ms > 1 || (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL))) ? MTLStorageModePrivate : MTLStorageModeShared;
    t->mtl = [d->mtl newTextureWithDescriptor:td];
    if (t->mtl && t->ms > 1 && !f->kind) {
        td.textureType = MTLTextureType2D; td.sampleCount = 1;
        t->resolve = [d->mtl newTextureWithDescriptor:td];
    }
    if (!t->mtl || (t->ms > 1 && !f->kind && !t->resolve)) {
        rt_log("[d3d9] Metal did not create a %ux%ux%u texture (format 0x%x, %u levels, %u samples)", w, h, t->d, fmt, mtl_levels, t->ms);
        tex_free_storage(t); free(tl); *err = D3DERR_OUTOFVIDEOMEMORY; return NULL;
    }
    // render targets start cleared like fresh video memory; alpha-less formats keep alpha at 1
    *err = D3D_OK;
    return t;
}
static void tex_destroy(Obj *o) { Tex *t = (Tex *)o; tex_free_storage(t); free(t); }

// the sampling view: the D3D9 channel mapping, sRGB when asked
static id<MTLTexture> tex_view(Tex *t, int srgb) {
    id<MTLTexture> base = t->ms > 1 ? t->resolve : t->mtl;
    if (srgb && t->f->srgb == MTLPixelFormatInvalid && fmt_find(t->fmt)->srgb != MTLPixelFormatInvalid)
        LOG_ONCE("[d3d9] sRGB read of a render target stored with 16 bits per channel (Reduced Banding): no sRGB conversion");
    if (srgb && t->f->srgb != MTLPixelFormatInvalid) {
        if (!t->srgb) t->srgb = [base newTextureViewWithPixelFormat:t->f->srgb textureType:base.textureType
                                                              levels:NSMakeRange(0, base.mipmapLevelCount)
                                                              slices:NSMakeRange(0, base.textureType == MTLTextureTypeCube ? 6 : 1)
                                                             swizzle:t->f->swz];
        return t->srgb;
    }
    if (!t->f->has_swz) return base;
    if (!t->view) t->view = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:base.textureType
                                                         levels:NSMakeRange(0, base.mipmapLevelCount)
                                                         slices:NSMakeRange(0, base.textureType == MTLTextureTypeCube ? 6 : 1)
                                                        swizzle:t->f->swz];
    return t->view;
}

static id<MTLRenderCommandEncoder> util_pass(Device *d, id<MTLTexture> tex, NSUInteger level, NSUInteger slice, int clear,
                                             MTLClearColor cc);
static void util_quad(Device *d, id<MTLRenderCommandEncoder> e, char tag, int blend, MTLPixelFormat dfmt, id<MTLTexture> src,
                      id<MTLSamplerState> smp, const int32_t *sr, uint32_t sw, uint32_t sh, const int32_t *dr, uint32_t dw, uint32_t dh,
                      __unsafe_unretained id<MTLBuffer> fbuf, uint32_t foff, const uint32_t *fbytes);
// ---- storage in another format or size than the Direct3D format (rt_enh.h) ----
// CPU reads and writes of such a texture go through a temporary texture in the Direct3D format and size, which a
// render pass copies from or into the storage
static int tex_alt(const Tex *t) { return t->sc != 1 || t->f != fmt_find(t->fmt); }
static id<MTLTexture> alt_temp(Device *d, Tex *t, uint32_t w, uint32_t h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt_find(t->fmt)->mtl width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> x = [d->mtl newTextureWithDescriptor:td];
    if (!x) rt_fatal("[d3d9] out of GPU memory for a %ux%u format conversion texture", w, h);
    return x;
}
// level `level` of face `face` as a 2D texture of one level (the stored values, no channel mapping)
static id<MTLTexture> level_view(id<MTLTexture> base, const Tex *t, uint32_t face, uint32_t level) {
    id<MTLTexture> v = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:MTLTextureType2D
                                                    levels:NSMakeRange(level, 1) slices:NSMakeRange(t->cube ? face : 0, 1)];
    if (!v) rt_fatal("[d3d9] Metal did not create a texture view for a format conversion");
    return v;
}
static uint32_t mtl_dim(id<MTLTexture> t, int height, uint32_t level) { return mip((uint32_t)(height ? t.height : t.width), level); }

// copy pixels into a subresource, in command order
static void tex_upload(Device *d, Tex *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                       uint32_t w, uint32_t h, uint32_t dd, const uint8_t *src, uint32_t pitch, uint32_t slice) {
    const Fmt *f = t->f;
    uint32_t rowb = fmt_pitch(f, w), rows = fmt_rows(f, h);
    NSUInteger sl = t->cube ? face : 0;
    if (d->pend.active) begin_pass_flush_clear(d);   // a pending clear of this texture comes first
    if (tex_alt(t)) {
        // the pixels in the Direct3D format into a temporary texture, then a scaled copy into the storage
        if (dd != 1 || t->ms > 1) rt_fatal("[d3d9] upload into converted storage: %u slices, %u samples", dd, t->ms);
        const Fmt *bf = fmt_find(t->fmt);
        uint32_t brow = fmt_pitch(bf, w), brows = fmt_rows(bf, h);
        id<MTLTexture> tmp = alt_temp(d, t, w, h);
        __unsafe_unretained id<MTLBuffer> buf; uint32_t off;
        uint8_t *dst = ring_alloc(d, brow * brows, &buf, &off);
        for (uint32_t r = 0; r < brows; r++) memcpy(dst + r * brow, src + (size_t)r * pitch, brow);
        [get_blit(d) copyFromBuffer:buf sourceOffset:off sourceBytesPerRow:brow sourceBytesPerImage:(NSUInteger)brow * brows
                         sourceSize:MTLSizeMake(w, h, 1) toTexture:tmp destinationSlice:0 destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        id<MTLRenderCommandEncoder> e = util_pass(d, t->mtl, level, sl, 0, MTLClearColorMake(0, 0, 0, 0));
        int32_t sr[4] = {0, 0, (int32_t)w, (int32_t)h};
        int32_t dr[4] = {(int32_t)(x * t->sc), (int32_t)(y * t->sc), (int32_t)((x + w) * t->sc), (int32_t)((y + h) * t->sc)};
        util_quad(d, e, 'K', 0, t->f->mtl, tmp, d->samp_pt, sr, w, h, dr, mtl_dim(t->mtl, 0, level), mtl_dim(t->mtl, 1, level), nil, 0, NULL);
        [e endEncoding];
        t->last_use = t->last_write = cur_serial(d);
        return;
    }
    if (t->mtl.storageMode != MTLStorageModePrivate && t->last_use <= done_serial(d)) {
        [t->mtl replaceRegion:MTLRegionMake3D(x, y, z, w, h, dd) mipmapLevel:level slice:sl withBytes:src
                  bytesPerRow:pitch bytesPerImage:t->volume ? slice : 0];
        return;
    }
    uint64_t img = (uint64_t)rowb * rows, total = img * dd;
    if (total > 0xFFFFFFFFull) rt_fatal("[d3d9] texture upload of %llu bytes", (unsigned long long)total);
    __unsafe_unretained id<MTLBuffer> buf; uint32_t off;
    uint8_t *dst = ring_alloc(d, (uint32_t)total, &buf, &off);
    for (uint32_t k = 0; k < dd; k++)
        for (uint32_t r = 0; r < rows; r++) memcpy(dst + k * img + r * rowb, src + (size_t)k * slice + (size_t)r * pitch, rowb);
    [get_blit(d) copyFromBuffer:buf sourceOffset:off sourceBytesPerRow:rowb sourceBytesPerImage:(NSUInteger)img
                     sourceSize:MTLSizeMake(w, h, dd) toTexture:t->mtl destinationSlice:sl destinationLevel:level
              destinationOrigin:MTLOriginMake(x, y, z)];
    t->last_use = t->last_write = cur_serial(d);
}
static void tex_gen_mips(Device *d, Tex *t) {
    if (!t->mtl || t->mtl_levels < 2) { t->mips_dirty = 0; return; }
    [get_blit(d) generateMipmapsForTexture:t->mtl];
    t->last_use = t->last_write = cur_serial(d);
    t->mips_dirty = 0;
}
// multisampled surfaces: resolve into t->resolve
static void tex_resolve(Device *d, Tex *t) {
    if (t->ms <= 1 || !t->resolve) return;
    if (d->pend.active) begin_pass_flush_clear(d);
    ensure_cmd(d);
    end_encoders(d);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t->mtl;
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    p.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    p.colorAttachments[0].resolveTexture = t->resolve;
    id<MTLRenderCommandEncoder> e = [d->cmd renderCommandEncoderWithDescriptor:p];
    [e endEncoding];
    t->last_use = t->last_write = d->serial;
}
// read pixels of a subresource (waits for the GPU when it still writes the texture)
static void tex_read(Device *d, Tex *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                     uint32_t w, uint32_t h, uint32_t dd, uint8_t *dst, uint32_t pitch, uint32_t slice) {
    const Fmt *f = t->f;
    NSUInteger sl = t->cube ? face : 0;
    if (d->pend.active) begin_pass_flush_clear(d);
    if (t->ms > 1) tex_resolve(d, t);
    id<MTLTexture> src = t->ms > 1 ? t->resolve : t->mtl;
    if (tex_alt(t)) {
        // a scaled copy of the storage into a temporary texture in the Direct3D format, which is read back
        if (dd != 1) rt_fatal("[d3d9] read of converted storage: %u slices", dd);
        id<MTLTexture> tmp = alt_temp(d, t, w, h);
        id<MTLRenderCommandEncoder> e = util_pass(d, tmp, 0, 0, 0, MTLClearColorMake(0, 0, 0, 0));
        int32_t sr[4] = {(int32_t)x, (int32_t)y, (int32_t)(x + w), (int32_t)(y + h)}, dr[4] = {0, 0, (int32_t)w, (int32_t)h};
        util_quad(d, e, 'K', 0, tmp.pixelFormat, level_view(src, t, face, level), d->samp_lin, sr, mip(t->w, level), mip(t->h, level),
                  dr, w, h, nil, 0, NULL);
        [e endEncoding];
        src = tmp; f = fmt_find(t->fmt); sl = 0; level = 0; x = y = z = 0;
    }
    MTLRegion r = MTLRegionMake3D(x, y, z, w, h, dd);
    if (src.storageMode != MTLStorageModePrivate) {
        if (t->last_write > done_serial(d)) wait_serial(d, t->last_write);
        [src getBytes:dst bytesPerRow:pitch bytesPerImage:t->volume ? slice : 0 fromRegion:r mipmapLevel:level slice:sl];
        return;
    }
    uint32_t rowb = fmt_pitch(f, w), rows = fmt_rows(f, h);
    uint64_t img = (uint64_t)rowb * rows, total = img * dd;
    id<MTLBuffer> b = [d->mtl newBufferWithLength:(NSUInteger)(total ? total : 1) options:MTLResourceStorageModeShared];
    if (!b) rt_fatal("[d3d9] out of memory reading back a texture (%llu bytes)", (unsigned long long)total);
    [get_blit(d) copyFromTexture:src sourceSlice:sl sourceLevel:level sourceOrigin:MTLOriginMake(x, y, z)
                      sourceSize:MTLSizeMake(w, h, dd) toBuffer:b destinationOffset:0 destinationBytesPerRow:rowb
         destinationBytesPerImage:(NSUInteger)img];
    t->last_use = cur_serial(d);
    wait_serial(d, d->serial);
    const uint8_t *s = b.contents;
    for (uint32_t k = 0; k < dd; k++)
        for (uint32_t row = 0; row < rows; row++) memcpy(dst + (size_t)k * slice + (size_t)row * pitch, s + k * img + row * rowb, rowb);
}

// a surface or volume object for a subresource of a texture (created on first use; freed with the texture)
static Surf *tex_sub(Tex *t, uint32_t face, uint32_t level) {
    uint32_t i = face * t->levels + level;
    if (!t->subs[i]) {
        Surf *s = calloc(1, sizeof *s);
        if (!s) return NULL;
        s->o.container = &t->o; s->o.dev = t->o.dev; s->o.rtype = t->volume ? RTYPE_VOLUME : RTYPE_SURFACE;
        s->tex = t; s->face = face; s->level = level;
        s->w = mip(t->w, level); s->h = mip(t->h, level); s->d = mip(t->d, level); s->volume = t->volume;
        com_wrap(&s->o.com, t->volume ? &c_volume : &c_surface);
        s->o.com.refs = 0;   // references count on the texture
        t->subs[i] = s;
    }
    return t->subs[i];
}
static void surf_destroy(Obj *o) {
    Surf *s = (Surf *)o;
    if (s->tex) { tex_free_storage(s->tex); free(s->tex); }
    free(s);
}
// a standalone surface (render target, depth stencil, offscreen plain, back buffer)
static Surf *surf_new(Device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t ms,
                      uint32_t msq, int lockable, int bb, uint32_t *err) {
    Tex *t = tex_new(d, w, h, 1, 1, usage, fmt, pool, 0, 0, ms, msq, 1, lockable, bb, err);
    if (!t) return NULL;
    t->lockable = lockable;
    t->o.dev = d; t->o.rtype = RTYPE_SURFACE;
    Surf *s = calloc(1, sizeof *s);
    if (!s) { tex_free_storage(t); free(t); *err = E_OUTOFMEMORY_; return NULL; }
    s->tex = t; s->w = w; s->h = h; s->d = 1;
    obj_wrap(&s->o, &c_surface, d, surf_destroy);
    s->o.rtype = RTYPE_SURFACE;
    return s;
}

// ---- locks ----
// rect: l, t, r, b (2D) or box: l, t, r, b, front, back (volumes); NULL: everything
static uint32_t sub_lock(Device *d, Tex *t, uint32_t face, uint32_t level, const int32_t *rect, const int32_t *box,
                         uint32_t flags, uint32_t out, int is_box) {
    if (face >= t->faces || level >= t->levels) return D3DERR_INVALIDCALL;
    uint32_t idx = face * t->levels + level;
    LockSt *L = &tex_locks(t)[idx];
    if (L->locked) return D3DERR_INVALIDCALL;
    if (t->f->kind) { LOG_ONCE("[d3d9] depth-stencil surfaces cannot be locked"); return D3DERR_INVALIDCALL; }
    if (t->ms > 1) return D3DERR_INVALIDCALL;
    if (t->pool == POOL_DEFAULT && !(t->usage & USAGE_DYNAMIC) && !t->lockable) return D3DERR_INVALIDCALL;
    const Fmt *f = t->f;
    uint32_t lw = mip(t->w, level), lh = mip(t->h, level), ld = mip(t->d, level);
    int64_t x0 = 0, y0 = 0, z0 = 0, x1 = lw, y1 = lh, z1 = ld;
    const int32_t *r = is_box ? box : rect;
    if (r) {
        x0 = r[0]; y0 = r[1]; x1 = r[2]; y1 = r[3];
        if (is_box) { z0 = r[4]; z1 = r[5]; }
        if (x0 < 0 || y0 < 0 || z0 < 0 || x1 > lw || y1 > lh || z1 > ld || x0 >= x1 || y0 >= y1 || z0 >= z1) return D3DERR_INVALIDCALL;
        if (f->block && ((x0 | y0) & 3)) return D3DERR_INVALIDCALL;
    }
    uint32_t rw = (uint32_t)(x1 - x0), rh = (uint32_t)(y1 - y0), rd = (uint32_t)(z1 - z0);
    uint32_t ptr, pitch, slice, mem = 0;
    if (t->sys) {
        pitch = fmt_pitch(f, lw); slice = pitch * fmt_rows(f, lh);
        uint32_t xb = f->block ? (uint32_t)x0 / 4 * f->bpp : (uint32_t)x0 * f->bpp;
        uint32_t yb = f->block ? (uint32_t)y0 / 4 : (uint32_t)y0;
        ptr = t->sys[idx] + (uint32_t)z0 * slice + yb * pitch + xb;
    } else {
        pitch = fmt_pitch(f, rw); slice = pitch * fmt_rows(f, rh);
        uint64_t total = (uint64_t)slice * rd;
        if (total > 0x7FFFFFFFu) return E_OUTOFMEMORY_;
        mem = gmalloc((uint32_t)total);
        if (!mem) return E_OUTOFMEMORY_;
        if (!(flags & LOCK_DISCARD) && t->init[idx])
            tex_read(d, t, face, level, (uint32_t)x0, (uint32_t)y0, (uint32_t)z0, rw, rh, rd, G2H(mem), pitch, slice);
        else memset(G2H(mem), 0, (size_t)total);
        ptr = mem;
    }
    if (is_box) { wr32(out, pitch); wr32(out + 4, slice); wr32(out + 8, ptr); }
    else { wr32(out, pitch); wr32(out + 4, ptr); }
    *L = (LockSt){1, flags, mem, pitch, slice, (uint32_t)x0, (uint32_t)y0, (uint32_t)z0, rw, rh, rd};
    t->nlocked++;
    return D3D_OK;
}
static uint32_t sub_unlock(Device *d, Tex *t, uint32_t face, uint32_t level) {
    if (face >= t->faces || level >= t->levels) return D3DERR_INVALIDCALL;
    uint32_t idx = face * t->levels + level;
    LockSt *L = &tex_locks(t)[idx];
    if (!L->locked) return D3DERR_INVALIDCALL;
    if (!t->sys) {
        if (!(L->flags & LOCK_READONLY)) {
            tex_upload(d, t, face, level, L->x, L->y, L->z, L->w, L->h, L->d, G2H(L->mem), L->pitch, L->slice);
            t->init[idx] = 1;
            if ((t->usage & USAGE_AUTOGENMIPMAP) && level == 0) t->mips_dirty = 1;
        }
        gfree(L->mem);
    }
    memset(L, 0, sizeof *L);
    t->nlocked--;
    return D3D_OK;
}

// ---------------- vertex and index buffers ----------------
static void vb_destroy(Obj *o) {
    VB *b = (VB *)o;
    store_free(o->dev, &b->st, b->last_use);
    free(b);
}

// ---------------- vertex declarations ----------------
enum { DT_FLOAT1, DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_D3DCOLOR, DT_UBYTE4, DT_SHORT2, DT_SHORT4, DT_UBYTE4N, DT_SHORT2N,
       DT_SHORT4N, DT_USHORT2N, DT_USHORT4N, DT_UDEC3, DT_DEC3N, DT_FLOAT16_2, DT_FLOAT16_4, DT_UNUSED };
enum { DU_POSITION, DU_BLENDWEIGHT, DU_BLENDINDICES, DU_NORMAL, DU_PSIZE, DU_TEXCOORD, DU_TANGENT, DU_BINORMAL,
       DU_TESSFACTOR, DU_POSITIONT, DU_COLOR, DU_FOG, DU_DEPTH, DU_SAMPLE };
static const uint8_t dt_size[17] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
static const uint8_t dt_comps[17] = {1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4};
static const MTLVertexFormat dt_mtl[17] = {
    MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4,
    MTLVertexFormatUChar4Normalized_BGRA, MTLVertexFormatUChar4, MTLVertexFormatShort2, MTLVertexFormatShort4,
    MTLVertexFormatUChar4Normalized, MTLVertexFormatShort2Normalized, MTLVertexFormatShort4Normalized,
    MTLVertexFormatUShort2Normalized, MTLVertexFormatUShort4Normalized, MTLVertexFormatInvalid,
    MTLVertexFormatInt1010102Normalized, MTLVertexFormatHalf2, MTLVertexFormatHalf4};
// the MSL type class of an element: 0 float, 1 int, 2 uint, 3 float with w = 1 (DEC3N)
static int dt_class(int t) { return t == DT_UBYTE4 ? 2 : (t == DT_SHORT2 || t == DT_SHORT4) ? 1 : t == DT_DEC3N ? 3 : 0; }
static void decl_destroy(Obj *o) { free(o); }

// D3DFVF -> declaration elements
static int fvf_to_decl(uint32_t fvf, Decl *dc) {
    uint32_t n = 0, off = 0;
#define EL(ty, us, ix) do { dc->el[n].stream = 0; dc->el[n].offset = (uint16_t)off; dc->el[n].type = (ty); \
    dc->el[n].method = 0; dc->el[n].usage = (us); dc->el[n].index = (ix); off += dt_size[ty]; n++; } while (0)
    uint32_t pos = fvf & 0x400E;
    switch (pos) {
    case 0: break;
    case 0x2: EL(DT_FLOAT3, DU_POSITION, 0); break;
    case 0x4: EL(DT_FLOAT4, DU_POSITIONT, 0); break;
    case 0x4002: EL(DT_FLOAT4, DU_POSITION, 0); break;
    case 0x6: case 0x8: case 0xA: case 0xC: case 0xE: {
        EL(DT_FLOAT3, DU_POSITION, 0);
        int betas = (int)(pos - 4) / 2, idx = (fvf & (0x1000 | 0x8000)) ? 1 : 0, wts = betas - idx;
        if (wts > 4) return 0;
        if (wts > 0) EL(DT_FLOAT1 + wts - 1, DU_BLENDWEIGHT, 0);
        if (idx) EL((fvf & 0x8000) ? DT_D3DCOLOR : DT_UBYTE4, DU_BLENDINDICES, 0);
        break;
    }
    default: return 0;
    }
    if (fvf & 0x10) EL(DT_FLOAT3, DU_NORMAL, 0);
    if (fvf & 0x20) EL(DT_FLOAT1, DU_PSIZE, 0);
    if (fvf & 0x40) EL(DT_D3DCOLOR, DU_COLOR, 0);
    if (fvf & 0x80) EL(DT_D3DCOLOR, DU_COLOR, 1);
    uint32_t ntex = (fvf >> 8) & 0xF;
    if (ntex > 8) return 0;
    for (uint32_t i = 0; i < ntex; i++) {
        static const uint8_t tt[4] = {DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_FLOAT1};
        EL(tt[(fvf >> (16 + 2 * i)) & 3], DU_TEXCOORD, (uint8_t)i);
    }
#undef EL
    dc->n = n; dc->fvf = fvf;
    return 1;
}

// ---------------- shaders ----------------
static void shader_destroy(Obj *o) {
    Shader *s = (Shader *)o;
    free(s->tok);
    free(s->info.msl);
    if (s->var) {
        for (uint32_t i = 0; i < s->nvar; i++) {
            free(s->var[i].info.msl);
            for (int k = 0; k < 4; k++) s->var[i].fn[k] = nil;
        }
        free(s->var);
    }
    s->fns = nil;
    free(s);
}

// ---------------- queries ----------------
enum { QUERY_EVENT = 8, QUERY_OCCLUSION = 9 };
static void query_destroy(Obj *o) {
    Query *q = (Query *)o;
    Device *d = o->dev;
    if (d && d->occl == q) d->occl = NULL;
    if (d) for (uint32_t k = 0; k < q->nslots; k++) d->vis_owner[q->slots[k]] = NULL;
    free(q->slots);
    free(q);
}
// ---------------- state ----------------
enum { RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
       RS_LASTPIXEL = 16, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24,
       RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_SPECULARENABLE = 29,
       RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37, RS_FOGDENSITY = 38,
       RS_RANGEFOGENABLE = 48, RS_STENCILENABLE = 52, RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55,
       RS_STENCILFUNC = 56, RS_STENCILREF = 57, RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59, RS_TEXTUREFACTOR = 60,
       RS_WRAP0 = 128, RS_CLIPPING = 136, RS_LIGHTING = 137, RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140,
       RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143, RS_DIFFUSEMATERIALSOURCE = 145,
       RS_SPECULARMATERIALSOURCE = 146, RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148,
       RS_VERTEXBLEND = 151, RS_CLIPPLANEENABLE = 152, RS_POINTSIZE = 154, RS_POINTSIZE_MIN = 155,
       RS_POINTSPRITEENABLE = 156, RS_POINTSCALEENABLE = 157, RS_POINTSCALE_A = 158, RS_POINTSCALE_B = 159,
       RS_POINTSCALE_C = 160, RS_MULTISAMPLEANTIALIAS = 161, RS_MULTISAMPLEMASK = 162, RS_PATCHEDGESTYLE = 163,
       RS_DEBUGMONITORTOKEN = 165, RS_POINTSIZE_MAX = 166, RS_INDEXEDVERTEXBLENDENABLE = 167,
       RS_COLORWRITEENABLE = 168, RS_TWEENFACTOR = 170, RS_BLENDOP = 171, RS_POSITIONDEGREE = 172,
       RS_NORMALDEGREE = 173, RS_SCISSORTESTENABLE = 174, RS_SLOPESCALEDEPTHBIAS = 175,
       RS_ANTIALIASEDLINEENABLE = 176, RS_MINTESSELLATIONLEVEL = 178, RS_MAXTESSELLATIONLEVEL = 179,
       RS_ADAPTIVETESS_X = 180, RS_ADAPTIVETESS_Y = 181, RS_ADAPTIVETESS_Z = 182, RS_ADAPTIVETESS_W = 183,
       RS_ENABLEADAPTIVETESSELLATION = 184, RS_TWOSIDEDSTENCILMODE = 185, RS_CCW_STENCILFAIL = 186,
       RS_CCW_STENCILZFAIL = 187, RS_CCW_STENCILPASS = 188, RS_CCW_STENCILFUNC = 189, RS_COLORWRITEENABLE1 = 190,
       RS_COLORWRITEENABLE2 = 191, RS_COLORWRITEENABLE3 = 192, RS_BLENDFACTOR = 193, RS_SRGBWRITEENABLE = 194,
       RS_DEPTHBIAS = 195, RS_WRAP8 = 198, RS_SEPARATEALPHABLENDENABLE = 206, RS_SRCBLENDALPHA = 207,
       RS_DESTBLENDALPHA = 208, RS_BLENDOPALPHA = 209 };
enum { TSS_COLOROP = 1, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2, TSS_BUMPENVMAT00,
       TSS_BUMPENVMAT01, TSS_BUMPENVMAT10, TSS_BUMPENVMAT11, TSS_TEXCOORDINDEX, TSS_BUMPENVLSCALE = 22,
       TSS_BUMPENVLOFFSET = 23, TSS_TEXTURETRANSFORMFLAGS = 24, TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27,
       TSS_RESULTARG = 28, TSS_CONSTANT = 32 };
enum { SAMP_ADDRESSU = 1, SAMP_ADDRESSV, SAMP_ADDRESSW, SAMP_BORDERCOLOR, SAMP_MAGFILTER, SAMP_MINFILTER,
       SAMP_MIPFILTER, SAMP_MIPMAPLODBIAS, SAMP_MAXMIPLEVEL, SAMP_MAXANISOTROPY, SAMP_SRGBTEXTURE, SAMP_ELEMENTINDEX,
       SAMP_DMAPOFFSET };
// D3DVERTEXTEXTURESAMPLER0-3 (257-260) are samplers 16-19; D3DDMAPSAMPLER (256) is not supported
static int sampler_index(uint32_t s) { return s < 16 ? (int)s : (s >= 257 && s <= 260) ? (int)(s - 257 + 16) : -1; }

static void set_identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void state_defaults(State *s, int autodepth, uint32_t w, uint32_t h) {
    free(s->lights);
    memset(s, 0, sizeof *s);
    static const uint32_t rs[][2] = {
        {RS_FILLMODE, 3}, {RS_SHADEMODE, 2}, {RS_ZWRITEENABLE, 1}, {RS_LASTPIXEL, 1}, {RS_SRCBLEND, 2},
        {RS_DESTBLEND, 1}, {RS_CULLMODE, 3}, {RS_ZFUNC, 4}, {RS_ALPHAFUNC, 8}, {RS_FOGEND, 0x3F800000},
        {RS_FOGDENSITY, 0x3F800000}, {RS_STENCILFAIL, 1}, {RS_STENCILZFAIL, 1}, {RS_STENCILPASS, 1},
        {RS_STENCILFUNC, 8}, {RS_STENCILMASK, 0xFFFFFFFF}, {RS_STENCILWRITEMASK, 0xFFFFFFFF},
        {RS_TEXTUREFACTOR, 0xFFFFFFFF}, {RS_CLIPPING, 1}, {RS_LIGHTING, 1}, {RS_COLORVERTEX, 1}, {RS_LOCALVIEWER, 1},
        {RS_DIFFUSEMATERIALSOURCE, 1}, {RS_SPECULARMATERIALSOURCE, 2}, {RS_POINTSIZE, 0x3F800000},
        {RS_POINTSIZE_MIN, 0x3F800000}, {RS_POINTSCALE_A, 0x3F800000}, {RS_MULTISAMPLEANTIALIAS, 1},
        {RS_MULTISAMPLEMASK, 0xFFFFFFFF}, {RS_DEBUGMONITORTOKEN, 0xBAADCAFE}, {RS_POINTSIZE_MAX, 0x43800000},
        {RS_COLORWRITEENABLE, 0xF}, {RS_BLENDOP, 1}, {RS_POSITIONDEGREE, 3}, {RS_NORMALDEGREE, 1},
        {RS_MINTESSELLATIONLEVEL, 0x3F800000}, {RS_MAXTESSELLATIONLEVEL, 0x3F800000}, {RS_ADAPTIVETESS_Z, 0x3F800000},
        {RS_CCW_STENCILFAIL, 1}, {RS_CCW_STENCILZFAIL, 1}, {RS_CCW_STENCILPASS, 1}, {RS_CCW_STENCILFUNC, 8},
        {RS_COLORWRITEENABLE1, 0xF}, {RS_COLORWRITEENABLE2, 0xF}, {RS_COLORWRITEENABLE3, 0xF},
        {RS_BLENDFACTOR, 0xFFFFFFFF}, {RS_SRCBLENDALPHA, 2}, {RS_DESTBLENDALPHA, 1}, {RS_BLENDOPALPHA, 1},
    };
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++) s->rs[rs[i][0]] = rs[i][1];
    s->rs[RS_ZENABLE] = autodepth ? 1 : 0;
    for (int i = 0; i < 8; i++) {
        uint32_t *t = s->tss[i];
        t[TSS_COLOROP] = i ? 1 : 4; t[TSS_ALPHAOP] = i ? 1 : 2;
        t[TSS_COLORARG1] = 2; t[TSS_COLORARG2] = 1; t[TSS_ALPHAARG1] = 2; t[TSS_ALPHAARG2] = 1;
        t[TSS_TEXCOORDINDEX] = (uint32_t)i; t[TSS_COLORARG0] = 1; t[TSS_ALPHAARG0] = 1; t[TSS_RESULTARG] = 1;
    }
    for (int i = 0; i < 20; i++) {
        uint32_t *t = s->samp[i];
        t[SAMP_ADDRESSU] = t[SAMP_ADDRESSV] = t[SAMP_ADDRESSW] = 1;
        t[SAMP_MAGFILTER] = t[SAMP_MINFILTER] = 1; t[SAMP_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < 512; i++) set_identity(s->xf[i]);
    s->vp[2] = w; s->vp[3] = h; s->vp[5] = ubits(1.0f);
    s->scissor[2] = (int32_t)w; s->scissor[3] = (int32_t)h;
    for (int i = 0; i < 16; i++) s->stream[i].freq = 1;
}
// drop the private references a state holds
static void state_unbind(State *s) {
    for (int i = 0; i < 20; i++) REBIND(s->tex[i], NULL);
    REBIND(s->vs, NULL); REBIND(s->ps, NULL); REBIND(s->decl, NULL); REBIND(s->ib, NULL);
    for (int i = 0; i < 16; i++) REBIND(s->stream[i].vb, NULL);
}
static void mark_dirty(Device *d) { d->vsc_dirty = d->psc_dirty = d->ffc_dirty = 1; }

static Light *light_find(State *s, uint32_t idx) {
    for (uint32_t i = 0; i < s->nlights; i++) if (s->lights[i].idx == idx) return &s->lights[i];
    return NULL;
}
#define MAX_LIGHTS 4096u
static Light *light_get(State *s, uint32_t idx) {
    Light *l = light_find(s, idx);
    if (l) return l;
    if (s->nlights >= MAX_LIGHTS) return NULL;
    if (s->nlights == s->caplights) {
        uint32_t cap = s->caplights ? s->caplights * 2 : 16;
        Light *q = realloc(s->lights, cap * sizeof *q);
        if (!q) return NULL;
        s->lights = q; s->caplights = cap;
    }
    l = &s->lights[s->nlights++];
    memset(l, 0, sizeof *l);
    l->idx = idx;
    // the default light: white directional light along +z
    l->data[0] = 3; l->data[1] = l->data[2] = l->data[3] = ubits(1.0f); l->data[18] = ubits(1.0f);
    return l;
}
static void light_enable(State *s, uint32_t idx, int on) {
    Light *l = light_get(s, idx);
    if (!l) return;
    uint32_t k = 0;
    for (; k < s->nactive; k++) if (s->active[k] == idx) break;
    if (on) {
        if (k == s->nactive) {
            if (s->nactive == 8) { LOG_ONCE("[d3d9] more than 8 lights enabled: the extra lights are ignored"); return; }
            s->active[s->nactive++] = idx;
        }
        l->enabled = 1;
    } else {
        if (k < s->nactive) { memmove(&s->active[k], &s->active[k + 1], (s->nactive - k - 1) * sizeof s->active[0]); s->nactive--; }
        l->enabled = 0;
    }
}

// ---------------- MSL generation and compilation ----------------
typedef struct { char *s; size_t n, cap; int oom; } SBuf;
static void sput(SBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sput(SBuf *b, const char *fmt, ...) {
    if (b->oom) return;
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) { va_end(ap2); b->oom = 1; return; }
    size_t need = b->n + (size_t)n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 8192;
        while (cap < need) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) { va_end(ap2); b->oom = 1; return; }
        b->s = p; b->cap = cap;
    }
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap2); va_end(ap2);
    b->n += (size_t)n;
}
static char *sdone(SBuf *b) { if (b->oom) { free(b->s); return NULL; } return b->s; }

static const char *tex_msl(int tt) { return tt == 3 ? "texturecube" : tt == 4 ? "texture3d" : "texture2d"; }
static void tex_params(SBuf *b, uint32_t samplers, const uint8_t *stype, int call) {
    for (int i = 0; i < 16; i++) if (samplers & (1u << i)) {
        if (call) sput(b, ", tex%d, smp%d", i, i);
        else sput(b, ", %s<float> tex%d [[texture(%d)]], sampler smp%d [[sampler(%d)]]", tex_msl(stype[i] ? stype[i] : 2), i, i, i, i);
    }
}
// vertex entry point: VSIn from the input classes, v[] filled from it, the body, the outputs
static char *vs_entry(const ShaderInfo *si, int ff, const uint8_t *cls, uint16_t present, int points) {
    SBuf b = {0};
    sput(&b, "%s%s", d3d9_msl_prelude, si->msl);
    if (present) {
        sput(&b, "struct VSIn {");
        static const char *ty[4] = {"float4", "int4", "uint4", "float4"};
        for (int r = 0; r < 16; r++) if (present >> r & 1) sput(&b, " %s a%d [[attribute(%d)]];", ty[cls[r]], r, r);
        sput(&b, " };\n");
    }
    sput(&b, "vertex %s vs_main(%sconstant %s &k [[buffer(0)]]", points ? "VSOutP" : "VSOut", present ? "VSIn in [[stage_in]], " : "",
         ff ? "FFVSConsts" : "VSConsts");
    tex_params(&b, si->samplers, si->sampler_type, 0);
    sput(&b, ") {\n    float4 v[16];\n    for (int i = 0; i < 16; i++) v[i] = float4(0.0, 0.0, 0.0, 1.0);\n");
    for (int r = 0; r < 16; r++) if (present >> r & 1) {
        if (cls[r] == 0) sput(&b, "    v[%d] = in.a%d;\n", r, r);
        else if (cls[r] == 3) sput(&b, "    v[%d] = float4(in.a%d.xyz, 1.0);\n", r, r);
        else sput(&b, "    v[%d] = float4(in.a%d);\n", r, r);
    }
    sput(&b, "    VSRegs o; vs_regs_init(o);\n");
    if (ff) sput(&b, "    vs_ff(v, o, k);\n");
    else {
        sput(&b, "    vs_%08x(v, o, k", si->hash);
        tex_params(&b, si->samplers, si->sampler_type, 1);
        sput(&b, ");\n");
    }
    sput(&b, "    VSOut r = vs_out_from(o, k.pos_fixup);\n");
    if (points) sput(&b, "    return vs_outp_from(r, max(%s, 1.0));\n", ff || si->writes_psize ? "o.psize4.x" : "k.pos_fixup.w");
    else sput(&b, "    return r;\n");
    sput(&b, "}\n");
    return sdone(&b);
}
// fragment entry point: flat colors, point sprite coordinates, fog and alpha test around the body
static char *ps_entry(const ShaderInfo *si, int ff, int flat, int sprite) {
    SBuf b = {0};
    sput(&b, "%s%s", d3d9_msl_prelude, si->msl);
    sput(&b, "struct PSOutC {");
    for (int i = 0; i < 4; i++) if (si->color_mask & (1 << i)) sput(&b, " float4 c%d [[color(%d)]];", i, i);
    if (si->writes_depth) sput(&b, " float depth [[depth(any)]];");
    if (!si->color_mask && !si->writes_depth) sput(&b, " float4 c0 [[color(0)]];");
    sput(&b, " };\n");
    sput(&b, "fragment PSOutC ps_main(%s in [[stage_in]], bool face [[front_facing]], %sconstant PSConsts &k [[buffer(0)]]",
         flat ? "PSInFlat" : "PSIn", sprite ? "float2 pc [[point_coord]], " : "");
    tex_params(&b, si->samplers, si->sampler_type, 0);
    sput(&b, ") {\n    PSRegs r; ps_regs_from(r, in, face);\n");
    if (sprite) for (int i = 0; i < 8; i++) sput(&b, "    r.TEXCOORD%d = float4(pc, 0.0, 1.0);\n", i);
    sput(&b, "    PSORegs o = {};\n");
    if (ff) sput(&b, "    ps_ff(r, o, k");
    else sput(&b, "    ps_%08x(r, o, k", si->hash);
    tex_params(&b, si->samplers, si->sampler_type, 1);
    sput(&b, ");\n");
    if (ff || ((si->version >> 8) & 0xFF) < 3) sput(&b, "    o.color0 = d3d_fog(o.color0, in, k);\n");
    sput(&b, "    if (!d3d_alpha_pass(o.color0.w, k)) discard_fragment();\n    PSOutC res;\n");
    for (int i = 0; i < 4; i++) if (si->color_mask & (1 << i)) sput(&b, "    res.c%d = o.color%d;\n", i, i);
    if (si->writes_depth) sput(&b, "    res.depth = o.depth4.x;\n");
    if (!si->color_mask && !si->writes_depth) sput(&b, "    res.c0 = float4(0.0);\n");
    sput(&b, "    return res;\n}\n");
    return sdone(&b);
}
static D3D9Fn *compile_fn(Device *d, char *src, const char *entry, const char *what, uint32_t hash, const ShaderInfo *si) {
    D3D9Fn *e = [D3D9Fn new];
    e->fid = ++d->next_fid;
    if (si) { e->samplers = si->samplers; memcpy(e->stype, si->sampler_type, 16); }
    if (!src) { rt_log("[d3d9] out of memory generating %s %08x", what, hash); return e; }
    NSString *s = [[NSString alloc] initWithBytesNoCopy:src length:strlen(src) encoding:NSUTF8StringEncoding freeWhenDone:YES];
    if (!s) { free(src); rt_log("[d3d9] %s %08x: generated source is not UTF-8", what, hash); return e; }
    MTLCompileOptions *o = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeSafe; else o.fastMathEnabled = NO;
    NSError *err = nil;
    id<MTLLibrary> lib = [d->mtl newLibraryWithSource:s options:o error:&err];
    const char *dump = getenv("D3D9_MSL_DIR");
    if (dump && *dump) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s_%08x_%llu.metal", dump, what, hash, (unsigned long long)e->fid);
        NSError *werr = nil;
        if (![s writeToFile:@(path) atomically:NO encoding:NSUTF8StringEncoding error:&werr])
            LOG_ONCE("[d3d9] D3D9_MSL_DIR: cannot write %s: %s", path, werr.localizedDescription.UTF8String);
    }
    if (!lib) { rt_log("[d3d9] %s %08x did not compile: %s", what, hash, err.localizedDescription.UTF8String); return e; }
    if (err) rt_log("[d3d9] %s %08x compiled with warnings: %s", what, hash, err.localizedDescription.UTF8String);
    e->fn = [lib newFunctionWithName:@(entry)];
    if (!e->fn) rt_log("[d3d9] %s %08x has no entry point %s", what, hash, entry);
    return e;
}
#define FN_CACHE_CAP 4096u
static void cache_put(NSMutableDictionary *c, id key, id val, const char *what) {
    if (c.count >= FN_CACHE_CAP) { rt_log("[d3d9] %s cache full (%u entries): cleared", what, FN_CACHE_CAP); [c removeAllObjects]; }
    c[key] = val;
}
typedef struct { uint8_t cls[16]; uint16_t present; uint8_t points; } VSFnKey;
static D3D9Fn *vs_fn(Device *d, Shader *vs, const uint8_t *cls, uint16_t present, int points) {
    VSFnKey k; memset(&k, 0, sizeof k);
    memcpy(k.cls, cls, 16); k.present = present; k.points = (uint8_t)points;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    D3D9Fn *f = vs->fns[key];
    if (f) return f;
    f = compile_fn(d, vs_entry(&vs->info, 0, cls, present, points), "vs_main", "vertex shader", vs->info.hash, &vs->info);
    if (vs->fns.count >= 256) [vs->fns removeAllObjects];
    vs->fns[key] = f;
    return f;
}
typedef struct { char tag; FFVSKey k; uint8_t cls[16]; uint16_t present; uint8_t points; } FFVSFnKey;
static D3D9Fn *ff_vs_fn(Device *d, const FFVSKey *fk, const uint8_t *cls, uint16_t present, int points) {
    FFVSFnKey k; memset(&k, 0, sizeof k);
    k.tag = 'V'; k.k = *fk; memcpy(k.cls, cls, 16); k.present = present; k.points = (uint8_t)points;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    D3D9Fn *f = d->ff_fns[key];
    if (f) return f;
    ShaderInfo si;
    if (!d3d9_ffvs(fk, &si)) {
        rt_log("[d3d9] fixed-function vertex stage: %s", si.error);
        f = [D3D9Fn new]; f->fid = ++d->next_fid;
    } else {
        f = compile_fn(d, vs_entry(&si, 1, cls, present, points), "vs_main", "fixed-function vertex stage", si.hash, &si);
        free(si.msl);
    }
    cache_put(d->ff_fns, key, f, "fixed-function shader");
    return f;
}
typedef struct { char tag; FFPSKey k; uint8_t flat, sprite; } FFPSFnKey;
static D3D9Fn *ff_ps_fn(Device *d, const FFPSKey *pk, int flat, int sprite) {
    FFPSFnKey k; memset(&k, 0, sizeof k);
    k.tag = 'P'; k.k = *pk; k.flat = (uint8_t)flat; k.sprite = (uint8_t)sprite;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    D3D9Fn *f = d->ff_fns[key];
    if (f) return f;
    ShaderInfo si;
    if (!d3d9_ffps(pk, &si)) {
        rt_log("[d3d9] fixed-function pixel stage: %s", si.error);
        f = [D3D9Fn new]; f->fid = ++d->next_fid;
    } else {
        f = compile_fn(d, ps_entry(&si, 1, flat, sprite), "ps_main", "fixed-function pixel stage", si.hash, &si);
        free(si.msl);
    }
    cache_put(d->ff_fns, key, f, "fixed-function shader");
    return f;
}
static int tex_type_of(Obj *o) {
    if (!o) return 0;
    Tex *t = (Tex *)o;
    return t->cube ? 3 : t->volume ? 4 : 2;
}
static D3D9Fn *ps_fn(Device *d, Shader *ps, int flat, int sprite) {
    ShaderVariant v; memset(&v, 0, sizeof v);
    int major = (int)((ps->info.version >> 8) & 0xFF), minor = (int)(ps->info.version & 0xFF);
    if (major < 2) {
        for (int s = 0; s < 16; s++) v.sampler_type[s] = (uint8_t)tex_type_of(d->st.tex[s]);
        if (major == 1 && minor <= 3)
            for (int s = 0; s < 8; s++) {
                uint32_t f = d->st.tss[s][TSS_TEXTURETRANSFORMFLAGS];
                if (f & 0x100) { uint32_t n = f & 0xFF; v.projected[s] = (uint8_t)(n >= 2 && n <= 4 ? n : 4); }
            }
    }
    PVar *pv = NULL;
    for (uint32_t i = 0; i < ps->nvar; i++) if (!memcmp(&ps->var[i].key, &v, sizeof v)) { pv = &ps->var[i]; break; }
    if (!pv) {
        if (ps->nvar == MAX_PVAR) {
            for (uint32_t i = 0; i < ps->nvar; i++) { free(ps->var[i].info.msl); for (int k = 0; k < 4; k++) ps->var[i].fn[k] = nil; }
            for (uint32_t i = 0; i < MAX_PVAR; i++) { memset(&ps->var[i].key, 0, sizeof ps->var[i].key); memset(&ps->var[i].info, 0, sizeof ps->var[i].info); ps->var[i].ok = 0; }
            ps->nvar = 0;
        }
        pv = &ps->var[ps->nvar++];
        pv->key = v;
        pv->ok = d3d9_translate_shader(ps->tok, ps->ntok, &v, &pv->info);
        if (!pv->ok) rt_log("[d3d9] pixel shader %08x: %s", ps->info.hash, pv->info.error);
    }
    if (!pv->ok) return nil;
    int k = (flat << 1) | sprite;
    if (!pv->fn[k]) pv->fn[k] = compile_fn(d, ps_entry(&pv->info, 0, flat, sprite), "ps_main", "pixel shader", pv->info.hash, &pv->info);
    return pv->fn[k];
}

// ---------------- pipeline, depth-stencil and sampler states ----------------
typedef struct {
    struct { uint8_t fmt, buf; uint16_t off; } attr[16];
    struct { uint32_t stride; uint8_t step; uint8_t pad; uint16_t rate; } lay[17];
} VLayout;
typedef struct {
    uint64_t vid, pid;
    VLayout vl;
    struct { uint16_t fmt; uint8_t mask, en, sb, db, op, sba, dba, opa; } c[4];
    uint16_t dfmt, sfmt;
    uint8_t samples;
} PSOKey;
static MTLBlendFactor blend_factor(uint32_t b, int dst_alpha) {
    switch (b) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return dst_alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorOne;
    case 8: return dst_alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorZero;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(uint32_t o) {
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}
static id<MTLRenderPipelineState> pso_get(Device *d, const PSOKey *k, D3D9Fn *vf, D3D9Fn *pf) {
    NSData *key = [NSData dataWithBytes:k length:sizeof *k];
    id p = d->pso_cache[key];
    if (p) return p == (id)[NSNull null] ? nil : p;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vf->fn;
    pd.fragmentFunction = pf->fn;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    int any = 0;
    for (int r = 0; r < 16; r++) if (k->vl.attr[r].fmt) {
        vd.attributes[r].format = (MTLVertexFormat)k->vl.attr[r].fmt;
        vd.attributes[r].offset = k->vl.attr[r].off;
        vd.attributes[r].bufferIndex = k->vl.attr[r].buf;
        any = 1;
    }
    for (int s = 1; s < 17; s++) if (k->vl.lay[s].stride) {
        vd.layouts[s].stride = k->vl.lay[s].stride;
        vd.layouts[s].stepFunction = (MTLVertexStepFunction)k->vl.lay[s].step;
        vd.layouts[s].stepRate = k->vl.lay[s].step == MTLVertexStepFunctionConstant ? 0 : k->vl.lay[s].rate ? k->vl.lay[s].rate : 1;
    }
    if (any) pd.vertexDescriptor = vd;
    for (int i = 0; i < 4; i++) if (k->c[i].fmt) {
        MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
        a.pixelFormat = (MTLPixelFormat)k->c[i].fmt;
        a.writeMask = k->c[i].mask;
        if (k->c[i].en) {
            a.blendingEnabled = YES;
            a.sourceRGBBlendFactor = (MTLBlendFactor)k->c[i].sb; a.destinationRGBBlendFactor = (MTLBlendFactor)k->c[i].db;
            a.rgbBlendOperation = (MTLBlendOperation)k->c[i].op;
            a.sourceAlphaBlendFactor = (MTLBlendFactor)k->c[i].sba; a.destinationAlphaBlendFactor = (MTLBlendFactor)k->c[i].dba;
            a.alphaBlendOperation = (MTLBlendOperation)k->c[i].opa;
        }
    }
    pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt;
    pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt;
    pd.rasterSampleCount = k->samples ? k->samples : 1;
    NSError *err = nil;
    p = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) rt_log("[d3d9] pipeline state not created: %s", err.localizedDescription.UTF8String);
    cache_put(d->pso_cache, key, p ? p : [NSNull null], "pipeline state");
    return p;
}
typedef struct { uint8_t zf, zw, sen, pad; uint8_t f[4], b[4]; uint8_t rmask, wmask; } DSSKey;
static id<MTLDepthStencilState> dss_get(Device *d) {
    State *S = &d->st;
    DSSKey k; memset(&k, 0, sizeof k);
    const Fmt *df = d->ds ? d->ds->tex->f : NULL;
    if (df && S->rs[RS_ZENABLE]) {
        uint32_t zf = S->rs[RS_ZFUNC];
        k.zf = (uint8_t)(zf >= 1 && zf <= 8 ? zf - 1 : MTLCompareFunctionAlways);
        k.zw = S->rs[RS_ZWRITEENABLE] ? 1 : 0;
    } else k.zf = MTLCompareFunctionAlways;
    if (df && df->kind == 2 && S->rs[RS_STENCILENABLE]) {
        k.sen = 1;
#define SOP(v) (uint8_t)((v) >= 1 && (v) <= 8 ? (v) - 1 : 0)
#define SFN(v) (uint8_t)((v) >= 1 && (v) <= 8 ? (v) - 1 : MTLCompareFunctionAlways)
        k.f[0] = SOP(S->rs[RS_STENCILFAIL]); k.f[1] = SOP(S->rs[RS_STENCILZFAIL]); k.f[2] = SOP(S->rs[RS_STENCILPASS]);
        k.f[3] = SFN(S->rs[RS_STENCILFUNC]);
        if (S->rs[RS_TWOSIDEDSTENCILMODE]) {
            k.b[0] = SOP(S->rs[RS_CCW_STENCILFAIL]); k.b[1] = SOP(S->rs[RS_CCW_STENCILZFAIL]); k.b[2] = SOP(S->rs[RS_CCW_STENCILPASS]);
            k.b[3] = SFN(S->rs[RS_CCW_STENCILFUNC]);
        } else memcpy(k.b, k.f, 4);
#undef SOP
#undef SFN
        k.rmask = (uint8_t)S->rs[RS_STENCILMASK]; k.wmask = (uint8_t)S->rs[RS_STENCILWRITEMASK];
    }
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    id<MTLDepthStencilState> s = d->dss_cache[key];
    if (s) return s;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = (MTLCompareFunction)k.zf;
    dd.depthWriteEnabled = k.zw;
    if (k.sen) {
        // front faces are clockwise (D3D9), back faces use the D3DRS_CCW_* operations in two-sided mode
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b = [MTLStencilDescriptor new];
        f.stencilFailureOperation = (MTLStencilOperation)k.f[0]; f.depthFailureOperation = (MTLStencilOperation)k.f[1];
        f.depthStencilPassOperation = (MTLStencilOperation)k.f[2]; f.stencilCompareFunction = (MTLCompareFunction)k.f[3];
        b.stencilFailureOperation = (MTLStencilOperation)k.b[0]; b.depthFailureOperation = (MTLStencilOperation)k.b[1];
        b.depthStencilPassOperation = (MTLStencilOperation)k.b[2]; b.stencilCompareFunction = (MTLCompareFunction)k.b[3];
        f.readMask = b.readMask = k.rmask; f.writeMask = b.writeMask = k.wmask;
        dd.frontFaceStencil = f; dd.backFaceStencil = b;
    }
    s = [d->mtl newDepthStencilStateWithDescriptor:dd];
    if (!s) rt_fatal("[d3d9] Metal did not create a depth-stencil state");
    cache_put(d->dss_cache, key, s, "depth-stencil state");
    return s;
}
static MTLSamplerAddressMode addr_mode(uint32_t a) {
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}
static id<MTLSamplerState> samp_get(Device *d, int s, Tex *t) {
    const uint32_t *v = d->st.samp[s];
    uint32_t mag = v[SAMP_MAGFILTER], min = v[SAMP_MINFILTER], mipf = v[SAMP_MIPFILTER];
    uint32_t aniso = (mag == 3 || min == 3) ? (v[SAMP_MAXANISOTROPY] < 1 ? 1 : v[SAMP_MAXANISOTROPY] > 16 ? 16 : v[SAMP_MAXANISOTROPY]) : 1;
    // enhancement: anisotropic filtering forced on every mipmapped texture with linear or anisotropic minification
    // (point-sampled textures, such as fonts and the interface, keep their own filtering)
    if (rt_enh && rt_enh->aniso >= 1 && rt_enh->aniso <= 4 && mipf != 0 && min >= 2) {
        uint32_t fa = 1u << rt_enh->aniso;
        if (fa > aniso) aniso = fa;
        if (mag == 1) mag = 2;
        min = 2;
    }
    uint32_t maxmip = v[SAMP_MAXMIPLEVEL], lod = t ? t->lod : 0;
    if (lod > maxmip) maxmip = lod;
    if (maxmip > 15) maxmip = 15;
    // Metal border colors: transparent black, opaque black, opaque white (the closest one)
    uint32_t bc = v[SAMP_BORDERCOLOR], border;
    if ((bc >> 24) < 0x80) border = 0;
    else border = (((bc >> 16) & 0xFF) + ((bc >> 8) & 0xFF) + (bc & 0xFF)) >= 3 * 0x80 ? 2 : 1;
    uint64_t key = (uint64_t)(v[SAMP_ADDRESSU] & 7) | (uint64_t)(v[SAMP_ADDRESSV] & 7) << 3 | (uint64_t)(v[SAMP_ADDRESSW] & 7) << 6 |
                   (uint64_t)(mag == 1 || mag == 0 ? 0 : 1) << 9 | (uint64_t)(min == 1 || min == 0 ? 0 : 1) << 10 |
                   (uint64_t)(mipf > 2 ? 2 : mipf) << 11 | (uint64_t)aniso << 13 | (uint64_t)maxmip << 18 | (uint64_t)border << 22;
    NSNumber *kn = @(key);
    id<MTLSamplerState> st = d->samp_cache[kn];
    if (st) return st;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.sAddressMode = addr_mode(v[SAMP_ADDRESSU]); sd.tAddressMode = addr_mode(v[SAMP_ADDRESSV]); sd.rAddressMode = addr_mode(v[SAMP_ADDRESSW]);
    sd.magFilter = (key >> 9 & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = (key >> 10 & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mipf == 0 ? MTLSamplerMipFilterNotMipmapped : mipf == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    sd.maxAnisotropy = aniso;
    sd.lodMinClamp = (float)maxmip;
    sd.borderColor = border == 0 ? MTLSamplerBorderColorTransparentBlack : border == 1 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
    st = [d->mtl newSamplerStateWithDescriptor:sd];
    if (!st) rt_fatal("[d3d9] Metal did not create a sampler state");
    if (d->samp_cache.count >= 1024) [d->samp_cache removeAllObjects];
    d->samp_cache[kn] = st;
    return st;
}

// ---------------- render passes ----------------
// a visibility result slot for an occlusion query segment (one per render pass while the query is active)
static uint32_t vis_slot(Device *d) {
    uint32_t i = d->vis_next;
    if (d->vis_serial[i] > done_serial(d)) wait_serial(d, d->vis_serial[i]);
    d->vis_next = (i + 1) % VIS_SLOTS;
    Query *ow = d->vis_owner[i];
    if (ow) {
        // the slot's command buffer completed: its count moves into the query that owned it
        ow->acc += ((uint64_t *)d->vis.contents)[i];
        for (uint32_t k = 0; k < ow->nslots; k++) if (ow->slots[k] == i) { ow->slots[k] = ow->slots[--ow->nslots]; break; }
        d->vis_owner[i] = NULL;
    }
    ((uint64_t *)d->vis.contents)[i] = 0;
    return i;
}
static void query_add_slot(Device *d, Query *q, uint32_t slot) {
    if (q->nslots == q->capslots) {
        uint32_t cap = q->capslots ? q->capslots * 2 : 8;
        uint32_t *p = realloc(q->slots, cap * sizeof *p);
        if (!p) rt_fatal("[d3d9] out of memory for an occlusion query");
        q->slots = p; q->capslots = cap;
    }
    q->slots[q->nslots++] = slot;
    d->vis_owner[slot] = q;
    d->vis_serial[slot] = d->serial;
}
static id<MTLTexture> rt_view(Tex *t, int srgb) {
    if (srgb && t->f->srgb == MTLPixelFormatInvalid && fmt_find(t->fmt)->srgb != MTLPixelFormatInvalid)
        LOG_ONCE("[d3d9] sRGB write to a render target stored with 16 bits per channel (Reduced Banding): no sRGB conversion");
    if (srgb && t->f->srgb != MTLPixelFormatInvalid) {
        if (!t->rtsrgb) t->rtsrgb = [t->mtl newTextureViewWithPixelFormat:t->f->srgb];
        return t->rtsrgb;
    }
    return t->mtl;
}
// the render encoder for the current render targets (pending full clears become load actions)
static id<MTLRenderCommandEncoder> begin_pass(Device *d) {
    if (d->enc) return d->enc;
    uint32_t slot = UINT32_MAX;
    if (d->occl && d->occl->active) slot = vis_slot(d);
    ensure_cmd(d);
    if (d->blit) { [d->blit endEncoding]; d->blit = nil; }
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    int srgb = d->st.rs[RS_SRGBWRITEENABLE] != 0, pc = d->pend.active ? (int)d->pend.flags : 0;
    uint32_t W = 0, H = 0, ms = 1;
    for (int i = 0; i < 4; i++) {
        Surf *s = d->rt[i];
        if (!s) continue;
        Tex *t = s->tex;
        MTLRenderPassColorAttachmentDescriptor *a = p.colorAttachments[i];
        a.texture = rt_view(t, srgb);
        a.level = s->level; a.slice = t->cube ? s->face : 0;
        if (pc & 1) {
            a.loadAction = MTLLoadActionClear;
            a.clearColor = MTLClearColorMake(d->pend.color[0], d->pend.color[1], d->pend.color[2], t->f->alpha ? d->pend.color[3] : 1.0);
        } else a.loadAction = MTLLoadActionLoad;
        a.storeAction = MTLStoreActionStore;
        if (!W) { W = s->w * t->sc; H = s->h * t->sc; ms = t->ms; }
        t->last_use = t->last_write = d->serial;
        t->init[s->face * t->levels + s->level] = 1;
        if (t->usage & USAGE_AUTOGENMIPMAP) t->mips_dirty = 1;
    }
    if (d->ds) {
        Tex *t = d->ds->tex;
        if (d->rt[0] && d->rt[0]->tex->sc != 1 && (d->ds->w != W || d->ds->h != H))
            rt_fatal("[d3d9] the %ux%u shadow target (stored %ux larger, Sharp Character Shadows) is drawn with a %ux%u depth "
                     "buffer; turn off Sharp Character Shadows", d->rt[0]->w, d->rt[0]->h, d->rt[0]->tex->sc, d->ds->w, d->ds->h);
        p.depthAttachment.texture = t->mtl;
        p.depthAttachment.loadAction = (pc & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        p.depthAttachment.clearDepth = d->pend.z;
        p.depthAttachment.storeAction = MTLStoreActionStore;
        if (t->f->kind == 2) {
            p.stencilAttachment.texture = t->mtl;
            p.stencilAttachment.loadAction = (pc & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
            p.stencilAttachment.clearStencil = d->pend.stencil & 0xFF;
            p.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!W) { W = d->ds->w; H = d->ds->h; ms = t->ms; }
        t->last_use = t->last_write = d->serial;
    }
    p.visibilityResultBuffer = d->vis;
    d->enc = [d->cmd renderCommandEncoderWithDescriptor:p];
    if (!d->enc) rt_fatal("[d3d9] Metal did not create a render encoder (%ux%u, %u samples)", W, H, ms);
    d->pend.active = 0;
    d->pass_w = W; d->pass_h = H; d->pass_ms = ms;
    d->cur_pso = nil; d->cur_dss = nil;
    if (slot != UINT32_MAX) {
        [d->enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:slot * 8];
        query_add_slot(d, d->occl, slot);
    }
    return d->enc;
}
static void begin_pass_flush_clear(Device *d) {
    if (!d->pend.active) return;
    if (d->enc) { d->pend.active = 0; return; }
    begin_pass(d);
    end_pass(d);
}
// render target or depth-stencil changes: finish the pass of the old targets
static void targets_changing(Device *d) {
    if (d->pend.active) begin_pass_flush_clear(d);
    end_pass(d);
}

// ---------------- utility pipelines (clears, copies, presentation) ----------------
static const char util_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct QV { float4 pos [[position]]; float2 uv; };\n"
    // r[0]: destination rectangle in clip space (x0, y0, x1, y1); r[1]: source coordinates; r[2].x: depth
    "vertex QV quad_vs(uint vid [[vertex_id]], constant float4 *r [[buffer(0)]]) {\n"
    "  float2 t = float2(float(vid & 1), float(vid >> 1));\n"
    "  QV o; o.pos = float4(mix(r[0].xy, r[0].zw, t), r[2].x, 1.0); o.uv = mix(r[1].xy, r[1].zw, t); return o; }\n"
    "struct C4 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };\n"
    "fragment C4 clear_fs(QV in [[stage_in]], constant float4 *c [[buffer(0)]]) {\n"
    "  C4 o; o.c0 = c[0]; o.c1 = c[1]; o.c2 = c[2]; o.c3 = c[3]; return o; }\n"
    "fragment float4 copy_fs(QV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, in.uv); }\n"
    "fragment float4 copy_opaque_fs(QV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, in.uv).rgb, 1.0); }\n"
    // presentation: the gamma ramp (D3DGAMMARAMP, 256 entries per channel, interpolated) in full-screen mode.
    // gamma: bit 0 the ramp applies; bit 1 dither (rt_enh.h reduced banding): a triangular noise of +-1 step of the
    // output, from a hash of the pixel position; bit 2 the output has 10 bits per channel (else 8)
    "static float3 dither(float2 p, uint gamma) {\n"
    "  uint2 q = uint2(p);\n"
    "  uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u; h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;\n"
    "  uint h2 = h * 0x297a2d39u; h2 ^= h2 >> 15;\n"
    "  float3 a = float3(float(h & 0xFFFu), float((h >> 12) & 0xFFFu), float((h >> 20) & 0xFFFu)) / 4096.0;\n"
    "  float3 b = float3(float(h2 & 0xFFFu), float((h2 >> 12) & 0xFFFu), float((h2 >> 20) & 0xFFFu)) / 4096.0;\n"
    "  return (a - b) / ((gamma & 4u) != 0u ? 1023.0 : 255.0); }\n"
    "fragment float4 present_fs(QV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
    "                           constant float4 *ramp [[buffer(0)]], constant uint &gamma [[buffer(1)]]) {\n"
    "  float3 c = saturate(t.sample(s, in.uv).rgb);\n"
    "  if ((gamma & 1u) != 0u) {\n"
    "    float3 r;\n"
    "    for (int k = 0; k < 3; k++) { float f = c[k] * 255.0; int i = min(int(floor(f)), 255); int j = min(i + 1, 255);\n"
    "      r[k] = mix(ramp[i][k], ramp[j][k], f - float(i)); }\n"
    "    c = r;\n"
    "  }\n"
    "  if ((gamma & 2u) != 0u) c = saturate(c + dither(in.pos.xy, gamma));\n"
    "  return float4(c, 1.0); }\n"
    // enhanced presentation (rt_enh.h): HDR expansion and GT7 tone mapping. The GT7 functions are a port of
    // gt7_tone_mapping.cpp v1.0 (Polyphony Digital Inc., MIT License; the notice is at present_tm_consts).
    // Units of the output: 1.0 = SDR white, the EDR unit of macOS. k.paper is paper white and k.target the peak in these
    // units (both 1 for SDR output; present_tm_consts). rel: the peak of the expansion as a factor of paper white (1 or
    // more); w, h: the size of the drawable in pixels. GT7: the units of the reference (frame-buffer units, 1.0 = 100
    // nits): gt is the target luminance of the curve, expo the factor from the game's linear color to these units and
    // oscale the factor from the curve's output to the output units; kA .. fade1 and target_i are the curve constants.
    "struct TM { uint mode; float paper, target, target_i; float kA, kB, kC, mid; float lin, toe, blend, fade0; float fade1, rel, w, h;\n"
    "            float gt, expo, oscale, pad; };\n"
    "static float pq_enc(float n) {\n"
    "  float ym = pow(max(n, 0.0) / 10000.0, 0.1593017578125);\n"
    "  return pow((0.8359375 + 18.8515625 * ym) / (1.0 + 18.6875 * ym), 78.84375); }\n"
    "static float pq_dec(float v) {\n"
    "  float np = pow(saturate(v), 1.0 / 78.84375);\n"
    "  float l = max(np - 0.8359375, 0.0) / (18.8515625 - 18.6875 * np);\n"
    "  return pow(l, 1.0 / 0.1593017578125) * 10000.0; }\n"
    "static float3 rgb_to_ucs(float3 c) {\n"
    "  float3 lms = float3(1688.0 * c.r + 2146.0 * c.g + 262.0 * c.b, 683.0 * c.r + 2951.0 * c.g + 462.0 * c.b,\n"
    "                      99.0 * c.r + 309.0 * c.g + 3688.0 * c.b) / 4096.0;\n"
    "  float3 p = float3(pq_enc(lms.x * 100.0), pq_enc(lms.y * 100.0), pq_enc(lms.z * 100.0));\n"
    "  return float3(2048.0 * p.x + 2048.0 * p.y, 6610.0 * p.x - 13613.0 * p.y + 7003.0 * p.z,\n"
    "                17933.0 * p.x - 17390.0 * p.y - 543.0 * p.z) / 4096.0; }\n"
    "static float3 ucs_to_rgb(float3 u) {\n"
    "  float l = u.x + 0.00860904 * u.y + 0.11103 * u.z, m = u.x - 0.00860904 * u.y - 0.11103 * u.z;\n"
    "  float s = u.x + 0.560031 * u.y - 0.320627 * u.z;\n"
    "  float3 q = float3(pq_dec(l), pq_dec(m), pq_dec(s)) / 100.0;\n"
    "  return max(float3(3.43661 * q.x - 2.50645 * q.y + 0.0698454 * q.z, -0.79133 * q.x + 1.9836 * q.y - 0.192271 * q.z,\n"
    "                    -0.0259499 * q.x - 0.0989137 * q.y + 1.12486 * q.z), 0.0); }\n"
    "static float gt7_curve(float x, constant TM &k) {\n"
    "  if (x < 0.0) return 0.0;\n"
    "  float wl = smoothstep(0.0, k.mid, x), wt = 1.0 - wl;\n"
    "  if (x < k.lin * k.gt) return wt * (k.mid * pow(x / k.mid, k.toe)) + wl * x;\n"
    "  return k.kA + k.kB * exp(x * k.kC); }\n"
    "static float3 gt7(float3 c, constant TM &k) {\n"
    "  float3 u = rgb_to_ucs(c);\n"
    "  float3 sk = float3(gt7_curve(c.r, k), gt7_curve(c.g, k), gt7_curve(c.b, k));\n"
    "  float3 su = rgb_to_ucs(sk);\n"
    "  float cs = 1.0 - smoothstep(k.fade0, k.fade1, u.x / k.target_i);\n"
    "  float3 sc = ucs_to_rgb(float3(su.x, u.y * cs, u.z * cs));\n"
    "  return min((1.0 - k.blend) * sk + k.blend * sc, float3(k.gt)); }\n"
    "static float3 tm_ramp(float3 c, constant float4 *ramp, uint gamma) {\n"
    "  if ((gamma & 1u) == 0u) return c;\n"
    "  float3 r;\n"
    "  for (int j = 0; j < 3; j++) { float f = c[j] * 255.0; int i = min(int(floor(f)), 255); int i1 = min(i + 1, 255);\n"
    "    r[j] = mix(ramp[i][j], ramp[i1][j], f - float(i)); }\n"
    "  return r; }\n"
    "static float3 tm_lin(float3 c) { return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }\n"
    "static float tm_luma(float3 l) { return dot(l, float3(0.2126, 0.7152, 0.0722)); }\n"
    // the scene: a linear color -> the output color (EDR units for HDR output: 1.0 = SDR white; linear SDR
    // otherwise). Its luminance Y is 1 at the white of the game's image and above 1 for the scene's colors above white
    // (tm_pre). The game's image is SDR: its highlights get back a range above paper white. The expansion acts on
    // luminance and scales the three channels by the same factor, so the hue and the saturation stay those of the
    // game. Below the knee (0.25, sRGB 0.54) the luminance does not change. Units of the curve: 1.0 = paper white.
    //   HDR output without tone mapping: white gets w1 = 1 + 0.75 (rel - 1), so that a quarter of the range above
    //   paper white stays for the colors above white (rel = peak / paper white). From the knee to white the curve
    //     knee + (w1 - knee) * (a u + (1 - a) u^3), u = (Y - knee) / (1 - knee), a = (1 - knee) / (w1 - knee)
    //   has slope 1 at the knee. Above white, w1 + (rel - w1) (1 - exp(-s (Y - 1) / (rel - w1))) continues it with the
    //   same slope s and approaches the peak.
    //   With GT7 tone mapping: the reference operator. The game's linear color (Rec. 709 primaries, converted to the
    //   Rec. 2020 input of the reference) times expo is the scene in frame-buffer units; the output times oscale is
    //   the output color.
    "static float3 to2020(float3 x) {\n"
    "  return float3(0.6274040 * x.r + 0.3292820 * x.g + 0.0433136 * x.b, 0.0690970 * x.r + 0.9195400 * x.g + 0.0113612 * x.b,\n"
    "                0.0163916 * x.r + 0.0880132 * x.g + 0.8955950 * x.b); }\n"
    "static float3 to709(float3 o) {\n"
    "  return float3(1.6604910 * o.r - 0.5876411 * o.g - 0.0728499 * o.b, -0.1245505 * o.r + 1.1328999 * o.g - 0.0083494 * o.b,\n"
    "                -0.0181508 * o.r - 0.1005789 * o.g + 1.1187297 * o.b); }\n"
    "static float3 scene_tm(float3 l, constant TM &k) {\n"
    "  if ((k.mode & 1u) != 0u) return to709(gt7(to2020(l * k.expo), k)) * k.oscale;\n"
    "  float knee = 0.25;\n"
    "  float Y = tm_luma(l);\n"
    "  float Ye;\n"
    "  {\n"
    "    float w1 = 1.0 + 0.75 * (k.rel - 1.0);\n"
    "    float a = min((1.0 - knee) / max(w1 - knee, 1e-3), 1.0);\n"
    "    if (Y <= knee) Ye = Y;\n"
    "    else if (Y <= 1.0) { float u = (Y - knee) / (1.0 - knee); Ye = knee + (w1 - knee) * (a * u + (1.0 - a) * u * u * u); }\n"
    "    else {\n"
    "      float sl = (w1 - knee) / (1.0 - knee) * (3.0 - 2.0 * a), room = k.rel - w1;\n"
    "      Ye = room > 1e-4 ? w1 + room * (1.0 - exp(-sl * (Y - 1.0) / room)) : w1;\n"
    "    }\n"
    "  }\n"
    "  float3 x = to2020(l * (Y > 1e-6 ? Ye / Y : 1.0) * k.paper);\n"
    // without tone mapping a color above the peak is scaled down as a whole, so that its hue stays
    "  float xm = max(x.r, max(x.g, x.b));\n"
    "  return to709(xm > k.target ? x * (k.target / xm) : x); }\n"
    // the HDR calibration test image (EDR units), below the rows of the calibration menu. Left: the test of Max
    // Brightness (HDR), as in the RE Engine: a square at the chosen peak in a field at the maximum of the screen (64 x
    // SDR white; macOS clips it to the peak of the screen). The outline of the square is visible while the chosen peak
    // is below the peak of the screen and disappears at the peak of the screen. Right: the test of Brightness (HDR): a
    // bar at paper white (the UI) above ten steps of the game's gray scale (sRGB 0.1 .. 1.0) through the scene curve.
    "static float3 calib(float2 p, float3 c, constant TM &k) {\n"
    "  float s = 0.24 * k.h, y0 = 0.6 * k.h, cx = 0.5 * k.w;\n"
    "  if (p.y < y0 || p.y >= y0 + s) return c;\n"
    "  float ax = cx - 1.55 * s, bx = cx - 0.35 * s;\n"
    "  if (p.x >= ax && p.x < ax + s) {\n"
    "    float2 q = abs(p - float2(ax + 0.5 * s, y0 + 0.5 * s)) / s;\n"
    "    return float3(max(q.x, q.y) < 0.3 ? k.target : 64.0);\n"
    "  }\n"
    "  if (p.x >= bx && p.x < bx + 2.0 * s) {\n"
    "    float q = (p.x - bx) / (0.2 * s);\n"
    "    if (fract(q) < 0.04 || (p.y >= y0 + 0.2 * s && p.y < y0 + 0.24 * s)) return float3(0.0);\n"
    "    if (p.y < y0 + 0.2 * s) return float3(k.paper);\n"
    "    return scene_tm(tm_lin(float3(float(min(int(q), 9) + 1) / 10.0)), k);\n"
    "  }\n"
    "  return c; }\n"
    // mode: 1 GT7 tone mapping, 2 the drawable is an EDR layer (extended linear, the screen's primaries), 8 HDR output,
    // 16 the frame has a scene, 32 the HDR calibration test image, 64 pre (tm_pre) is the scene of this frame before
    // the game's post-processing.
    // t is the back buffer; rt is the back buffer at the end of the scene, before the UI of the frame (t itself when
    // the frame has no UI after the scene). The scene gets the scene curve; the UI stays at paper white. A pixel that
    // the UI changed: final + (scene output - paper white x scene) x min(Y final / Y scene, 1). This keeps the curve
    // for an unchanged pixel, scales it with a fade or a dark panel over the scene, and adds UI light at paper white.
    // The colors above white: the game's post-processing clamps the scene to white. Where pre is above white, the
    // scene gets the factor luminance(pre) / luminance(pre clamped to white) (linear light). The factor keeps the hue
    // of the post-processed scene (for example a black-and-white or a sepia effect) and is 1 where pre is not above
    // white.
    "fragment float4 present_enh_fs(QV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
    "                               constant float4 *ramp [[buffer(0)]], constant uint &gamma [[buffer(1)]],\n"
    "                               constant TM &k [[buffer(2)]], texture2d<float> rt [[texture(1)]],\n"
    "                               texture2d<float> pre [[texture(2)]]) {\n"
    "  float3 l = tm_lin(tm_ramp(saturate(t.sample(s, in.uv).rgb), ramp, gamma));\n"
    "  float3 disp = l;\n"
    "  if ((k.mode & 9u) != 0u) {\n"
    "    if ((k.mode & 16u) != 0u) {\n"
    "      float3 r = tm_lin(tm_ramp(saturate(rt.sample(s, in.uv).rgb), ramp, gamma));\n"
    "      float yr = tm_luma(r);\n"
    "      float w = yr > 1e-6 ? min(tm_luma(l) / yr, 1.0) : 0.0;\n"
    "      float g = 1.0;\n"
    "      if ((k.mode & 64u) != 0u) {\n"
    "        float3 q = max(pre.sample(s, in.uv).rgb, 0.0);\n"
    "        g = max(tm_luma(tm_lin(q)) / max(tm_luma(tm_lin(min(q, 1.0))), 1e-4), 1.0);\n"
    "      }\n"
    "      disp = max(l * k.paper + (scene_tm(r * g, k) - r * k.paper) * w, 0.0);\n"
    "    } else {\n"
    "      disp = l * k.paper;\n"
    "    }\n"
    "  }\n"
    "  if ((k.mode & 32u) != 0u) disp = calib(in.pos.xy, disp, k);\n"
    "  if ((k.mode & 2u) != 0u) return float4(disp, 1.0);\n"
    "  disp = saturate(disp);\n"
    "  float3 e = select(1.055 * pow(disp, 1.0 / 2.4) - 0.055, disp * 12.92, disp <= 0.0031308);\n"
    "  if ((gamma & 2u) != 0u) e = saturate(e + dither(in.pos.xy, gamma));\n"
    "  return float4(e, 1.0); }\n";

typedef struct { char tag; uint8_t mask, blend, samples; uint16_t fmt[4]; uint16_t dfmt, sfmt; } UtilKey;
// tag 'C': clear quad (mask: color written); 'K': copy; 'O': opaque copy; 'P': present; 'H': enhanced present
// (tone mapping, HDR); blend: alpha blending
static id<MTLRenderPipelineState> util_pso(Device *d, const UtilKey *k) {
    NSData *key = [NSData dataWithBytes:k length:sizeof *k];
    id<MTLRenderPipelineState> p = d->util[key];
    if (p) return p;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [d->util_lib newFunctionWithName:@"quad_vs"];
    NSString *fs = k->tag == 'C' ? @"clear_fs" : k->tag == 'K' ? @"copy_fs" : k->tag == 'O' ? @"copy_opaque_fs" :
                   k->tag == 'H' ? @"present_enh_fs" : @"present_fs";
    pd.fragmentFunction = [d->util_lib newFunctionWithName:fs];
    for (int i = 0; i < 4; i++) if (k->fmt[i]) {
        pd.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->fmt[i];
        pd.colorAttachments[i].writeMask = k->mask ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        if (k->blend) {
            pd.colorAttachments[i].blendingEnabled = YES;
            pd.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[i].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pd.colorAttachments[i].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[i].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
    }
    pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt;
    pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt;
    pd.rasterSampleCount = k->samples ? k->samples : 1;
    NSError *err = nil;
    p = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) rt_fatal("[d3d9] utility pipeline '%c' not created: %s", k->tag, err.localizedDescription.UTF8String);
    if (d->util.count >= 256) [d->util removeAllObjects];
    d->util[key] = p;
    return p;
}
static id<MTLDepthStencilState> clear_dss(Device *d, uint32_t flags) {
    NSNumber *key = @(0x100 | (flags & 6));
    id<MTLDepthStencilState> s = d->dss_cache[key];
    if (s) return s;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (flags & 2) != 0;
    if (flags & 4) {
        MTLStencilDescriptor *st = [MTLStencilDescriptor new];
        st.stencilCompareFunction = MTLCompareFunctionAlways;
        st.depthStencilPassOperation = MTLStencilOperationReplace;
        st.readMask = st.writeMask = 0xFF;
        dd.frontFaceStencil = st; dd.backFaceStencil = st;
    }
    s = [d->mtl newDepthStencilStateWithDescriptor:dd];
    if (!s) rt_fatal("[d3d9] Metal did not create a depth-stencil state");
    d->dss_cache[key] = s;
    return s;
}
// clear a rectangle of the current targets with a quad (partial clears)
static void clear_quad(Device *d, uint32_t flags, const float *col, float z, uint32_t stencil, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    id<MTLRenderCommandEncoder> e = begin_pass(d);
    UtilKey k; memset(&k, 0, sizeof k);
    k.tag = 'C'; k.mask = (flags & 1) ? 1 : 0; k.samples = (uint8_t)d->pass_ms;
    float cols[4][4];
    int srgb = d->st.rs[RS_SRGBWRITEENABLE] != 0;
    for (int i = 0; i < 4; i++) {
        memcpy(cols[i], col, 16);
        if (!d->rt[i]) continue;
        Tex *t = d->rt[i]->tex;
        k.fmt[i] = (uint16_t)((srgb && t->f->srgb != MTLPixelFormatInvalid) ? t->f->srgb : t->f->mtl);
        if (!t->f->alpha) cols[i][3] = 1.0f;
    }
    if (d->ds) { k.dfmt = (uint16_t)d->ds->tex->f->mtl; if (d->ds->tex->f->kind == 2) k.sfmt = k.dfmt; }
    id<MTLRenderPipelineState> p = util_pso(d, &k);
    [e setRenderPipelineState:p]; d->cur_pso = p;
    id<MTLDepthStencilState> ds = clear_dss(d, flags);
    [e setDepthStencilState:ds]; d->cur_dss = ds;
    [e setStencilReferenceValue:stencil & 0xFF];
    [e setViewport:(MTLViewport){0, 0, d->pass_w, d->pass_h, 0, 1}];
    [e setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
    [e setCullMode:MTLCullModeNone];
    [e setTriangleFillMode:MTLTriangleFillModeFill];
    [e setDepthBias:0 slopeScale:0 clamp:0];
    float W = (float)d->pass_w, H = (float)d->pass_h;
    float r[12] = {2.0f * x0 / W - 1.0f, 1.0f - 2.0f * y0 / H, 2.0f * x1 / W - 1.0f, 1.0f - 2.0f * y1 / H, 0, 0, 1, 1, z, 0, 0, 0};
    [e setVertexBytes:r length:sizeof r atIndex:0];
    [e setFragmentBytes:cols length:sizeof cols atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

// ---------------- temporal anti-aliasing (rt_enh.h aa) ----------------
// MetalFX (d3d9_enh_taa) needs the 3D scene of each frame at a different sub-pixel position. The offset of frame f is
// the Halton (2, 3) point of index f mod 8 + 1, minus 0.5 (8 phases, the MetalFX count for a scale of 1). A frame
// after a frame without anti-aliasing gets no offset (MetalFX starts its history again in that frame). The draws into
// the back buffer with a depth buffer, before the end of the opaque scene (enh_apply), move by the offset; the
// post-processing and the UI after it do not.
#define TAA_PHASES 8
static float taa_halton(uint32_t i, uint32_t b) {
    float r = 0.0f, f = 1.0f / (float)b;
    while (i > 0) { r += f * (float)(i % b); i /= b; f /= (float)b; }
    return r;
}
// returns 1 when frame f has offsets (o), 0 when it has none (o = 0)
static int taa_offset(const Device *d, uint32_t f, float o[2]) {
    o[0] = o[1] = 0.0f;
    if (!rt_enh || rt_enh->aa != 1 || d->taa_frame == 0 || d->taa_frame != f - 1) return 0;
    uint32_t i = f % TAA_PHASES + 1;
    o[0] = taa_halton(i, 2) - 0.5f; o[1] = taa_halton(i, 3) - 0.5f;
    return 1;
}
// The texture LOD bias of the scene draws with an offset: log2(input size / output size) - 1 (the MetalFX
// integration guide of Apple), with the input size equal to the output size. It is added to D3DSAMP_MIPMAPLODBIAS.
// MetalFX gets back the detail of the sharper mipmap levels from the offsets of the frames.
#define TAA_LOD_BIAS (-1.0f)
// the offset of this draw (o, pixels with y down); returns its texture LOD bias (TAA_LOD_BIAS with an offset, or 0)
static float taa_draw_offset(const Device *d, float o[2]) {
    uint32_t f = d->frame + 1;
    const Surf *r0 = d->rt[0];
    if (r0 && r0->swap && d->ds && d->enh_done != f && taa_offset(d, f, o)) return TAA_LOD_BIAS;
    o[0] = o[1] = 0.0f;
    return 0.0f;
}

// ---------------- constants ----------------
static void pos_fixup(Device *d, float *f) {
    State *S = &d->st;
    // the half-pixel offset is that of the viewport of the draw (in 4:3 for the interface: d3d9_enh_ui_vs)
    float w = (float)S->vp[2] * d->ui_k, h = (float)S->vp[3], zr = fbits(S->vp[5]) - fbits(S->vp[4]);
    // with the sub-pixel offset of temporal anti-aliasing (taa_jit, in pixels with y down)
    f[0] = w > 0 ? ((63.0f / 64.0f) + 2.0f * d->taa_jit[0]) / w : 0.0f;
    f[1] = h > 0 ? -((63.0f / 64.0f) + 2.0f * d->taa_jit[1]) / h : 0.0f;
    f[2] = fbits(S->rs[RS_DEPTHBIAS]) / (zr != 0.0f ? zr : 1.0f);
    f[3] = fbits(S->rs[RS_POINTSIZE]);
}
static void mat_mul(const float *a, const float *b, float *o) {   // row-vector D3D matrices: o = a * b
    float t[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof t);
}
// the inverse of a 4x4 matrix (Gauss-Jordan elimination with partial pivoting, in double precision); 0 when the matrix
// is singular
static int mat_inv(const float *m, float *o) {
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][4 + j] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (!(fabs(a[p][c]) > 1e-12)) return 0;
        if (p != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double iv = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= iv;
        for (int r = 0; r < 4; r++) if (r != c && a[r][c] != 0.0) { double k = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= k * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        o[i * 4 + j] = (float)a[i][4 + j];
        if (!isfinite(o[i * 4 + j])) return 0;
    }
    return 1;
}
// the normal matrix: inverse transpose of the upper 3x3
static void normal_mat(const float *m, float *o) {
    float a = m[0], b = m[1], c3 = m[2], dd = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float k00 = e * i - f * h, k01 = -(dd * i - f * g), k02 = dd * h - e * g;
    float k10 = -(b * i - c3 * h), k11 = a * i - c3 * g, k12 = -(a * h - b * g);
    float k20 = b * f - c3 * e, k21 = -(a * f - c3 * dd), k22 = a * e - b * dd;
    float det = a * k00 + b * k01 + c3 * k02;
    float s = det != 0.0f ? 1.0f / det : 0.0f;
    memset(o, 0, 64);
    // (M^-1)^T = cofactor matrix / det
    o[0] = k00 * s; o[1] = k01 * s; o[2] = k02 * s;
    o[4] = k10 * s; o[5] = k11 * s; o[6] = k12 * s;
    o[8] = k20 * s; o[9] = k21 * s; o[10] = k22 * s;
    o[15] = 1.0f;
}
static void ff_consts(Device *d, FFVSConsts *k) {
    State *S = &d->st;
    memset(k, 0, sizeof *k);
    pos_fixup(d, k->pos_fixup);
    memcpy(k->proj, S->xf[3], 64);
    for (int i = 0; i < 4; i++) { mat_mul(S->xf[256 + i], S->xf[2], k->wv[i]); normal_mat(k->wv[i], k->wvn[i]); }
    for (int i = 0; i < 8; i++) memcpy(k->tex[i], S->xf[16 + i], 64);
    float w = (float)S->vp[2], h = (float)S->vp[3];
    k->vp[0] = (float)S->vp[0]; k->vp[1] = (float)S->vp[1]; k->vp[2] = w > 0 ? 2.0f / w : 0.0f; k->vp[3] = h > 0 ? 2.0f / h : 0.0f;
    k->zr[0] = fbits(S->vp[4]); k->zr[1] = fbits(S->vp[5]) - fbits(S->vp[4]);
    memcpy(k->mat_diffuse, &S->material[0], 16); memcpy(k->mat_ambient, &S->material[4], 16);
    memcpy(k->mat_specular, &S->material[8], 16); memcpy(k->mat_emissive, &S->material[12], 16);
    k->mat_power[0] = S->material[16];
    color4(S->rs[RS_AMBIENT], k->ambient);
    k->fog[0] = fbits(S->rs[RS_FOGSTART]); k->fog[1] = fbits(S->rs[RS_FOGEND]); k->fog[2] = fbits(S->rs[RS_FOGDENSITY]);
    k->point[0] = fbits(S->rs[RS_POINTSIZE]); k->point[1] = fbits(S->rs[RS_POINTSIZE_MIN]);
    k->point[2] = fbits(S->rs[RS_POINTSIZE_MAX]); k->point[3] = h;
    k->point_abc[0] = fbits(S->rs[RS_POINTSCALE_A]); k->point_abc[1] = fbits(S->rs[RS_POINTSCALE_B]); k->point_abc[2] = fbits(S->rs[RS_POINTSCALE_C]);
    const float *V = S->xf[2];
    for (uint32_t i = 0; i < S->nactive; i++) {
        Light *l = light_find(S, S->active[i]);
        if (!l) continue;
        FFLight *o = &k->light[i];
        float L[26]; memcpy(L, l->data, sizeof L);
        memcpy(o->diffuse, &L[1], 16); memcpy(o->specular, &L[5], 16); memcpy(o->ambient, &L[9], 16);
        float px = L[13], py = L[14], pz = L[15], dx = L[16], dy = L[17], dz = L[18];
        o->position[0] = px * V[0] + py * V[4] + pz * V[8] + V[12];
        o->position[1] = px * V[1] + py * V[5] + pz * V[9] + V[13];
        o->position[2] = px * V[2] + py * V[6] + pz * V[10] + V[14];
        o->position[3] = 1.0f;
        o->direction[0] = dx * V[0] + dy * V[4] + dz * V[8];
        o->direction[1] = dx * V[1] + dy * V[5] + dz * V[9];
        o->direction[2] = dx * V[2] + dy * V[6] + dz * V[10];
        o->att[0] = L[19]; o->att[1] = L[20]; o->att[2] = L[21]; o->att[3] = L[22];
        o->spot[0] = L[23]; o->spot[1] = cosf(L[24] * 0.5f); o->spot[2] = cosf(L[25] * 0.5f);
    }
}
static void ps_consts_header(Device *d, PSConsts *k) {
    State *S = &d->st;
    k->alpha_ref[0] = (float)(S->rs[RS_ALPHAREF] & 0xFF);
    k->alpha_ref[1] = S->rs[RS_ALPHATESTENABLE] ? 1.0f : 0.0f;
    k->alpha_ref[2] = (float)S->rs[RS_ALPHAFUNC];
    k->fog[0] = fbits(S->rs[RS_FOGSTART]); k->fog[1] = fbits(S->rs[RS_FOGEND]); k->fog[2] = fbits(S->rs[RS_FOGDENSITY]);
    color4(S->rs[RS_FOGCOLOR], k->fog_color);
    memset(k->fog_mode, 0, sizeof k->fog_mode);
    if (S->rs[RS_FOGENABLE]) {
        uint32_t table = S->rs[RS_FOGTABLEMODE];
        if (table >= 1 && table <= 3) {
            const float *P = S->xf[3];
            // pixel fog uses the eye distance (w) with a perspective projection and fixed-function vertices
            int wfog = !S->vs && !(P[3] == 0.0f && P[7] == 0.0f && P[11] == 0.0f && P[15] == 1.0f);
            k->fog_mode[0] = (float)table; k->fog_mode[1] = wfog ? 2.0f : 1.0f;
        } else { k->fog_mode[0] = 1.0f; k->fog_mode[1] = 0.0f; }   // the vertex fog factor
    }
    color4(S->rs[RS_TEXTUREFACTOR], k->tfactor);
    for (int i = 0; i < 8; i++) {
        color4(S->tss[i][TSS_CONSTANT], k->tss_const[i]);
        k->bumpenv[i][0] = fbits(S->tss[i][TSS_BUMPENVMAT00]); k->bumpenv[i][1] = fbits(S->tss[i][TSS_BUMPENVMAT01]);
        k->bumpenv[i][2] = fbits(S->tss[i][TSS_BUMPENVMAT10]); k->bumpenv[i][3] = fbits(S->tss[i][TSS_BUMPENVMAT11]);
        k->bumplum[i][0] = fbits(S->tss[i][TSS_BUMPENVLSCALE]); k->bumplum[i][1] = fbits(S->tss[i][TSS_BUMPENVLOFFSET]);
        k->bumplum[i][2] = k->bumplum[i][3] = 0.0f;
    }
    for (int s = 0; s < 16; s++) k->lod_bias[s >> 2][s & 3] = fbits(S->samp[s][SAMP_MIPMAPLODBIAS]) + d->taa_lod;
    memcpy(k->i, S->psi, sizeof k->i);
    memcpy(k->b, S->psb, sizeof k->b);
}

// ---------------- drawing ----------------
static int ff_reg(int usage, int index) {
    switch (usage) {
    case DU_POSITION: case DU_POSITIONT: return index == 0 ? FF_IN_POSITION : -1;
    case DU_BLENDWEIGHT: return index == 0 ? FF_IN_BLENDWEIGHT : -1;
    case DU_BLENDINDICES: return index == 0 ? FF_IN_BLENDINDICES : -1;
    case DU_NORMAL: return index == 0 ? FF_IN_NORMAL : -1;
    case DU_PSIZE: return index == 0 ? FF_IN_PSIZE : -1;
    case DU_COLOR: return index == 0 ? FF_IN_COLOR0 : index == 1 ? FF_IN_COLOR1 : -1;
    case DU_TEXCOORD: return index < 8 ? FF_IN_TEXCOORD0 + index : -1;
    default: return -1;
    }
}
static void ff_vs_key(Device *d, FFVSKey *k, int points) {
    State *S = &d->st;
    switch (S->rs[RS_VERTEXBLEND]) {
    case 1: k->blend = 1; break;
    case 2: k->blend = 2; break;
    case 3: k->blend = 3; break;
    case 255: LOG_ONCE("[d3d9] D3DVBF_TWEENING is not supported: vertices are not blended"); break;
    default: break;
    }
    if (S->rs[RS_INDEXEDVERTEXBLENDENABLE]) LOG_ONCE("[d3d9] indexed vertex blending is not offered (MaxVertexBlendMatrixIndex 0)");
    k->lighting = S->rs[RS_LIGHTING] && !k->pretransformed;
    k->normalize = S->rs[RS_NORMALIZENORMALS] != 0;
    k->localviewer = S->rs[RS_LOCALVIEWER] != 0;
    k->specular = S->rs[RS_SPECULARENABLE] != 0;
    static const int src_rs[4] = {RS_DIFFUSEMATERIALSOURCE, RS_SPECULARMATERIALSOURCE, RS_AMBIENTMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE};
    for (int i = 0; i < 4; i++) {
        uint32_t v = S->rs[RS_COLORVERTEX] ? S->rs[src_rs[i]] : 0;
        if ((v == 1 && !k->has_color0) || (v == 2 && !k->has_color1) || v > 2) v = 0;
        k->mat_src[i] = (uint8_t)v;
    }
    if (k->lighting)
        for (uint32_t i = 0; i < S->nactive; i++) { Light *l = light_find(S, S->active[i]); k->light[i] = (uint8_t)(l ? l->data[0] : 0); }
    uint32_t table = S->rs[RS_FOGTABLEMODE], vmode = S->rs[RS_FOGVERTEXMODE];
    k->fog_mode = (uint8_t)(S->rs[RS_FOGENABLE] && (table == 0 || table > 3) && vmode <= 3 ? vmode : 0);
    k->range_fog = S->rs[RS_RANGEFOGENABLE] != 0;
    for (int i = 0; i < 8; i++) {
        uint32_t tci = S->tss[i][TSS_TEXCOORDINDEX];
        k->tci[i] = (uint8_t)((((tci >> 16) & 0xF) << 4) | (tci & 7));
        uint32_t n = S->tss[i][TSS_TEXTURETRANSFORMFLAGS] & 0xFF;
        k->ttff[i] = (uint8_t)(n > 4 ? 4 : n);
    }
    k->points = (uint8_t)points;
    k->point_scale = points && S->rs[RS_POINTSCALEENABLE];
}
static void ff_ps_key(Device *d, FFPSKey *k) {
    State *S = &d->st;
    for (int i = 0; i < 8; i++) {
        const uint32_t *t = S->tss[i];
        k->st[i].cop = (uint8_t)t[TSS_COLOROP]; k->st[i].aop = (uint8_t)t[TSS_ALPHAOP];
        k->st[i].ca[0] = (uint8_t)t[TSS_COLORARG0]; k->st[i].ca[1] = (uint8_t)t[TSS_COLORARG1]; k->st[i].ca[2] = (uint8_t)t[TSS_COLORARG2];
        k->st[i].aa[0] = (uint8_t)t[TSS_ALPHAARG0]; k->st[i].aa[1] = (uint8_t)t[TSS_ALPHAARG1]; k->st[i].aa[2] = (uint8_t)t[TSS_ALPHAARG2];
        k->st[i].res = (uint8_t)t[TSS_RESULTARG];
        k->st[i].ttype = (uint8_t)tex_type_of(S->tex[i]);
        uint32_t f = t[TSS_TEXTURETRANSFORMFLAGS];
        if (f & 0x100) { uint32_t n = f & 0xFF; k->st[i].proj = (uint8_t)(n >= 2 && n <= 4 ? n : 4); }
        if (t[TSS_COLOROP] == 1) break;
    }
    k->specular = S->rs[RS_SPECULARENABLE] != 0;
}
static Tex *bound_tex(Device *d, int s) {
    Obj *o = d->st.tex[s];
    if (!o) return NULL;
    Tex *t = (Tex *)o;
    return t->mtl ? t : NULL;
}
typedef struct { __unsafe_unretained id<MTLBuffer> buf; uint32_t off; uint32_t stride; int on; uint32_t len; const uint8_t *cpu; } UpStream;

// everything a draw needs; 0: skip the draw (the reason was logged)
static int prepare(Device *d, int points, const UpStream *up, uint32_t *instances, int indexed) {
    State *S = &d->st;
    Decl *dc = S->decl;
    if (!dc) { LOG_ONCE("[d3d9] draw without a vertex declaration or FVF"); return 0; }
    if (!d->rt[0]) { LOG_ONCE("[d3d9] draw without a render target"); return 0; }
    Shader *vs = S->vs;
    int ff = !vs;
    uint8_t cls[16] = {0};
    uint16_t present = 0;
    PSOKey pk; memset(&pk, 0, sizeof pk);
    FFVSKey fk; memset(&fk, 0, sizeof fk);
    uint32_t streams = 0, ends[16] = {0};
    for (uint32_t i = 0; i < dc->n; i++) {
        int usage = dc->el[i].usage, index = dc->el[i].index, type = dc->el[i].type, r = -1;
        if (type > DT_FLOAT16_4 || dc->el[i].stream > 15) continue;
        if (ff) r = ff_reg(usage, index);
        else {
            for (int k = 0; k < 16 && r < 0; k++)
                if (vs->info.in[k].used && vs->info.in[k].usage == usage && vs->info.in[k].index == index) r = k;
            if (r < 0 && usage == DU_POSITIONT)
                for (int k = 0; k < 16 && r < 0; k++)
                    if (vs->info.in[k].used && vs->info.in[k].usage == DU_POSITION && vs->info.in[k].index == index) r = k;
        }
        if (r < 0 || (present >> r & 1)) continue;
        if (dt_mtl[type] == MTLVertexFormatInvalid) { LOG_ONCE("[d3d9] vertex element type %d (UDEC3) is not supported", type); return 0; }
        present |= (uint16_t)(1u << r);
        cls[r] = (uint8_t)dt_class(type);
        uint32_t st = dc->el[i].stream;
        pk.vl.attr[r].fmt = (uint8_t)dt_mtl[type]; pk.vl.attr[r].buf = (uint8_t)(1 + st); pk.vl.attr[r].off = dc->el[i].offset;
        streams |= 1u << st;
        uint32_t end = (uint32_t)dc->el[i].offset + dt_size[type];
        if (end > ends[st]) ends[st] = end;
        if (ff) {
            if (usage == DU_POSITIONT) fk.pretransformed = 1;
            else if (usage == DU_NORMAL) fk.has_normal = 1;
            else if (usage == DU_PSIZE) fk.has_psize = 1;
            else if (usage == DU_COLOR) { if (index == 0) fk.has_color0 = 1; else fk.has_color1 = 1; }
            else if (usage == DU_TEXCOORD) fk.tc_dim[index] = dt_comps[type];
        }
    }
    *instances = 1;
    if (indexed && (S->stream[0].freq & 0x40000000u)) *instances = S->stream[0].freq & 0x3FFFFFFFu;
    for (uint32_t s = 0; s < 16; s++) if (streams >> s & 1) {
        uint32_t stride, off;
        if (s == 0 && up && up->on) { stride = up->stride; off = up->off; }
        else {
            if (!S->stream[s].vb) { LOG_ONCE("[d3d9] a draw reads stream %u, which has no vertex buffer", s); return 0; }
            stride = S->stream[s].stride; off = S->stream[s].off;
        }
        if ((stride & 3) || (off & 3)) { LOG_ONCE("[d3d9] stream %u: stride %u / offset %u not a multiple of 4 bytes", s, stride, off); return 0; }
        if (!stride) { pk.vl.lay[1 + s].stride = (ends[s] + 3) & ~3u; pk.vl.lay[1 + s].step = MTLVertexStepFunctionConstant; }
        else {
            pk.vl.lay[1 + s].stride = stride;
            uint32_t fq = S->stream[s].freq;
            if (*instances > 1 && (fq & 0x80000000u)) { pk.vl.lay[1 + s].step = MTLVertexStepFunctionPerInstance; pk.vl.lay[1 + s].rate = (uint16_t)(fq & 0xFFFF); }
            else pk.vl.lay[1 + s].step = MTLVertexStepFunctionPerVertex;
        }
    }
    // shaders
    D3D9Fn *vf, *pf;
    if (ff) { ff_vs_key(d, &fk, points); vf = ff_vs_fn(d, &fk, cls, present, points); }
    else vf = vs_fn(d, vs, cls, present, points);
    int flat = S->rs[RS_SHADEMODE] == 1, sprite = points && S->rs[RS_POINTSPRITEENABLE];
    if (S->ps) pf = ps_fn(d, S->ps, flat, sprite);
    else { FFPSKey qk; memset(&qk, 0, sizeof qk); ff_ps_key(d, &qk); pf = ff_ps_fn(d, &qk, flat, sprite); }
    if (!vf || !vf->fn || !pf || !pf->fn) return 0;
    // pipeline
    pk.vid = vf->fid; pk.pid = pf->fid;
    int srgb = S->rs[RS_SRGBWRITEENABLE] != 0;
    uint32_t sb = S->rs[RS_SRCBLEND], db = S->rs[RS_DESTBLEND], sba = S->rs[RS_SRCBLENDALPHA], dba = S->rs[RS_DESTBLENDALPHA];
    if (sb == 12) { sb = 5; db = 6; } else if (sb == 13) { sb = 6; db = 5; }
    if (!S->rs[RS_SEPARATEALPHABLENDENABLE]) { sba = sb; dba = db; }
    else if (sba == 12) { sba = 5; dba = 6; } else if (sba == 13) { sba = 6; dba = 5; }
    for (int i = 0; i < 4; i++) {
        if (!d->rt[i]) continue;
        const Fmt *f = d->rt[i]->tex->f;
        pk.c[i].fmt = (uint16_t)((srgb && f->srgb != MTLPixelFormatInvalid) ? f->srgb : f->mtl);
        uint32_t cw = i == 0 ? S->rs[RS_COLORWRITEENABLE] : S->rs[RS_COLORWRITEENABLE1 + i - 1];
        pk.c[i].mask = (uint8_t)(((cw & 1) ? MTLColorWriteMaskRed : 0) | ((cw & 2) ? MTLColorWriteMaskGreen : 0) |
                                 ((cw & 4) ? MTLColorWriteMaskBlue : 0) | ((cw & 8) && f->alpha ? MTLColorWriteMaskAlpha : 0));
        if (S->rs[RS_ALPHABLENDENABLE] && f->blend) {
            pk.c[i].en = 1;
            pk.c[i].sb = (uint8_t)blend_factor(sb, f->alpha); pk.c[i].db = (uint8_t)blend_factor(db, f->alpha);
            pk.c[i].op = (uint8_t)blend_op(S->rs[RS_BLENDOP]);
            pk.c[i].sba = (uint8_t)blend_factor(sba, f->alpha); pk.c[i].dba = (uint8_t)blend_factor(dba, f->alpha);
            pk.c[i].opa = (uint8_t)blend_op(S->rs[RS_SEPARATEALPHABLENDENABLE] ? S->rs[RS_BLENDOPALPHA] : S->rs[RS_BLENDOP]);
        }
    }
    if (d->ds) { pk.dfmt = (uint16_t)d->ds->tex->f->mtl; if (d->ds->tex->f->kind == 2) pk.sfmt = pk.dfmt; }
    pk.samples = (uint8_t)d->rt[0]->tex->ms;
    id<MTLRenderPipelineState> pso = pso_get(d, &pk, vf, pf);
    if (!pso) return 0;
    // textures that need their mipmaps generated (blits, before the render pass)
    for (int s = 0; s < 16; s++) if (pf->samplers >> s & 1) { Tex *t = bound_tex(d, s); if (t && t->mips_dirty) tex_gen_mips(d, t); }
    for (int s = 0; s < 4; s++) if (vf->samplers >> s & 1) { Tex *t = bound_tex(d, 16 + s); if (t && t->mips_dirty) tex_gen_mips(d, t); }
    id<MTLRenderCommandEncoder> e = begin_pass(d);
    uint64_t serial = d->serial;
    if (pso != d->cur_pso) { [e setRenderPipelineState:pso]; d->cur_pso = pso; }
    id<MTLDepthStencilState> dss = dss_get(d);
    if (dss != d->cur_dss) { [e setDepthStencilState:dss]; d->cur_dss = dss; }
    [e setStencilReferenceValue:S->rs[RS_STENCILREF] & 0xFF];
    uint32_t cull = S->rs[RS_CULLMODE];
    [e setFrontFacingWinding:MTLWindingClockwise];
    [e setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone];
    if (S->rs[RS_FILLMODE] == 1) LOG_ONCE("[d3d9] D3DFILL_POINT is drawn as wireframe");
    [e setTriangleFillMode:S->rs[RS_FILLMODE] == 3 ? MTLTriangleFillModeFill : MTLTriangleFillModeLines];
    [e setDepthBias:0 slopeScale:fbits(S->rs[RS_SLOPESCALEDEPTHBIAS]) clamp:0];
    float bf[4]; color4(S->rs[RS_BLENDFACTOR], bf);
    [e setBlendColorRed:bf[0] green:bf[1] blue:bf[2] alpha:bf[3]];
    // viewport and scissor rectangle in Direct3D pixels; the storage of the target can be larger (Tex.sc)
    int64_t sc = d->rt[0]->tex->sc;
    // an interface draw in 4:3 (d3d9_enh_ui_vs): the viewport and the scissor rectangle are scaled horizontally by
    // ui_k about the center of the target
    double uk = d->ui_k, ucx = (double)d->pass_w * 0.5;
    double vx = (double)S->vp[0] * sc, vw = (double)S->vp[2] * sc;
    if (uk != 1.0) { vx = ucx + (vx - ucx) * uk; vw *= uk; }
    [e setViewport:(MTLViewport){vx, (double)S->vp[1] * sc, vw, (double)S->vp[3] * sc, fbits(S->vp[4]), fbits(S->vp[5])}];
    int64_t x0 = 0, y0 = 0, x1 = d->pass_w, y1 = d->pass_h;
    if (S->rs[RS_SCISSORTESTENABLE]) {
        int64_t sx0 = (int64_t)S->scissor[0] * sc, sx1 = (int64_t)S->scissor[2] * sc;
        if (uk != 1.0) { sx0 = (int64_t)floor(ucx + ((double)sx0 - ucx) * uk); sx1 = (int64_t)ceil(ucx + ((double)sx1 - ucx) * uk); }
        if (sx0 > x0) x0 = sx0;
        if (S->scissor[1] * sc > y0) y0 = S->scissor[1] * sc;
        if (sx1 < x1) x1 = sx1;
        if (S->scissor[3] * sc < y1) y1 = S->scissor[3] * sc;
    }
    if (x1 <= x0 || y1 <= y0) return 0;   // nothing inside the scissor rectangle
    [e setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
    // the sub-pixel offset of temporal anti-aliasing changes the half-pixel offset of the vertex constants
    // and adds its texture LOD bias to the pixel constants
    float jt[2];
    float lod = taa_draw_offset(d, jt);
    if (jt[0] != d->taa_jit[0] || jt[1] != d->taa_jit[1]) { d->taa_jit[0] = jt[0]; d->taa_jit[1] = jt[1]; d->vsc_dirty = d->ffc_dirty = 1; }
    if (d->ui_k != d->ui_k_up) { d->ui_k_up = d->ui_k; d->vsc_dirty = d->ffc_dirty = 1; }
    if (lod != d->taa_lod) {
        d->taa_lod = lod; d->psc_dirty = 1;
        if (lod != 0.0f && !d->taa_lod_logged) {
            rt_log("[d3d9] anti-aliasing: frame %u, texture LOD bias %.1f on the scene draws", d->frame + 1, lod);
            d->taa_lod_logged = 1;
        }
    }
    // constants
    __unsafe_unretained id<MTLBuffer> cb; uint32_t co;
    if (ff) {
        if (d->ffc_dirty || d->ffc_up.serial != serial) {
            FFVSConsts *k = ring_alloc(d, sizeof(FFVSConsts), &cb, &co);
            ff_consts(d, k);
            d->ffc_up.buf = (__bridge void *)cb; d->ffc_up.off = co; d->ffc_up.serial = serial; d->ffc_dirty = 0;
        }
        [e setVertexBuffer:(__bridge id<MTLBuffer>)d->ffc_up.buf offset:d->ffc_up.off atIndex:0];
    } else {
        if (d->vsc_dirty || d->vsc_up.serial != serial) {
            VSConsts *k = ring_alloc(d, sizeof(VSConsts), &cb, &co);
            pos_fixup(d, k->pos_fixup);
            memset(k->clip, 0, sizeof k->clip);
            memcpy(k->i, S->vsi, sizeof k->i); memcpy(k->b, S->vsb, sizeof k->b);
            int n = vs->info.max_const_f; if (n > VS_CONST_F) n = VS_CONST_F;
            memcpy(k->c, S->vsf, (size_t)n * 16);
            d->vsc_up.buf = (__bridge void *)cb; d->vsc_up.off = co; d->vsc_up.serial = serial; d->vsc_dirty = 0;
        }
        [e setVertexBuffer:(__bridge id<MTLBuffer>)d->vsc_up.buf offset:d->vsc_up.off atIndex:0];
    }
    if (d->psc_dirty || d->psc_up.serial != serial) {
        PSConsts *k = ring_alloc(d, sizeof(PSConsts), &cb, &co);
        ps_consts_header(d, k);
        int n = S->ps ? S->ps->info.max_const_f : 0; if (n > PS_CONST_F) n = PS_CONST_F;
        memcpy(k->c, S->psf, (size_t)n * 16);
        d->psc_up.buf = (__bridge void *)cb; d->psc_up.off = co; d->psc_up.serial = serial; d->psc_dirty = 0;
    }
    [e setFragmentBuffer:(__bridge id<MTLBuffer>)d->psc_up.buf offset:d->psc_up.off atIndex:0];
    // vertex streams
    for (uint32_t s = 0; s < 16; s++) if (streams >> s & 1) {
        if (s == 0 && up && up->on) { [e setVertexBuffer:up->buf offset:up->off atIndex:1]; continue; }
        VB *b = S->stream[s].vb;
        [e setVertexBuffer:(__bridge id<MTLBuffer>)b->st.buf offset:b->st.off + S->stream[s].off atIndex:1 + s];
        b->last_use = serial;
    }
    // textures and samplers
    for (int s = 0; s < 16; s++) if (pf->samplers >> s & 1) {
        int want = pf->stype[s] ? pf->stype[s] : 2;
        Tex *t = bound_tex(d, s);
        if (t && tex_type_of(&t->o) != want) { LOG_ONCE("[d3d9] sampler %d: the bound texture's type does not match the shader", s); t = NULL; }
        [e setFragmentTexture:t ? tex_view(t, S->samp[s][SAMP_SRGBTEXTURE] != 0) : d->dummy[want] atIndex:(NSUInteger)s];
        [e setFragmentSamplerState:samp_get(d, s, t) atIndex:(NSUInteger)s];
        if (t) t->last_use = serial;
    }
    for (int s = 0; s < 4; s++) if (vf->samplers >> s & 1) {
        int want = vf->stype[s] ? vf->stype[s] : 2;
        Tex *t = bound_tex(d, 16 + s);
        if (t && tex_type_of(&t->o) != want) t = NULL;
        [e setVertexTexture:t ? tex_view(t, S->samp[16 + s][SAMP_SRGBTEXTURE] != 0) : d->dummy[want] atIndex:(NSUInteger)s];
        [e setVertexSamplerState:samp_get(d, 16 + s, t) atIndex:(NSUInteger)s];
        if (t) t->last_use = serial;
    }
    return 1;
}
// vertices (or indices) of `prims` primitives of D3DPRIMITIVETYPE `pt`
static int prim_count(uint32_t pt, uint32_t prims, MTLPrimitiveType *mt, uint32_t *n) {
    switch (pt) {
    case 1: *mt = MTLPrimitiveTypePoint; *n = prims; return 1;
    case 2: *mt = MTLPrimitiveTypeLine; *n = prims * 2; return 1;
    case 3: *mt = MTLPrimitiveTypeLineStrip; *n = prims + 1; return 1;
    case 4: *mt = MTLPrimitiveTypeTriangle; *n = prims * 3; return 1;
    case 5: *mt = MTLPrimitiveTypeTriangleStrip; *n = prims + 2; return 1;
    case 6: *mt = MTLPrimitiveTypeTriangle; *n = prims + 2; return 1;   // fans become lists
    default: return 0;
    }
}
// ---------------- scene enhancements (rt_enh.h) ----------------
RtEnh *rt_enh;
static struct { uint32_t w, h, fmt, creg; } enh_lp;
void d3d9_enh_light_pass(uint32_t w, uint32_t h, uint32_t fmt, uint32_t creg) {
    if (!w || !h || creg > 256 - 4) rt_fatal("[d3d9] d3d9_enh_light_pass: bad light pass %ux%u, constants c%u", w, h, creg);
    enh_lp.w = w; enh_lp.h = h; enh_lp.fmt = fmt; enh_lp.creg = creg;
}
// d3d9_enh_camera_vs: the camera comes from the vertex shader constants creg .. creg + 3
static struct { int on; uint32_t creg; } enh_camvs;
static uint32_t enh_bb_w, enh_bb_h;   // the back buffer of the last device reset (d3d9_enh_back_buffer)
void d3d9_enh_back_buffer(uint32_t *w, uint32_t *h) { *w = enh_bb_w; *h = enh_bb_h; }
void d3d9_enh_camera_vs(uint32_t creg) {
    if (creg > 256 - 4) rt_fatal("[d3d9] d3d9_enh_camera_vs: bad constant register c%u", creg);
    enh_camvs.on = 1; enh_camvs.creg = creg;
}
// M = view * proj (row vectors, register r = row r) with a rigid view and a centered perspective projection
// (P00 = sx, P11 = sy, P22 = a, P23 = 1, P32 = b): the columns 0, 1 and 3 of the rotation part are sx, sy and 1 times
// the view axes, column 2 is a times column 3 (plus b in row 3). Returns 0 when M is not of this form (a world
// transform with scale, an orthographic or off-center projection, a 4x3 matrix).
static float enh_dot3(const float (*m)[4], int i, int j) { return m[0][i] * m[0][j] + m[1][i] * m[1][j] + m[2][i] * m[2][j]; }
static int enh_camera_split(const float (*m)[4], float view[16], float proj[16]) {
    float sx = sqrtf(enh_dot3(m, 0, 0)), sy = sqrtf(enh_dot3(m, 1, 1)), n3 = sqrtf(enh_dot3(m, 3, 3));
    if (!(sx > 1e-4f && sy > 1e-4f && isfinite(sx) && isfinite(sy)) || !(fabsf(n3 - 1.0f) < 0.01f)) return 0;
    if (fabsf(enh_dot3(m, 0, 1)) > 0.01f * sx * sy || fabsf(enh_dot3(m, 0, 3)) > 0.01f * sx || fabsf(enh_dot3(m, 1, 3)) > 0.01f * sy) return 0;
    float a = enh_dot3(m, 2, 3) / (n3 * n3);
    for (int r = 0; r < 3; r++) if (fabsf(m[r][2] - a * m[r][3]) > 1e-3f * (fabsf(a) + 1.0f)) return 0;
    float b = m[3][2] - a * m[3][3];
    if (!(a > 0.0f && b < 0.0f && isfinite(a) && isfinite(b))) return 0;
    for (int r = 0; r < 4; r++) {
        view[r * 4 + 0] = m[r][0] / sx; view[r * 4 + 1] = m[r][1] / sy; view[r * 4 + 2] = m[r][3]; view[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
    }
    const float P[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, a, 1, 0, 0, b, 0};
    memcpy(proj, P, sizeof P);
    return 1;
}
static int enh_same(const float *x, const float *y, int n) {
    for (int i = 0; i < n; i++) if (fabsf(x[i] - y[i]) > 1e-3f * (1.0f + fabsf(x[i]))) return 0;
    return 1;
}
// counts the camera of one draw; the camera with the most draws in a frame is the scene camera
static void enh_camera_vote(Device *d, uint32_t f, const float view[16], const float proj[16]) {
    if (d->enh_cam_frame != f) { d->enh_cam_frame = f; d->enh_ncam = 0; }
    for (uint32_t i = 0; i < d->enh_ncam; i++)
        if (enh_same(d->enh_cam[i].view, view, 16) && enh_same(d->enh_cam[i].proj, proj, 16)) { d->enh_cam[i].n++; return; }
    uint32_t k = d->enh_ncam;
    if (k == 4) {   // full: the camera with the fewest draws gives its place
        k = 0;
        for (uint32_t i = 1; i < 4; i++) if (d->enh_cam[i].n < d->enh_cam[k].n) k = i;
        if (d->enh_cam[k].n > 1) return;
    } else d->enh_ncam++;
    memcpy(d->enh_cam[k].view, view, 64); memcpy(d->enh_cam[k].proj, proj, 64); d->enh_cam[k].n = 1;
}
static struct { int on; float dir[3]; } enh_ld;
static struct { int on; char dir[64], pos[64], grad[64]; uint32_t stride; } enh_lvs;
void d3d9_enh_light_dir(float x, float y, float z) {
    float n = sqrtf(x * x + y * y + z * z);
    if (!(n > 0.0f) || !isfinite(n)) rt_fatal("[d3d9] d3d9_enh_light_dir: the direction (%g, %g, %g) has no length", x, y, z);
    if (enh_lvs.on) rt_fatal("[d3d9] d3d9_enh_light_dir: d3d9_enh_light_vs already gives the light");
    enh_ld.on = 1; enh_ld.dir[0] = x / n; enh_ld.dir[1] = y / n; enh_ld.dir[2] = z / n;
}
void d3d9_enh_light_vs(const char *dir, const char *pos, const char *grad, uint32_t stride) {
    if (enh_ld.on) rt_fatal("[d3d9] d3d9_enh_light_vs: d3d9_enh_light_dir already gives the light");
    if (!dir || !pos || !grad || strlen(dir) >= sizeof enh_lvs.dir || strlen(pos) >= sizeof enh_lvs.pos ||
        strlen(grad) >= sizeof enh_lvs.grad || stride < 1 || stride > 64)
        rt_fatal("[d3d9] d3d9_enh_light_vs: bad constant names or register stride %u", stride);
    strcpy(enh_lvs.dir, dir); strcpy(enh_lvs.pos, pos); strcpy(enh_lvs.grad, grad);
    enh_lvs.stride = stride; enh_lvs.on = 1;
}
// a vertex shader of d3d9_enh_light_vs: the registers of its light constants (from its constant table)
static void enh_light_shader(Shader *s) {
    uint32_t rd, cd, rp, cp, rg, cg;
    if (!enh_lvs.on || !d3d9_shader_const_reg(s->tok, s->ntok, enh_lvs.dir, &rd, &cd) ||
        !d3d9_shader_const_reg(s->tok, s->ntok, enh_lvs.pos, &rp, &cp) || !d3d9_shader_const_reg(s->tok, s->ntok, enh_lvs.grad, &rg, &cg))
        return;
    uint32_t gn = 2 * enh_lvs.stride + 1;
    if (cd < 1 || cp < 1 || cg < gn || rd >= VS_CONST_F || rp >= VS_CONST_F || rg > VS_CONST_F - gn) {
        rt_log("[d3d9] a vertex shader has the light constants %s c%u, %s c%u, %s c%u (%u registers), outside the constants; its draws give no light",
               enh_lvs.dir, rd, enh_lvs.pos, rp, enh_lvs.grad, rg, cg);
        return;
    }
    s->lv = 1; s->lv_dir = rd; s->lv_pos = rp; s->lv_grad = rg;
}
// a draw with such a shader: its light, once per world position in a frame (at most ENH_LC_MAX)
static void enh_light_note(Device *d, uint32_t f, const State *S) {
    const Shader *vs = S->vs;
    if (d->enh_lc_frame != f) { d->enh_lc_frame = f; d->enh_nlc = 0; }
    const float *P = S->vsf[vs->lv_pos], *L = S->vsf[vs->lv_dir];
    for (uint32_t i = 0; i < d->enh_nlc; i++) if (enh_same(d->enh_lc[i].pos, P, 3)) return;
    if (d->enh_nlc == ENH_LC_MAX) return;
    float n = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (!(n > 1e-6f) || !isfinite(n) || !isfinite(P[0]) || !isfinite(P[1]) || !isfinite(P[2])) return;
    // the trace of the derivative matrix: -2 / r for a point light at distance r
    float tr = S->vsf[vs->lv_grad][0] + S->vsf[vs->lv_grad + enh_lvs.stride][1] + S->vsf[vs->lv_grad + 2 * enh_lvs.stride][2];
    float r = tr < 0.0f ? -2.0f / tr : 0.0f;
    if (!isfinite(r) || r > 1e7f) r = 0.0f;   // a derivative of almost 0: a light at infinity
    typeof(d->enh_lc[0]) *c = &d->enh_lc[d->enh_nlc++];
    for (int i = 0; i < 3; i++) { c->pos[i] = P[i]; c->dir[i] = L[i] / n; }
    c->r = r;
}
// the light of the contact shadows for the frame's camera (view): the light of the draw nearest the camera. The light
// moves 15% of the way to the new light in each frame (a change from a point light to a light at infinity or back
// is immediate); a frame with no such draw keeps the light. Returns 0 while no frame has had such a draw.
#define ENH_LV_STEP 0.15f
static int enh_light_pick(Device *d, uint32_t f, const float view[16]) {
    if (d->enh_lc_frame != f || !d->enh_nlc) return d->enh_lv_ok;
    float cam[3];
    for (int j = 0; j < 3; j++) cam[j] = -(view[12] * view[j * 4] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
    uint32_t best = 0; float bd = INFINITY;
    for (uint32_t i = 0; i < d->enh_nlc; i++) {
        float dx = d->enh_lc[i].pos[0] - cam[0], dy = d->enh_lc[i].pos[1] - cam[1], dz = d->enh_lc[i].pos[2] - cam[2];
        float dd = dx * dx + dy * dy + dz * dz;
        if (dd < bd) { bd = dd; best = i; }
    }
    const typeof(d->enh_lc[0]) *c = &d->enh_lc[best];
    float t[4];
    if (c->r > 0.0f) { for (int i = 0; i < 3; i++) t[i] = c->pos[i] + c->r * c->dir[i]; t[3] = 1.0f; }
    else { for (int i = 0; i < 3; i++) t[i] = c->dir[i]; t[3] = 0.0f; }
    if (!d->enh_lv_ok || t[3] != d->enh_lv[3]) memcpy(d->enh_lv, t, sizeof t);
    else {
        for (int i = 0; i < 3; i++) d->enh_lv[i] += (t[i] - d->enh_lv[i]) * ENH_LV_STEP;
        if (t[3] == 0.0f) {
            float n = sqrtf(d->enh_lv[0] * d->enh_lv[0] + d->enh_lv[1] * d->enh_lv[1] + d->enh_lv[2] * d->enh_lv[2]);
            if (n > 1e-6f) for (int i = 0; i < 3; i++) d->enh_lv[i] /= n;
            else memcpy(d->enh_lv, t, sizeof t);   // opposite directions: the new light
        }
    }
    if (rt_verbose && (!d->enh_lv_ok || d->frame % 600 == 0))
        rt_log("[d3d9] contact shadow light: %s (%.1f %.1f %.1f), from the nearest of %u lit draws (%.1f from the camera, light distance %.1f)",
               t[3] != 0.0f ? "point" : "direction", d->enh_lv[0], d->enh_lv[1], d->enh_lv[2], d->enh_nlc, sqrtf(bd), c->r);
    d->enh_lv_ok = 1;
    return 1;
}
static int enh_is_light_target(const Surf *s) { return enh_lp.w && s && s->w == enh_lp.w && s->h == enh_lp.h && s->tex->fmt == enh_lp.fmt; }
static float det3(float a0, float a1, float a2, float b0, float b1, float b2, float c0, float c1, float c2) {
    return a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) + a2 * (b0 * c1 - b1 * c0);
}
// the light of a projector matrix M (register r = row r, clip = position * M): the point where clip x, y and w are 0
// (the 4D cross product of the columns x, y and w), or a direction when that point is at infinity
static int enh_light_of(const float (*m)[4], float out[4]) {
    float a[4], b[4], c[4], z[3];
    for (int r = 0; r < 4; r++) { a[r] = m[r][0]; b[r] = m[r][1]; c[r] = m[r][3]; }
    for (int r = 0; r < 3; r++) z[r] = m[r][2];
    float L[4] = { det3(a[1], a[2], a[3], b[1], b[2], b[3], c[1], c[2], c[3]),
                  -det3(a[0], a[2], a[3], b[0], b[2], b[3], c[0], c[2], c[3]),
                   det3(a[0], a[1], a[3], b[0], b[1], b[3], c[0], c[1], c[3]),
                  -det3(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2])};
    float n3 = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (!(n3 > 0.0f) || !isfinite(n3) || !isfinite(L[3])) return 0;
    if (fabsf(L[3]) > 1e-7f * n3) {
        for (int i = 0; i < 3; i++) out[i] = L[i] / L[3];
        out[3] = 1.0f;
    } else {
        // a direction: toward the light, against the projector's depth gradient
        float sg = L[0] * z[0] + L[1] * z[1] + L[2] * z[2] > 0.0f ? -1.0f : 1.0f;
        for (int i = 0; i < 3; i++) out[i] = sg * L[i] / n3;
        out[3] = 0.0f;
    }
    return 1;
}
// a draw that passed prepare(): the light pass and the opaque scene of the back buffer are captured
static void enh_note_draw(Device *d) {
    if (!rt_enh) return;
    State *S = &d->st; Surf *r0 = d->rt[0];
    uint32_t f = d->frame + 1;
    if (!r0) return;
    if (!d->ds && enh_is_light_target(r0) && S->vs && d->enh_light_frame != f) {
        if (enh_light_of((const float (*)[4])S->vsf[enh_lp.creg], d->enh_sc.light)) d->enh_light_frame = f;
    }
    if (rt_enh->ao <= 0 && rt_enh->contact <= 0 && rt_enh->bloom <= 0 && rt_enh->tonemap != 1 && rt_enh->hdr != 1 && rt_enh->aa != 1) return;
    if (d->enh_done == f) return;
    if (rt_enh->contact > 0 && r0->swap && S->vs && S->vs->lv) enh_light_note(d, f, S);
    if (r0->swap && d->ds && S->rs[RS_ZENABLE] && S->rs[RS_ZWRITEENABLE] && !S->rs[RS_ALPHABLENDENABLE]) {
        d->enh_scene = f;
        if (enh_camvs.on) {
            float v[16], p[16];
            if (S->vs && enh_camera_split((const float (*)[4])S->vsf[enh_camvs.creg], v, p)) enh_camera_vote(d, f, v, p);
        } else {
            memcpy(d->enh_sc.view, S->xf[2], sizeof d->enh_sc.view);
            memcpy(d->enh_sc.proj, S->xf[3], sizeof d->enh_sc.proj);
        }
        d->enh_color = r0->tex->mtl; d->enh_depth = d->ds->tex->mtl;
    }
}
// HDR output and tone mapping (rt_enh.h) act on the scene and keep the UI at paper white. A draw into the back
// buffer with no depth test and with alpha blending is a UI draw (in a level of the game: the HUD, the text and the
// menus, after the game's full-screen copy of its post-processed scene); the other draws into the back buffer are
// scene draws. At each change from scene draws to UI draws the back buffer is copied into tm_ref, at most TM_COPIES
// times in a frame (later changes in that frame keep the last copy). A frame of the game has one change in a level
// and two in some frames of the pause menu.
#define TM_COPIES 4
static void tm_note(Device *d) {
    if (!rt_enh || (rt_enh->tonemap != 1 && rt_enh->hdr != 1)) return;
    Surf *r0 = d->rt[0];
    if (!r0 || !r0->swap || r0->swap != d->swap) return;
    const uint32_t *rs = d->st.rs;
    uint32_t f = d->frame + 1;
    if ((d->ds && rs[RS_ZENABLE]) || !rs[RS_ALPHABLENDENABLE]) { d->tm_scene = f; d->tm_ui = 0; return; }
    int start = !d->tm_ui && d->tm_scene == f;
    d->tm_ui = 1;
    if (!start) return;
    if (d->tm_ref_frame != f) { d->tm_ref_frame = f; d->tm_copies = 0; }
    if (d->tm_copies >= TM_COPIES) return;
    d->tm_copies++;
    Tex *bt = r0->tex;
    if (d->pend.active) begin_pass_flush_clear(d);
    if (bt->ms > 1) tex_resolve(d, bt);
    id<MTLTexture> src = bt->ms > 1 ? bt->resolve : bt->mtl;
    if (!d->tm_ref || d->tm_ref.width != src.width || d->tm_ref.height != src.height || d->tm_ref.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width
                                                                                      height:src.height mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead;
        d->tm_ref = [d->mtl newTextureWithDescriptor:td];
        if (!d->tm_ref) rt_fatal("[d3d9] out of GPU memory for the scene copy of HDR output (%lux%lu)",
                                 (unsigned long)src.width, (unsigned long)src.height);
    }
    [get_blit(d) copyFromTexture:src toTexture:d->tm_ref];
    bt->last_use = cur_serial(d);
    if (rt_verbose && d->frame % 600 == 0)
        rt_log("[d3d9] HDR output and tone mapping: frame %u, scene copy %u before the UI (%lux%lu)", d->frame, d->tm_copies,
               (unsigned long)src.width, (unsigned long)src.height);
}
// the end of the opaque scene of this frame (a target change, a copy or clear of the back buffer, Present): the
// ambient occlusion, contact shadow and bloom passes run once, on the GPU after the draws recorded so far. With HDR
// output or tone mapping, a half-float back buffer is then copied into tm_pre: the game's post-processing ends with
// fixed-function copies, which clamp the scene to white, and present_enh_fs takes the scene's colors above white
// from this copy.
static void enh_apply(Device *d) {
    uint32_t f = d->frame + 1;
    if (!rt_enh || d->enh_scene != f || d->enh_done == f) return;
    d->enh_done = f;
    if (d->pend.active) begin_pass_flush_clear(d);
    ensure_cmd(d);
    end_encoders(d);
    if (enh_ld.on) {
        memcpy(d->enh_sc.light, enh_ld.dir, sizeof enh_ld.dir); d->enh_sc.light[3] = 0.0f;
        d->enh_sc.have_light = 1;
    } else if (!enh_lvs.on) d->enh_sc.have_light = d->enh_light_frame == f;
    int ao = rt_enh->ao, contact = rt_enh->contact, taa = rt_enh->aa == 1, have_cam = !enh_camvs.on;
    if (enh_camvs.on && (ao > 0 || contact > 0 || taa)) {
        // the camera with the most draws (the level geometry, whose world transform is the identity)
        uint32_t best = 0, n = d->enh_cam_frame == f ? d->enh_ncam : 0;
        for (uint32_t i = 1; i < n; i++) if (d->enh_cam[i].n > d->enh_cam[best].n) best = i;
        if (n) {
            have_cam = 1;
            memcpy(d->enh_sc.view, d->enh_cam[best].view, 64); memcpy(d->enh_sc.proj, d->enh_cam[best].proj, 64);
            if (rt_verbose && !d->enh_cam_logged) {
                const float *P = d->enh_sc.proj;
                rt_log("[d3d9] scene camera from vertex shader constants c%u: %u of the frame's draws, P00 %.4f P11 %.4f near %.3f far %.1f",
                       enh_camvs.creg, d->enh_cam[best].n, P[0], P[5], -P[14] / P[10], P[14] / (1.0f - P[10]));
                d->enh_cam_logged = 1;
            }
        } else {
            // no draw of this frame had a perspective camera (a menu, a loading screen): no scene passes
            ao = contact = 0;
        }
    }
    if (enh_lvs.on) {
        d->enh_sc.have_light = contact > 0 && enh_light_pick(d, f, d->enh_sc.view);
        memcpy(d->enh_sc.light, d->enh_lv, sizeof d->enh_lv);
    }
    d3d9_enh_scene(d->mtl, d->cmd, d->enh_color, d->enh_depth, &d->enh_sc, ao, contact);
    if (taa) {
        // the motion of the camera from the last frame: inverse(this view-projection) x the last view-projection
        float o[2], vp[16], iv[16], rp[16];
        taa_offset(d, f, o);
        int cont = d->taa_frame != 0 && d->taa_frame == f - 1;
        mat_mul(d->enh_sc.view, d->enh_sc.proj, vp);
        int motion = have_cam && cont && d->taa_vp_ok && mat_inv(vp, iv);
        if (motion) mat_mul(iv, d->taa_vp, rp);
        if (d3d9_enh_taa(d->mtl, d->cmd, d->enh_color, d->enh_depth, motion ? rp : NULL, o[0], o[1], !cont)) {
            d->taa_frame = f;
            d->taa_vp_ok = have_cam;
            memcpy(d->taa_vp, vp, sizeof vp);
        }
    }
    d3d9_enh_bloom(d->mtl, d->cmd, d->enh_color, rt_enh->bloom);
    id<MTLTexture> c = d->enh_color;
    if ((rt_enh->tonemap == 1 || rt_enh->hdr == 1) && c.pixelFormat == MTLPixelFormatRGBA16Float && c.sampleCount == 1) {
        if (!d->tm_pre || d->tm_pre.width != c.width || d->tm_pre.height != c.height) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.pixelFormat width:c.width
                                                                                          height:c.height mipmapped:NO];
            td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead;
            d->tm_pre = [d->mtl newTextureWithDescriptor:td];
            if (!d->tm_pre) rt_fatal("[d3d9] out of GPU memory for the scene copy of HDR output (%lux%lu)",
                                     (unsigned long)c.width, (unsigned long)c.height);
        }
        [get_blit(d) copyFromTexture:c toTexture:d->tm_pre];
        d->tm_pre_frame = f;
        if (rt_verbose && d->frame % 600 == 0)
            rt_log("[d3d9] HDR output and tone mapping: frame %u, the scene before post-processing copied (%lux%lu)", d->frame,
                   (unsigned long)c.width, (unsigned long)c.height);
    }
    d->enh_color = d->enh_depth = nil;
}
// The interface in 4:3 (rt_enh.h, d3d9_enh_ui_vs).
static struct { int on; char src[64]; uint32_t creg; } enh_ui;
void d3d9_enh_ui_vs(const char *source, uint32_t creg) {
    if (!source || !source[0] || strlen(source) >= sizeof enh_ui.src || creg >= VS_CONST_F)
        rt_fatal("[d3d9] d3d9_enh_ui_vs: bad source file name or register c%u", creg);
    strcpy(enh_ui.src, source); enh_ui.creg = creg; enh_ui.on = 1;
}
// a vertex shader of d3d9_enh_ui_vs: its debug information (the DBUG comment before the instructions) names the source
// file enh_ui.src, after a path separator or at the start, and followed by a NUL
static void enh_ui_shader(Shader *s) {
    if (!enh_ui.on) return;
    size_t L = strlen(enh_ui.src);
    for (uint32_t i = 1; i < s->ntok && (s->tok[i] & 0xFFFFu) == 0xFFFEu; ) {
        uint32_t len = (s->tok[i] >> 16) & 0x7FFFu;
        if ((uint64_t)i + 1 + len > s->ntok) return;
        if (len >= 1 && s->tok[i + 1] == 0x47554244u) {   // 'DBUG'
            const char *b = (const char *)&s->tok[i + 2];
            size_t n = (size_t)(len - 1) * 4;
            for (size_t j = 0; j + L < n; j++)
                if ((j == 0 || b[j - 1] == '\\' || b[j - 1] == '/') && !memcmp(b + j, enh_ui.src, L) && b[j + L] == 0) {
                    s->ui = 1; s->ui_creg = enh_ui.creg;
                    return;
                }
        }
        i += 1 + len;
    }
}
// The horizontal scale of a draw: 4:3 / the aspect ratio of the back buffer for an interface draw into a back buffer
// wider than 4:3 that does not cover its full width; else 1. An interface draw has a vertex shader of d3d9_enh_ui_vs
// (screen x = position.x * c[creg].x + c[creg].z, 0 to 1) or pretransformed vertices (screen x in pixels). Its width
// comes from the position element of stream 0 of its vertices.
#define UI_ASPECT (4.0 / 3.0)
static float ui_draw_scale(Device *d, uint32_t n, int indexed, uint32_t first, int32_t base, int i32, const uint8_t *icpu,
                           const UpStream *up) {
    State *S = &d->st;
    Surf *r0 = d->rt[0];
    if (!enh_ui.on || !r0 || !r0->swap || r0->swap != d->swap || !r0->w || !r0->h) return 1.0f;
    double aspect = (double)r0->w / (double)r0->h;
    if (aspect <= UI_ASPECT + 1e-6) return 1.0f;
    Shader *vs = S->vs;
    if (vs && !vs->ui) return 1.0f;
    Decl *dc = S->decl;
    if (!dc) return 1.0f;
    int pos = -1;
    for (uint32_t i = 0; i < dc->n; i++)
        if (dc->el[i].stream == 0 && dc->el[i].index == 0 && dc->el[i].usage == (vs ? DU_POSITION : DU_POSITIONT)) { pos = (int)i; break; }
    if (pos < 0) return 1.0f;   // a fixed-function draw without pretransformed vertices: not an interface draw
    int type = dc->el[pos].type;
    if (type != DT_FLOAT2 && type != DT_FLOAT3 && type != DT_FLOAT4 && type != DT_SHORT2 && type != DT_SHORT4) return 1.0f;
    uint32_t eoff = dc->el[pos].offset, esz = (type == DT_SHORT2 || type == DT_SHORT4) ? 2 : 4;
    const uint8_t *vb; uint64_t vlen; uint32_t stride;
    if (up && up->on) { vb = up->cpu; vlen = up->len; stride = up->stride; }
    else {
        VB *b = S->stream[0].vb;
        if (!b || S->stream[0].off > b->len) return 1.0f;
        vb = (const uint8_t *)G2H(b->st.g) + S->stream[0].off; vlen = b->len - S->stream[0].off; stride = S->stream[0].stride;
    }
    if (!stride) return 1.0f;
    double sx = vs ? S->vsf[vs->ui_creg][0] : 1.0 / (double)r0->w, ox = vs ? S->vsf[vs->ui_creg][2] : 0.0;
    double x0 = INFINITY, x1 = -INFINITY;
    for (uint32_t i = 0; i < n; i++) {
        int64_t v;
        if (indexed) v = (int64_t)base + (i32 ? ((const uint32_t *)icpu)[first + i] : ((const uint16_t *)icpu)[first + i]);
        else v = (int64_t)first + i;
        if (v < 0 || (uint64_t)v * stride + eoff + esz > vlen) return 1.0f;   // outside the vertices: drawn as the game gives it
        const uint8_t *q = vb + (size_t)v * stride + eoff;
        double x;
        if (esz == 2) { int16_t t; memcpy(&t, q, 2); x = t; } else { float t; memcpy(&t, q, 4); x = t; }
        x = x * sx + ox;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
    }
    if (!(x0 <= x1) || (x0 <= 0.001 && x1 >= 0.999)) return 1.0f;   // no vertices, or the full width
    float k = (float)(UI_ASPECT / aspect);
    if (!d->ui_logged) {
        rt_log("[d3d9] interface in 4:3: frame %u, the interface draws use %.4f of the width of the %ux%u back buffer, centered",
               d->frame + 1, k, r0->w, r0->h);
        d->ui_logged = 1;
    }
    return k;
}

// indexed: idx/ioff/i32 the index source (CPU pointer for fans), first = start index; else first = start vertex
static uint32_t draw(Device *d, uint32_t pt, uint32_t prims, int indexed, uint32_t first, int32_t base,
                     __unsafe_unretained id<MTLBuffer> ibuf, uint32_t ioff, int i32, const uint8_t *icpu, const UpStream *up) {
    MTLPrimitiveType mt; uint32_t n;
    if (!prim_count(pt, prims, &mt, &n)) return D3DERR_INVALIDCALL;
    if (!prims) return D3D_OK;
    if (prims > 0x555555u) return D3DERR_INVALIDCALL;
    uint32_t inst = 1;
    tm_note(d);
    d->ui_k = ui_draw_scale(d, n, indexed, first, base, i32, icpu, up);
    if (!prepare(d, pt == 1, up, &inst, indexed)) { d->stat_skipped++; return D3D_OK; }
    d->stat_draws++;
    enh_note_draw(d);
    id<MTLRenderCommandEncoder> e = d->enc;
    if (pt == 6) {
        // triangle fan -> triangle list: (0, i + 1, i + 2)
        __unsafe_unretained id<MTLBuffer> fb; uint32_t fo;
        uint32_t *ix = ring_alloc(d, prims * 12, &fb, &fo);
        for (uint32_t i = 0; i < prims; i++) {
            if (indexed) {
                uint32_t a, b, c;
                if (i32) { const uint32_t *s = (const uint32_t *)icpu + first; a = s[0]; b = s[i + 1]; c = s[i + 2]; }
                else { const uint16_t *s = (const uint16_t *)icpu + first; a = s[0]; b = s[i + 1]; c = s[i + 2]; }
                ix[i * 3] = a; ix[i * 3 + 1] = b; ix[i * 3 + 2] = c;
            } else { ix[i * 3] = first; ix[i * 3 + 1] = first + i + 1; ix[i * 3 + 2] = first + i + 2; }
        }
        [e drawIndexedPrimitives:mt indexCount:prims * 3 indexType:MTLIndexTypeUInt32 indexBuffer:fb indexBufferOffset:fo
                   instanceCount:inst baseVertex:indexed ? base : 0 baseInstance:0];
        return D3D_OK;
    }
    if (indexed)
        [e drawIndexedPrimitives:mt indexCount:n indexType:i32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:ibuf
               indexBufferOffset:ioff + first * (i32 ? 4u : 2u) instanceCount:inst baseVertex:base baseInstance:0];
    else [e drawPrimitives:mt vertexStart:first vertexCount:n instanceCount:1];
    return D3D_OK;
}

// ---------------- copies, fills and presentation ----------------
// a render pass of its own on one color target (the current pass ends; the next draw starts a new one)
static id<MTLRenderCommandEncoder> util_pass(Device *d, id<MTLTexture> tex, NSUInteger level, NSUInteger slice, int clear,
                                             MTLClearColor cc) {
    if (d->pend.active) begin_pass_flush_clear(d);
    ensure_cmd(d);
    end_encoders(d);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = tex;
    p.colorAttachments[0].level = level;
    p.colorAttachments[0].slice = slice;
    p.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    p.colorAttachments[0].clearColor = cc;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [d->cmd renderCommandEncoderWithDescriptor:p];
    if (!e) rt_fatal("[d3d9] Metal did not create a render encoder for a copy");
    return e;
}
// draw `src` (rectangle sr of a sw x sh image) into rectangle dr of a dw x dh target
static void util_quad(Device *d, id<MTLRenderCommandEncoder> e, char tag, int blend, MTLPixelFormat dfmt, id<MTLTexture> src,
                      id<MTLSamplerState> smp, const int32_t *sr, uint32_t sw, uint32_t sh, const int32_t *dr, uint32_t dw, uint32_t dh,
                      __unsafe_unretained id<MTLBuffer> fbuf, uint32_t foff, const uint32_t *fbytes) {
    UtilKey k; memset(&k, 0, sizeof k);
    k.tag = tag; k.mask = 1; k.blend = (uint8_t)blend; k.samples = 1; k.fmt[0] = (uint16_t)dfmt;
    [e setRenderPipelineState:util_pso(d, &k)];
    [e setViewport:(MTLViewport){0, 0, dw, dh, 0, 1}];
    float r[12] = {2.0f * dr[0] / dw - 1.0f, 1.0f - 2.0f * dr[1] / dh, 2.0f * dr[2] / dw - 1.0f, 1.0f - 2.0f * dr[3] / dh,
                   (float)sr[0] / sw, (float)sr[1] / sh, (float)sr[2] / sw, (float)sr[3] / sh, 0, 0, 0, 0};
    [e setVertexBytes:r length:sizeof r atIndex:0];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentSamplerState:smp atIndex:0];
    if (fbuf) { [e setFragmentBuffer:fbuf offset:foff atIndex:0]; [e setFragmentBytes:fbytes length:4 atIndex:1]; }
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}
static Surf *surf_of(uint32_t g) {
    static ComClass *const cls[] = {&c_surface};
    return (Surf *)obj_of(g, cls, 1);
}
// a RECT from guest memory clipped to w x h; 0 if empty or outside
static int read_rect(uint32_t p, uint32_t w, uint32_t h, int32_t *r) {
    if (!p) { r[0] = 0; r[1] = 0; r[2] = (int32_t)w; r[3] = (int32_t)h; return 1; }
    memcpy(r, G2H(p), 16);
    return r[0] >= 0 && r[1] >= 0 && r[2] <= (int32_t)w && r[3] <= (int32_t)h && r[0] < r[2] && r[1] < r[3];
}
static uint32_t stretch_rect(Device *d, Surf *s, uint32_t srp, Surf *t, uint32_t drp, uint32_t filter) {
    Tex *st = s->tex, *dt = t->tex;
    if (st->pool != POOL_DEFAULT || dt->pool != POOL_DEFAULT || !st->mtl || !dt->mtl) return D3DERR_INVALIDCALL;
    int32_t sr[4], dr[4];
    if (!read_rect(srp, s->w, s->h, sr) || !read_rect(drp, t->w, t->h, dr)) return D3DERR_INVALIDCALL;
    if (st->f->kind || dt->f->kind) {
        // depth-stencil: whole surfaces of the same size and format
        if (st->fmt != dt->fmt || s->w != t->w || s->h != t->h || srp || drp || st->ms != dt->ms) return D3DERR_INVALIDCALL;
        [get_blit(d) copyFromTexture:st->mtl toTexture:dt->mtl];
        st->last_use = dt->last_use = dt->last_write = cur_serial(d);
        return D3D_OK;
    }
    if (!(dt->mtl.usage & MTLTextureUsageRenderTarget) || (dt->ms > 1 && st->ms <= 1)) return D3DERR_INVALIDCALL;
    if (st->ms > 1) tex_resolve(d, st);
    id<MTLTexture> src = st->ms > 1 ? st->resolve : st->mtl;
    int same = sr[2] - sr[0] == dr[2] - dr[0] && sr[3] - sr[1] == dr[3] - dr[1];
    if (same && st->f->mtl == dt->f->mtl && st->sc == 1 && dt->sc == 1 && dt->ms <= 1 && src != dt->mtl) {
        [get_blit(d) copyFromTexture:src sourceSlice:st->cube ? s->face : 0 sourceLevel:s->level
                        sourceOrigin:MTLOriginMake((NSUInteger)sr[0], (NSUInteger)sr[1], 0)
                          sourceSize:MTLSizeMake((NSUInteger)(sr[2] - sr[0]), (NSUInteger)(sr[3] - sr[1]), 1)
                           toTexture:dt->mtl destinationSlice:dt->cube ? t->face : 0 destinationLevel:t->level
                   destinationOrigin:MTLOriginMake((NSUInteger)dr[0], (NSUInteger)dr[1], 0)];
    } else {
        if (dt->ms > 1) { LOG_ONCE("[d3d9] StretchRect into a multisampled surface is not supported"); return D3DERR_INVALIDCALL; }
        if (src == dt->mtl && s->level == t->level && s->face == t->face) {
            LOG_ONCE("[d3d9] StretchRect within one surface is not supported");
            return D3DERR_INVALIDCALL;
        }
        // the source as a single-level view, so the copy reads the source level
        id<MTLTexture> sv = [tex_view(st, 0) newTextureViewWithPixelFormat:tex_view(st, 0).pixelFormat
                                                               textureType:MTLTextureType2D
                                                                    levels:NSMakeRange(s->level, 1)
                                                                    slices:NSMakeRange(st->cube ? s->face : 0, 1)];
        id<MTLRenderCommandEncoder> e = util_pass(d, dt->mtl, t->level, dt->cube ? t->face : 0, 0, MTLClearColorMake(0, 0, 0, 0));
        int32_t sc = (int32_t)dt->sc, drs[4] = {dr[0] * sc, dr[1] * sc, dr[2] * sc, dr[3] * sc};   // storage pixels
        util_quad(d, e, dt->f->alpha ? 'K' : 'O', 0, dt->f->mtl, sv, filter == 1 ? d->samp_pt : d->samp_lin,
                  sr, s->w, s->h, drs, t->w * dt->sc, t->h * dt->sc, nil, 0, NULL);
        [e endEncoding];
    }
    st->last_use = dt->last_use = dt->last_write = cur_serial(d);
    dt->init[t->face * dt->levels + t->level] = 1;
    if (dt->usage & USAGE_AUTOGENMIPMAP) dt->mips_dirty = 1;
    return D3D_OK;
}
static uint32_t color_fill(Device *d, Surf *s, uint32_t rp, uint32_t color) {
    Tex *t = s->tex;
    if (t->pool != POOL_DEFAULT || !t->mtl || t->f->kind || t->ms > 1 || !(t->mtl.usage & MTLTextureUsageRenderTarget)) return D3DERR_INVALIDCALL;
    int32_t r[4];
    if (!read_rect(rp, s->w, s->h, r)) return D3DERR_INVALIDCALL;
    float c[4]; color4(color, c);
    if (!t->f->alpha) c[3] = 1.0f;
    int full = r[0] == 0 && r[1] == 0 && r[2] == (int32_t)s->w && r[3] == (int32_t)s->h;
    id<MTLRenderCommandEncoder> e = util_pass(d, t->mtl, s->level, t->cube ? s->face : 0, full, MTLClearColorMake(c[0], c[1], c[2], c[3]));
    if (!full) {
        UtilKey k; memset(&k, 0, sizeof k);
        k.tag = 'C'; k.mask = 1; k.samples = 1; k.fmt[0] = (uint16_t)t->f->mtl;
        [e setRenderPipelineState:util_pso(d, &k)];
        NSUInteger sc = t->sc;   // the storage can be larger than the Direct3D size
        [e setViewport:(MTLViewport){0, 0, (double)s->w * sc, (double)s->h * sc, 0, 1}];
        [e setScissorRect:(MTLScissorRect){(NSUInteger)r[0] * sc, (NSUInteger)r[1] * sc, (NSUInteger)(r[2] - r[0]) * sc, (NSUInteger)(r[3] - r[1]) * sc}];
        float q[12] = {-1, 1, 1, -1, 0, 0, 1, 1, 0, 0, 0, 0}, cols[4][4];
        for (int i = 0; i < 4; i++) memcpy(cols[i], c, 16);
        [e setVertexBytes:q length:sizeof q atIndex:0];
        [e setFragmentBytes:cols length:sizeof cols atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [e endEncoding];
    t->last_use = t->last_write = d->serial;
    t->init[s->face * t->levels + s->level] = 1;
    if (t->usage & USAGE_AUTOGENMIPMAP) t->mips_dirty = 1;
    return D3D_OK;
}
// SYSTEMMEM surface -> DEFAULT surface
static uint32_t update_surface(Device *d, Surf *s, uint32_t srp, Surf *t, uint32_t dpp) {
    Tex *st = s->tex, *dt = t->tex;
    if (!st->sys || dt->sys || !dt->mtl || st->fmt != dt->fmt || dt->ms > 1 || dt->f->kind) return D3DERR_INVALIDCALL;
    int32_t sr[4];
    if (!read_rect(srp, s->w, s->h, sr)) return D3DERR_INVALIDCALL;
    int32_t dx = 0, dy = 0;
    if (dpp) { memcpy(&dx, G2H(dpp), 4); memcpy(&dy, G2H(dpp + 4), 4); }
    uint32_t w = (uint32_t)(sr[2] - sr[0]), h = (uint32_t)(sr[3] - sr[1]);
    if (dx < 0 || dy < 0 || (uint64_t)dx + w > t->w || (uint64_t)dy + h > t->h) return D3DERR_INVALIDCALL;
    const Fmt *f = st->f;
    if (f->block && ((sr[0] | sr[1] | dx | dy) & 3)) return D3DERR_INVALIDCALL;
    uint32_t pitch = fmt_pitch(f, s->w);
    uint32_t xb = f->block ? (uint32_t)sr[0] / 4 * f->bpp : (uint32_t)sr[0] * f->bpp, yb = f->block ? (uint32_t)sr[1] / 4 : (uint32_t)sr[1];
    const uint8_t *src = (const uint8_t *)G2H(st->sys[s->face * st->levels + s->level]) + (size_t)yb * pitch + xb;
    // block formats: a partial block at the right or bottom edge of the level is a whole block in the copy
    uint32_t cw = w, ch = h;
    if (f->block) { if ((uint32_t)dx + cw > t->w) cw = t->w - (uint32_t)dx; if ((uint32_t)dy + ch > t->h) ch = t->h - (uint32_t)dy; }
    tex_upload(d, dt, t->face, t->level, (uint32_t)dx, (uint32_t)dy, 0, cw, ch, 1, src, pitch, 0);
    dt->init[t->face * dt->levels + t->level] = 1;
    if ((dt->usage & USAGE_AUTOGENMIPMAP) && t->level == 0) dt->mips_dirty = 1;
    return D3D_OK;
}
// SYSTEMMEM texture -> DEFAULT texture, every level the destination has (matched by size)
static uint32_t update_texture(Device *d, Tex *s, Tex *t) {
    if (!s->sys || t->sys || !t->mtl || s->fmt != t->fmt || s->cube != t->cube || s->volume != t->volume) return D3DERR_INVALIDCALL;
    uint32_t base = 0;
    while (base < s->levels && (mip(s->w, base) != t->w || mip(s->h, base) != t->h || mip(s->d, base) != t->d)) base++;
    if (base == s->levels) return D3DERR_INVALIDCALL;
    uint32_t n = t->levels;
    if (s->levels - base < n) {
        if (!(t->usage & USAGE_AUTOGENMIPMAP)) return D3DERR_INVALIDCALL;
        n = 1;
    }
    for (uint32_t fa = 0; fa < t->faces; fa++) for (uint32_t l = 0; l < n; l++) {
        uint32_t w = mip(t->w, l), h = mip(t->h, l), dd = mip(t->d, l);
        const Fmt *f = s->f;
        uint32_t pitch = fmt_pitch(f, w), slice = pitch * fmt_rows(f, h);
        tex_upload(d, t, fa, l, 0, 0, 0, w, h, dd, G2H(s->sys[fa * s->levels + base + l]), pitch, slice);
        t->init[fa * t->levels + l] = 1;
    }
    if (t->usage & USAGE_AUTOGENMIPMAP) t->mips_dirty = 1;
    return D3D_OK;
}
// DEFAULT render target -> SYSTEMMEM surface of the same size and format
static uint32_t get_rt_data(Device *d, Surf *s, Surf *t) {
    Tex *st = s->tex, *dt = t->tex;
    if (!st->mtl || !dt->sys || st->fmt != dt->fmt || s->w != t->w || s->h != t->h || st->f->kind) return D3DERR_INVALIDCALL;
    uint32_t pitch = fmt_pitch(st->f, s->w);
    if (!st->init[s->face * st->levels + s->level] && !(st->mtl.usage & MTLTextureUsageRenderTarget)) {
        memset(G2H(dt->sys[t->face * dt->levels + t->level]), 0, (size_t)pitch * fmt_rows(st->f, s->h));
        return D3D_OK;
    }
    tex_read(d, st, s->face, s->level, 0, 0, 0, s->w, s->h, 1, G2H(dt->sys[t->face * dt->levels + t->level]), pitch, 0);
    return D3D_OK;
}

// ---- frame pacing ----
// A full-screen swap chain with the presentation interval D3DPRESENT_INTERVAL_DEFAULT or ONE and a refresh rate
// (FullScreen_RefreshRateInHz) presents at that rate, as the display mode of Direct3D 9 does. The game asks for 60 Hz.
// The game's scripts depend on this rate: above 60 frames per second their timing fails (the 60 FPS limit of the GOG
// release fixes these errors; "DCoTE Unofficial Patch" v1.7 notes). On a screen of a higher rate (ProMotion, 120 Hz)
// the layer alone would present at the rate of the screen.
//   Each drawable shows for at least one period (presentDrawable:afterMinimumDuration:).
//   Present returns at the next point of a fixed grid of periods. Thus the frames of the game start at even
//   intervals, and the game's clocks advance by one period per frame. A frame that ends more than one period late
//   starts a new grid (a slow frame, a pause in the background).
//   The grid follows the screen: the layer frees a drawable when the screen shows the next one, once per period.
//   When nextDrawable waits for that (more than 0.5 ms), the grid point of this frame moves to the time of the free
//   drawable + a quarter period (the time for the rest of Present), if that is later. Thus the grid comes after the
//   time at which the screen frees a drawable.
// A windowed swap chain (refresh rate 0) and D3DPRESENT_INTERVAL_IMMEDIATE have no pacing.
static uint64_t pace_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void pace_setup(Swap *sw) {
    uint32_t rate = sw->pp[8] == 0 && sw->pp[13] != 0x80000000u ? sw->pp[12] : 0;
    if (rate != sw->pace_rate) {
        if (rate) rt_log("[d3d9] frame pacing: %u frames per second (the refresh rate of the presentation parameters)", rate);
        else if (sw->pace_rate) rt_log("[d3d9] frame pacing: off");
    }
    sw->pace_rate = rate;
    sw->pace_due = 0;
}
static void pace_drawable(Swap *sw, uint64_t t_ask, uint64_t t_got) {
    if (!sw->pace_rate || t_got - t_ask <= 500000ull) return;
    uint64_t due = t_got + 250000000ull / sw->pace_rate;
    if (due > sw->pace_due) sw->pace_due = due;   // later only: a drawable that is free early does not raise the rate
}
static void pace_wait(Swap *sw) {
    if (!sw->pace_rate) return;
    uint64_t period = 1000000000ull / sw->pace_rate, now = pace_now();
    if (!sw->pace_due || now > sw->pace_due + period) { sw->pace_due = now + period; return; }
    static mach_timebase_info_data_t tb;
    if (!tb.denom && mach_timebase_info(&tb) != KERN_SUCCESS) rt_fatal("[d3d9] mach_timebase_info failed");
    if (now < sw->pace_due) {
        // CLOCK_UPTIME_RAW and mach_absolute_time are the same clock
        uint64_t ticks = mach_absolute_time() + (sw->pace_due - now) * tb.denom / tb.numer;
        kern_return_t kr;
        while ((kr = mach_wait_until(ticks)) == KERN_ABORTED) { }
        if (kr != KERN_SUCCESS) rt_fatal("[d3d9] frame pacing: mach_wait_until failed (%d)", kr);
    }
    sw->pace_due += period;
}

// ---- swap chains ----
static void swap_destroy(Obj *o) {
    Swap *sw = (Swap *)o;
    if (sw->bb) { sw->bb->swap = NULL; priv_unref(&sw->bb->o); }
    sw->layer = nil; sw->front = nil;
    free(sw);
}
// the back buffer of presentation parameters `pp` (already validated)
static uint32_t swap_make_bb(Device *d, Swap *sw) {
    uint32_t err;
    Surf *bb = surf_new(d, sw->pp[0], sw->pp[1], sw->pp[2], USAGE_RENDERTARGET, POOL_DEFAULT, sw->pp[4] == 1 ? d->caps_ms_counts[sw->pp[5]] : sw->pp[4],
                        sw->pp[5], (sw->pp[11] & 1) != 0, 1, &err);
    if (!bb) return err;
    bb->o.com.refs = 0; bb->o.priv = 1;   // owned by the swap chain
    bb->swap = sw;
    if (sw->bb) { sw->bb->swap = NULL; priv_unref(&sw->bb->o); }
    sw->bb = bb;
    sw->front = nil;
    return D3D_OK;
}
static uint32_t gamma_identity(const uint16_t *r) {
    for (int i = 0; i < 256; i++) for (int c = 0; c < 3; c++) if (r[c * 256 + i] != (uint16_t)(i * 257)) return 0;
    return 1;
}
static void gamma_reset(Swap *sw) { for (int i = 0; i < 256; i++) for (int c = 0; c < 3; c++) sw->ramp[c * 256 + i] = (uint16_t)(i * 257); }

// The constants of present_enh_fs. GT7 tone mapping: a port of the reference implementation gt7_tone_mapping.cpp
// v1.0 of Polyphony Digital ("Driving Toward Reality: Physically Based Tone Mapping and Perceptual Fidelity in Gran
// Turismo 7", SIGGRAPH 2025 course "Physically Based Shading in Theory and Practice"):
//
//   MIT License
//
//   Copyright (c) 2025 Polyphony Digital Inc.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without
//   limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the following
//   conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//   TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// The curve parameters are those of the reference (alpha 0.25, mid point 0.538, linear section 0.444, toe strength
// 1.280, blend ratio 0.6, chroma fade 0.98 .. 1.16), in the units of the reference: frame-buffer units, 1.0 = 100 nits.
//   SDR output (initializeAsSDR): the target is 2.5 (the SDR paper white of 250 nits of the reference); the game's
//   white is at that paper white (expo 2.5), and the output is divided by 2.5 (oscale 0.4). Thus the colors between
//   the mid point and the linear section (linear 0.215 .. 0.444) do not change.
//   HDR output (initializeAsHDR): the target is the calibrated peak in nits / 100. The game's white is at the
//   calibrated paper white in nits (expo = paper white nits / 100), and the output in nits is converted to EDR units
//   (oscale = 100 / the nits of SDR white). SDR white is taken as 100 nits x the potential EDR headroom / the current
//   EDR headroom (rt_enh.h, RT_ENH_HDR_PEAK_NITS).
// HDR output without tone mapping uses the units of EDR (1.0 = SDR white).
typedef struct { uint32_t mode; float paper, target, target_i, kA, kB, kC, mid, lin, toe, blend, fade0, fade1, rel, w, h;
                 float gt, expo, oscale, pad; } PresentTM;
static float pq_enc(float n) {
    float ym = powf(n > 0 ? n / 10000.0f : 0.0f, 0.1593017578125f);
    return powf((0.8359375f + 18.8515625f * ym) / (1.0f + 18.6875f * ym), 78.84375f);
}
static void present_tm_consts(PresentTM *k, uint32_t mode, float paper, float peak, float gt, float expo, float oscale,
                              uint32_t w, uint32_t h) {
    memset(k, 0, sizeof *k);
    k->mode = mode;
    k->paper = paper;
    k->target = peak;
    k->rel = peak > paper ? peak / paper : 1.0f;
    k->w = (float)w; k->h = (float)h;
    k->gt = gt; k->expo = expo; k->oscale = oscale;
    const float alpha = 0.25f, mid = 0.538f, lin = 0.444f, toe = 1.280f;
    float kk = (lin - 1.0f) / (alpha - 1.0f);
    k->kA = gt * lin + gt * kk;
    k->kB = -gt * kk * expf(lin / kk);
    k->kC = -1.0f / (kk * gt);
    k->mid = mid; k->lin = lin; k->toe = toe;
    k->blend = 0.6f; k->fade0 = 0.98f; k->fade1 = 1.16f;
    // ICtCp intensity of a gray of the target luminance (the LMS rows of the reference sum to 1)
    float p = pq_enc(gt * 100.0f);
    k->target_i = (2048.0f * p + 2048.0f * p) / 4096.0f;
}

static uint32_t swap_present(Device *d, Swap *sw, uint32_t srp, uint32_t drp, uint32_t override) {
    Surf *bb = sw->bb;
    Tex *bt = bb->tex;
    enh_apply(d);
    if (d->pend.active) begin_pass_flush_clear(d);
    if (bt->ms > 1) tex_resolve(d, bt);
    ensure_cmd(d);
    end_encoders(d);
    uint32_t hwnd = override ? override : sw->hwnd;
    CAMetalLayer *layer = override ? (__bridge CAMetalLayer *)rt_window_layer(override) : sw->layer;
    if (!layer) { LOG_ONCE("[d3d9] Present: window 0x%x has no Metal layer", hwnd); return D3DERR_INVALIDCALL; }
    int32_t sr[4];
    if (!read_rect(srp, bb->w, bb->h, sr)) return D3DERR_INVALIDCALL;
    id<MTLTexture> img = bt->ms > 1 ? bt->resolve : bt->mtl;
    uint64_t t_ask = pace_now();
    id<CAMetalDrawable> dr = [layer nextDrawable];
    if (!override) pace_drawable(sw, t_ask, pace_now());
    if (!dr) {
        rt_log("[d3d9] Present: the window gave no drawable; the frame is not shown");
        commit(d);
        return D3D_OK;
    }
    id<MTLTexture> dt = dr.texture;
    uint32_t DW = (uint32_t)dt.width, DH = (uint32_t)dt.height;
    // the destination rectangle is in client coordinates; the drawable covers the client area
    int cw = 0, ch = 0;
    rt_window_size(hwnd, &cw, &ch);
    if (cw <= 0 || ch <= 0) { cw = (int)DW; ch = (int)DH; }
    int32_t drc[4] = {0, 0, (int32_t)DW, (int32_t)DH};
    if (drp) {
        int32_t q[4]; memcpy(q, G2H(drp), 16);
        drc[0] = (int32_t)((int64_t)q[0] * DW / cw); drc[1] = (int32_t)((int64_t)q[1] * DH / ch);
        drc[2] = (int32_t)((int64_t)q[2] * DW / cw); drc[3] = (int32_t)((int64_t)q[3] * DH / ch);
    }
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = dt;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [d->cmd renderCommandEncoderWithDescriptor:p];
    if (!e) rt_fatal("[d3d9] Metal did not create the presentation encoder");
    int scaled = (sr[2] - sr[0]) != drc[2] - drc[0] || (sr[3] - sr[1]) != drc[3] - drc[1];
    // the gamma ramp applies in full-screen mode (D3D9 ignores it for windowed swap chains)
    uint32_t gamma = !sw->pp[8] && !gamma_identity(sw->ramp);
    // reduced banding (rt_enh.h): a back buffer of 16 bits per channel is dithered into an 8-bit or a 10-bit drawable
    if ((bt->f->mtl == MTLPixelFormatRGBA16Unorm || bt->f->mtl == MTLPixelFormatRGBA16Float) &&
        (dt.pixelFormat == MTLPixelFormatBGRA8Unorm || dt.pixelFormat == MTLPixelFormatBGR10A2Unorm))
        gamma |= dt.pixelFormat == MTLPixelFormatBGR10A2Unorm ? 6u : 2u;
    __unsafe_unretained id<MTLBuffer> rb; uint32_t ro;
    float *ramp = ring_alloc(d, 256 * 16, &rb, &ro);
    for (int i = 0; i < 256; i++) for (int c = 0; c < 3; c++) ramp[i * 4 + c] = sw->ramp[c * 256 + i] / 65535.0f;
    for (int i = 0; i < 256; i++) ramp[i * 4 + 3] = 1.0f;
    // enhancement: HDR output and GT7 tone mapping (an EDR drawable always takes this path, so that the frames
    // presented while the layer changes back to SDR are encoded for the layer they go to)
    int edr_dt = dt.pixelFormat == MTLPixelFormatRGBA16Float;
    uint32_t tm_mode = (rt_enh && rt_enh->tonemap == 1 ? 1u : 0u) | (edr_dt ? 2u : 0u) | (rt_enh && rt_enh->hdr == 1 ? 8u : 0u);
    if (tm_mode) {
        __unsafe_unretained id<MTLBuffer> tb; uint32_t to;
        PresentTM *k = ring_alloc(d, sizeof(PresentTM), &tb, &to);
        float paper = 1.0f, peak = 1.0f, gt = 2.5f, expo = 2.5f, oscale = 0.4f;   // GT7: SDR output
        if (edr_dt && (tm_mode & 8)) {
            static const int peaks[RT_ENH_HDR_PEAKS] = RT_ENH_HDR_PEAK_NITS, papers[RT_ENH_HDR_PAPERS] = RT_ENH_HDR_PAPER_NITS;
            if (rt_enh->hdr_peak < 0 || rt_enh->hdr_peak >= RT_ENH_HDR_PEAKS || rt_enh->hdr_paper < 0 || rt_enh->hdr_paper >= RT_ENH_HDR_PAPERS)
                rt_fatal("[d3d9] HDR calibration out of range: peak %d, paper white %d", rt_enh->hdr_peak, rt_enh->hdr_paper);
            float headroom = rt_cocoa_edr_headroom(), potential = rt_cocoa_edr_potential();
            if (!(headroom >= 1.0f) || !(potential >= headroom))
                rt_fatal("[d3d9] HDR output: the screen gave the EDR headroom %.3f and the potential EDR headroom %.3f", headroom, potential);
            float white = 100.0f * potential / headroom;  // the nits of SDR white (rt_enh.h)
            paper = (float)papers[rt_enh->hdr_paper] / white;
            peak = (float)peaks[rt_enh->hdr_peak] / white;
            gt = (float)peaks[rt_enh->hdr_peak] / 100.0f; expo = (float)papers[rt_enh->hdr_paper] / 100.0f; oscale = 100.0f / white;   // GT7: HDR output
            if (headroom != d->tm_logged.headroom || potential != d->tm_logged.potential ||
                peaks[rt_enh->hdr_peak] != d->tm_logged.peak || papers[rt_enh->hdr_paper] != d->tm_logged.paper) {
                rt_log("[d3d9] HDR output: peak %d nits (%.2f x SDR white), paper white %d nits (%.2f x SDR white)%s; the screen: EDR "
                       "headroom %.2f of %.2f, SDR white approximately %.0f nits, peak approximately %.0f nits",
                       peaks[rt_enh->hdr_peak], peak, papers[rt_enh->hdr_paper], paper, (tm_mode & 1) ? ", GT7 tone mapping" : "",
                       headroom, potential, white, white * headroom);
                d->tm_logged.headroom = headroom; d->tm_logged.potential = potential;
                d->tm_logged.peak = peaks[rt_enh->hdr_peak]; d->tm_logged.paper = papers[rt_enh->hdr_paper];
            }
            if (rt_enh->hdr_test) tm_mode |= 32u;
        }
        // the scene of this frame and its copy before the UI (tm_note); the back buffer itself when the last draw was
        // not a UI draw
        uint32_t f = d->frame + 1;
        int ref = sw == d->swap && d->tm_ui && d->tm_ref_frame == f && d->tm_ref && d->tm_ref.width == img.width && d->tm_ref.height == img.height;
        if (sw == d->swap && d->tm_scene == f) tm_mode |= 16u;
        // the scene of this frame before the game's post-processing (enh_apply)
        int pre = sw == d->swap && d->tm_pre_frame == f && d->tm_pre && d->tm_pre.width == img.width && d->tm_pre.height == img.height;
        if (pre) tm_mode |= 64u;
        present_tm_consts(k, tm_mode, paper, peak, gt, expo, oscale, DW, DH);
        [e setFragmentBuffer:tb offset:to atIndex:2];
        [e setFragmentTexture:ref ? d->tm_ref : img atIndex:1];
        [e setFragmentTexture:pre ? d->tm_pre : img atIndex:2];
    }
    util_quad(d, e, tm_mode ? 'H' : 'P', 0, dt.pixelFormat, img, scaled ? d->samp_lin : d->samp_pt, sr, bb->w, bb->h, drc, DW, DH, rb, ro, &gamma);
    // the D3D9 cursor (SetCursorProperties / ShowCursor) follows the mouse
    if (d->cursor && d->cursor_show && sw == d->swap) {
        if (rt_mouse_x != d->last_mx || rt_mouse_y != d->last_my) {
            d->last_mx = rt_mouse_x; d->last_my = rt_mouse_y;
            d->cur_x = (int)((int64_t)rt_mouse_x * bb->w / (DW ? DW : 1)); d->cur_y = (int)((int64_t)rt_mouse_y * bb->h / (DH ? DH : 1));
        }
        int32_t cs[4] = {0, 0, d->cur_w, d->cur_h};
        int64_t x0 = d->cur_x - d->hot_x, y0 = d->cur_y - d->hot_y;
        int32_t cd[4] = {(int32_t)(drc[0] + (x0 - sr[0]) * (drc[2] - drc[0]) / (sr[2] - sr[0])),
                         (int32_t)(drc[1] + (y0 - sr[1]) * (drc[3] - drc[1]) / (sr[3] - sr[1])), 0, 0};
        cd[2] = cd[0] + (int32_t)((int64_t)d->cur_w * (drc[2] - drc[0]) / (sr[2] - sr[0]));
        cd[3] = cd[1] + (int32_t)((int64_t)d->cur_h * (drc[3] - drc[1]) / (sr[3] - sr[1]));
        util_quad(d, e, 'K', 1, dt.pixelFormat, d->cursor, d->samp_pt, cs, (uint32_t)d->cur_w, (uint32_t)d->cur_h, cd, DW, DH, nil, 0, NULL);
    }
    [e endEncoding];
    if (!override && sw->pace_rate) [d->cmd presentDrawable:dr afterMinimumDuration:1.0 / (double)sw->pace_rate];
    else [d->cmd presentDrawable:dr];
    if (!override) {
        int want = rt_enh && rt_enh->hdr == 1;
        if (want != sw->edr) { rt_cocoa_layer_edr((__bridge void *)layer, want, 0); sw->edr = want; }
        else if (want && d->frame % 60 == 0) rt_cocoa_layer_edr((__bridge void *)layer, 1, 1);
    }
    // the front buffer (GetFrontBufferData) is the presented image
    if (!sw->front) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:img.pixelFormat width:img.width height:img.height mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead;
        sw->front = [d->mtl newTextureWithDescriptor:td];
        if (!sw->front) rt_fatal("[d3d9] out of GPU memory for the front buffer");
    }
    [get_blit(d) copyFromTexture:img toTexture:sw->front];
    bt->last_use = d->serial;
    uint64_t s = d->serial;
    commit(d);
    // at most three frames in flight
    d->frame_serial[d->frame % 3] = s;
    d->frame++;
    if (rt_verbose && (d->frame == 1 || d->frame % 600 == 0)) {
        rt_log("[d3d9] frame %u presented: back buffer %ux%u -> drawable %ux%u, %llu draws, %llu not drawn since the last report",
               d->frame, bb->w, bb->h, DW, DH, (unsigned long long)d->stat_draws, (unsigned long long)d->stat_skipped);
        d->stat_draws = d->stat_skipped = 0;
    }
    uint64_t old = d->frame_serial[d->frame % 3];
    if (old) wait_serial(d, old);
    if (!override) pace_wait(sw);
    return D3D_OK;
}
// validate presentation parameters (defaults are written back, as Direct3D 9 does)
static uint32_t pp_check(Device *d, uint32_t gpp, uint32_t focus, uint32_t *pp) {
    for (int i = 0; i < 14; i++) pp[i] = rd32(gpp + 4u * (uint32_t)i);
    uint32_t hwnd = pp[7] ? pp[7] : focus;
    if (!hwnd || !rt_window_layer(hwnd)) { rt_log("[d3d9] presentation window 0x%x is not a window of this process", hwnd); return D3DERR_INVALIDCALL; }
    int windowed = pp[8] != 0;
    if (windowed && (!pp[0] || !pp[1])) {
        int w = 0, h = 0; rt_window_size(hwnd, &w, &h);
        if (!pp[0]) pp[0] = w > 0 ? (uint32_t)w : 640;
        if (!pp[1]) pp[1] = h > 0 ? (uint32_t)h : 480;
    }
    if (windowed && pp[2] == FMT_UNKNOWN) pp[2] = FMT_X8R8G8B8;
    const Fmt *f = fmt_find(pp[2]);
    if (!pp[0] || !pp[1] || !f || !f->rt || f->kind || (pp[2] != FMT_X8R8G8B8 && pp[2] != FMT_A8R8G8B8 && pp[2] != FMT_R5G6B5)) {
        rt_log("[d3d9] back buffer %ux%u format %u is not supported", pp[0], pp[1], pp[2]);
        return D3DERR_INVALIDCALL;
    }
    if (!pp[3]) pp[3] = 1;
    if (pp[3] > 3 || pp[6] < 1 || pp[6] > 3 || (pp[6] == 3 && pp[3] != 1)) return D3DERR_INVALIDCALL;
    if (pp[4] == 1) { if (pp[5] >= d->n_ms) return D3DERR_INVALIDCALL; }
    else if (pp[4] >= 2) {
        int ok = 0;
        for (uint32_t i = 0; i < d->n_ms; i++) if (d->caps_ms_counts[i] == pp[4]) ok = 1;
        if (!ok || pp[5] != 0 || pp[6] != 1) return D3DERR_INVALIDCALL;
    }
    if (pp[9]) {
        const Fmt *df = fmt_find(pp[10]);
        if (!df || !df->kind) { rt_log("[d3d9] automatic depth-stencil format %u is not supported", pp[10]); return D3DERR_INVALIDCALL; }
    }
    if (pp[13] != 0 && pp[13] != 1 && pp[13] != 0x80000000u) return D3DERR_INVALIDCALL;
    for (int i = 0; i < 14; i++) wr32(gpp + 4u * (uint32_t)i, pp[i]);
    return D3D_OK;
}
static void layer_setup(Swap *sw) {
    CAMetalLayer *l = sw->layer;
    BOOL sync = sw->pp[13] != 0x80000000u;
    uint32_t hwnd = sw->hwnd, w = sw->pp[0], h = sw->pp[1];
    int windowed = sw->pp[8] != 0;
    dispatch_async(dispatch_get_main_queue(), ^{ l.displaySyncEnabled = sync; });
    pace_setup(sw);
    // full screen: the client area becomes the display mode (the window is sized to the back buffer) and the window
    // goes to macOS full screen; windowed: the window leaves macOS full screen
    if (!windowed) rt_cocoa_set_size(hwnd, (int)w, (int)h);
    rt_cocoa_full_screen(hwnd, !windowed);
}
static Swap *swap_new(Device *d, const uint32_t *pp, uint32_t focus, uint32_t *err) {
    Swap *sw = calloc(1, sizeof *sw);
    if (!sw) { *err = E_OUTOFMEMORY_; return NULL; }
    memcpy(sw->pp, pp, sizeof sw->pp);
    sw->hwnd = pp[7] ? pp[7] : focus;
    sw->layer = (__bridge CAMetalLayer *)rt_window_layer(sw->hwnd);
    sw->edr = -1;                  // the first Present sets the layer's output mode
    gamma_reset(sw);
    obj_wrap(&sw->o, &c_swap, d, swap_destroy);
    *err = swap_make_bb(d, sw);
    if (*err) { sw->o.com.refs = 0; sw->o.priv = 1; priv_unref(&sw->o); return NULL; }
    layer_setup(sw);
    return sw;
}

// ---------------- adapter ----------------
typedef struct { Obj o; } D3D9;
static void d3d9_destroy(Obj *o) { free(o); }

static uint32_t ms_counts(id<MTLDevice> dev, uint32_t *out) {
    static const uint32_t cand[4] = {2, 4, 8, 16};
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) if ([dev supportsTextureSampleCount:cand[i]]) out[n++] = cand[i];
    return n;
}
// display modes: common sizes that fit the screen, and the screen itself (X8R8G8B8, 60 Hz)
#define MAX_MODES 32
static uint32_t mode_list(uint32_t (*m)[2]) {
    static const uint16_t std[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 768}, {1280, 800},
                                      {1280, 960}, {1280, 1024}, {1360, 768}, {1366, 768}, {1440, 900}, {1600, 900}, {1600, 1200},
                                      {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600}};
    int sw = 0, sh = 0;
    rt_cocoa_screen_size(&sw, &sh);
    uint32_t n = 0;
    int native = 0;
    for (size_t i = 0; i < sizeof std / sizeof std[0] && n < MAX_MODES - 1; i++) {
        if (std[i][0] > sw || std[i][1] > sh) continue;
        if (std[i][0] == sw && std[i][1] == sh) native = 1;
        m[n][0] = std[i][0]; m[n][1] = std[i][1]; n++;
    }
    if (!native && sw > 0 && sh > 0) { m[n][0] = (uint32_t)sw; m[n][1] = (uint32_t)sh; n++; }
    return n;
}
static void fill_caps(uint32_t p, uint32_t type) {
    uint8_t c[304]; memset(c, 0, sizeof c);
#define CU(off, v) do { uint32_t v_ = (v); memcpy(c + (off), &v_, 4); } while (0)
#define CF(off, v) do { float v_ = (v); memcpy(c + (off), &v_, 4); } while (0)
    CU(0, type); CU(4, 0); CU(8, 0x20000); CU(12, 0x60120000); CU(16, 0x3A0); CU(20, 0x80000001u); CU(24, 3);
    CU(28, 0x1BBEF0); CU(32, 0x1FCFF2); CU(36, 0xF732191); CU(40, 0xFF); CU(44, 0x3FFF); CU(48, 0x3FFF); CU(52, 0xFF);
    CU(56, 0x84208); CU(60, 0x1EC45); CU(64, 0x07030700); CU(68, 0x07030700); CU(72, 0x03030300); CU(76, 0x3F); CU(80, 0x3F);
    CU(84, 0x1F); CU(88, 8192); CU(92, 8192); CU(96, 2048); CU(100, 8192); CU(104, 8192); CU(108, 16); CF(112, 1e10f);
    CF(116, -32768.0f); CF(120, -32768.0f); CF(124, 32768.0f); CF(128, 32768.0f); CF(132, 0.0f); CU(136, 0x1FF);
    CU(140, 0x100008); CU(144, 0x3FFFFFF); CU(148, 8); CU(152, 8); CU(156, 0x13B); CU(160, 8); CU(164, 0); CU(168, 4);
    CU(172, 0); CF(176, 256.0f); CU(180, 0x555555); CU(184, 0xFFFFFF); CU(188, 16); CU(192, 508); CU(196, 0xFFFE0300u);
    CU(200, 256); CU(204, 0xFFFF0300u); CF(208, 8.0f); CU(212, 0x51); CF(216, 0.0f); CU(224, 0); CU(228, 0); CU(232, 1);
    CU(236, 0x3BF); CU(240, 4); CU(244, 0x03000300);
    CU(248, 1); CU(252, 24); CU(256, 32); CU(260, 4);                     // D3DVSHADERCAPS2_0
    CU(264, 0x1F); CU(268, 24); CU(272, 32); CU(276, 4); CU(280, 512);    // D3DPSHADERCAPS2_0
    CU(284, 0x03000300); CU(288, 65535); CU(292, 65535); CU(296, 32768); CU(300, 32768);
#undef CU
#undef CF
    memcpy(G2H(p), c, sizeof c);
}
static int fmt_is_display(uint32_t f) { return f == FMT_X8R8G8B8; }
static int fmt_is_bb(uint32_t f) { return f == FMT_X8R8G8B8 || f == FMT_A8R8G8B8 || f == FMT_R5G6B5; }

static uint32_t check_format(uint32_t usage, uint32_t rtype, uint32_t fmt) {
    if (rtype == RTYPE_VERTEXBUFFER) return fmt == FMT_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (rtype == RTYPE_INDEXBUFFER) return (fmt == FMT_INDEX16 || fmt == FMT_INDEX32) ? D3D_OK : D3DERR_NOTAVAILABLE;
    const Fmt *f = fmt_find(fmt);
    if (!f) return D3DERR_NOTAVAILABLE;
    if (rtype != RTYPE_SURFACE && rtype != RTYPE_TEXTURE && rtype != RTYPE_CUBETEXTURE && rtype != RTYPE_VOLUMETEXTURE && rtype != RTYPE_VOLUME)
        return D3DERR_NOTAVAILABLE;
    if (usage & USAGE_DEPTHSTENCIL) return (f->kind && rtype == RTYPE_SURFACE) ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (f->kind) return D3DERR_NOTAVAILABLE;
    if ((rtype == RTYPE_VOLUMETEXTURE || rtype == RTYPE_VOLUME) && f->block) return D3DERR_NOTAVAILABLE;
    if ((usage & USAGE_RENDERTARGET) && !f->rt) return D3DERR_NOTAVAILABLE;
    if (usage & USAGE_DMAP) return D3DERR_NOTAVAILABLE;
    id<MTLDevice> dev = rt_metal_device();
    int filt = f->f32 ? dev.supports32BitFloatFiltering : 1;
    if ((usage & USAGE_QUERY_FILTER) && !filt) return D3DERR_NOTAVAILABLE;
    if ((usage & (USAGE_QUERY_SRGBREAD | USAGE_QUERY_SRGBWRITE)) && f->srgb == MTLPixelFormatInvalid) return D3DERR_NOTAVAILABLE;
    if ((usage & USAGE_QUERY_SRGBWRITE) && !f->rt) return D3DERR_NOTAVAILABLE;
    if ((usage & USAGE_QUERY_POSTPIXELSHADER_BLENDING) && !f->blend) return D3DERR_NOTAVAILABLE;
    if ((usage & USAGE_QUERY_LEGACYBUMPMAP) && !f->bump) return D3DERR_NOTAVAILABLE;
    if ((usage & USAGE_AUTOGENMIPMAP) && (!f->rt || !filt || rtype == RTYPE_VOLUMETEXTURE)) return D3DOK_NOAUTOGEN;
    return D3D_OK;
}

static uint32_t qi(CPU *c, ComObj *self, const ComMethod *m, const Guid *const *ids, int n) {
    uint32_t out = M(2);
    if (!out) return E_POINTER_;
    const void *g = G2H(M(1));
    for (int i = 0; i < n; i++) if (guid_eq(g, ids[i])) { obj_addref((Obj *)self); wr32(out, self->guest); return D3D_OK; }
    wr32(out, 0);
    return E_NOINTERFACE_;
}
METHOD(m_AddRef) { return obj_addref((Obj *)self); }
METHOD(m_Release) { return obj_release((Obj *)self); }
#define QI_METHOD(name, ...) METHOD(name) { static const Guid *const ids_[] = {&IID_Unknown, __VA_ARGS__}; \
    return qi(c, self, m, ids_, (int)(sizeof ids_ / sizeof ids_[0])); }
QI_METHOD(qi_d3d9, &IID_D3D9)
QI_METHOD(qi_device, &IID_Device)
QI_METHOD(qi_texture, &IID_Resource, &IID_BaseTex, &IID_Tex)
QI_METHOD(qi_cube, &IID_Resource, &IID_BaseTex, &IID_Cube)
QI_METHOD(qi_voltex, &IID_Resource, &IID_BaseTex, &IID_VolTex)
QI_METHOD(qi_volume, &IID_Volume)
QI_METHOD(qi_surface, &IID_Resource, &IID_Surface)
QI_METHOD(qi_vb, &IID_Resource, &IID_VB)
QI_METHOD(qi_ib, &IID_Resource, &IID_IB)
QI_METHOD(qi_decl, &IID_Decl)
QI_METHOD(qi_vs, &IID_VS)
QI_METHOD(qi_ps, &IID_PS)
QI_METHOD(qi_query, &IID_Query)
QI_METHOD(qi_sb, &IID_SB)
QI_METHOD(qi_swap, &IID_Swap)

METHOD(d3d_RegisterSoftwareDevice) { return D3DERR_NOTAVAILABLE; }
METHOD(d3d_GetAdapterCount) { return 1; }
METHOD(d3d_GetAdapterIdentifier) {
    uint32_t a = M(1), p = M(3);
    if (a != 0 || !p) return D3DERR_INVALIDCALL;
    uint8_t *o = G2H(p);
    memset(o, 0, 1100);
    id<MTLDevice> dev = rt_metal_device();
    snprintf((char *)o, 512, "%s", "Metal");
    snprintf((char *)o + 512, 512, "%s", dev ? dev.name.UTF8String : "Metal");
    snprintf((char *)o + 1024, 32, "%s", "\\\\.\\DISPLAY1");
    uint32_t ver[2] = {0x00000001u, 0x00060000u};   // DriverVersion (LARGE_INTEGER): 6.0.0.1
    memcpy(o + 1056, ver, 8);
    uint32_t vendor = 0x106B, devid = 0, z = 0;
    memcpy(o + 1064, &vendor, 4); memcpy(o + 1068, &devid, 4); memcpy(o + 1072, &z, 4); memcpy(o + 1076, &z, 4);
    uint64_t reg = dev ? dev.registryID : 0;
    memcpy(o + 1080, &reg, 8); memcpy(o + 1088, "D3D9MTL", 8);
    return D3D_OK;
}
METHOD(d3d_GetAdapterModeCount) {
    if (M(1) != 0 || !fmt_is_display(M(2))) return 0;
    uint32_t m_[MAX_MODES][2];
    return mode_list(m_);
}
static void write_mode(uint32_t p, uint32_t w, uint32_t h) { wr32(p, w); wr32(p + 4, h); wr32(p + 8, 60); wr32(p + 12, FMT_X8R8G8B8); }
METHOD(d3d_EnumAdapterModes) {
    uint32_t a = M(1), fmt = M(2), i = M(3), p = M(4);
    if (a != 0 || !p) return D3DERR_INVALIDCALL;
    if (!fmt_is_display(fmt)) return D3DERR_NOTAVAILABLE;
    uint32_t ml[MAX_MODES][2], n = mode_list(ml);
    if (i >= n) return D3DERR_INVALIDCALL;
    write_mode(p, ml[i][0], ml[i][1]);
    return D3D_OK;
}
METHOD(d3d_GetAdapterDisplayMode) {
    if (M(1) != 0 || !M(2)) return D3DERR_INVALIDCALL;
    int w = 0, h = 0; rt_cocoa_screen_size(&w, &h);
    write_mode(M(2), (uint32_t)w, (uint32_t)h);
    return D3D_OK;
}
METHOD(d3d_CheckDeviceType) {
    uint32_t a = M(1), type = M(2), dfmt = M(3), bfmt = M(4), windowed = M(5);
    if (a != 0) return D3DERR_INVALIDCALL;
    if (type != 1) return D3DERR_NOTAVAILABLE;
    if (!fmt_is_display(dfmt) && !(windowed && dfmt == FMT_UNKNOWN)) return D3DERR_NOTAVAILABLE;
    if (bfmt == FMT_UNKNOWN ? !windowed : !fmt_is_bb(bfmt)) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}
METHOD(d3d_CheckDeviceFormat) {
    uint32_t a = M(1), type = M(2), dfmt = M(3), usage = M(4), rtype = M(5), fmt = M(6);
    if (a != 0) return D3DERR_INVALIDCALL;
    if (type != 1 || !fmt_is_display(dfmt)) return D3DERR_NOTAVAILABLE;
    return check_format(usage, rtype, fmt);
}
METHOD(d3d_CheckDeviceMultiSampleType) {
    uint32_t a = M(1), type = M(2), fmt = M(3), mst = M(5), pq = M(6);
    if (a != 0) return D3DERR_INVALIDCALL;
    const Fmt *f = fmt_find(fmt);
    if (type != 1 || !f || (!f->rt && !f->kind) || mst > 16) return D3DERR_NOTAVAILABLE;
    uint32_t counts[4], n = ms_counts(rt_metal_device(), counts), q = 1;
    if (mst == 1) q = n;
    else if (mst >= 2) {
        int ok = 0;
        for (uint32_t i = 0; i < n; i++) if (counts[i] == mst) ok = 1;
        if (!ok) return D3DERR_NOTAVAILABLE;
    }
    if (mst == 1 && !n) return D3DERR_NOTAVAILABLE;
    if (pq) wr32(pq, q);
    return D3D_OK;
}
METHOD(d3d_CheckDepthStencilMatch) {
    uint32_t a = M(1), type = M(2), rfmt = M(4), dsfmt = M(5);
    if (a != 0) return D3DERR_INVALIDCALL;
    const Fmt *r = fmt_find(rfmt), *dsf = fmt_find(dsfmt);
    return (type == 1 && r && r->rt && dsf && dsf->kind) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
METHOD(d3d_CheckDeviceFormatConversion) {
    uint32_t a = M(1), type = M(2), sfmt = M(3), tfmt = M(4);
    if (a != 0) return D3DERR_INVALIDCALL;
    const Fmt *s = fmt_find(sfmt), *t = fmt_find(tfmt);
    return (type == 1 && s && t && !s->kind && !s->block && t->rt) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
METHOD(d3d_GetDeviceCaps) {
    if (M(1) != 0 || !M(3)) return D3DERR_INVALIDCALL;
    if (M(2) != 1) return D3DERR_NOTAVAILABLE;
    fill_caps(M(3), 1);
    return D3D_OK;
}
METHOD(d3d_GetAdapterMonitor) { return M(1) == 0 ? 1 : 0; }

// ---------------- device lifecycle ----------------
static void dev_drop_bindings(Device *d) {
    if (d->rec) { SB *r = d->rec; d->rec = NULL; priv_unref(&r->o); }
    state_unbind(&d->st);
    for (int i = 0; i < 4; i++) REBIND(d->rt[i], NULL);
    REBIND(d->ds, NULL);
    d->occl = NULL;
    d->pend.active = 0;
}
static void dev_last_public(Obj *o) {
    Device *d = (Device *)o;
    commit(d);
    wait_serial(d, d->submitted);
    dev_drop_bindings(d);
    if (d->auto_ds) { Surf *s = d->auto_ds; d->auto_ds = NULL; priv_unref(&s->o); }
    if (d->swap) { Swap *s = d->swap; d->swap = NULL; priv_unref(&s->o); }
    for (uint32_t i = 0; i < d->nfvfd; i++) priv_unref(&d->fvfd[i].decl->o);
    d->nfvfd = 0;
    Obj *d3d = d->d3d; d->d3d = NULL;
    priv_unref(d3d);
}
static void dev_destroy(Obj *o) {
    Device *d = (Device *)o;
    if (d->sy) wait_serial(d, d->submitted);
    d->enc = nil; d->blit = nil; d->cmd = nil;
    if (d->sy) process_deferred(d);
    for (uint32_t i = 0; i < d->ndef; i++) if (d->defq[i].kind == 1) { CFRelease(d->defq[i].ref); gvm_free(d->defq[i].a); }
    free(d->defq);
    for (uint32_t i = 0; i < d->nslabs; i++) { CFRelease(d->slabs[i].buf); gvm_free(d->slabs[i].g); }
    free(d->slabs);
    for (int i = 0; i < NCLS; i++) free(d->freel[i]);
    for (uint32_t i = 0; i < d->nchunks; i++) CFRelease(d->chunks[i].buf);
    free(d->chunks);
    d->pso_cache = d->dss_cache = d->samp_cache = d->ff_fns = d->util = nil;
    d->util_lib = nil;
    for (int i = 0; i < 5; i++) d->dummy[i] = nil;
    d->vis = nil; d->cursor = nil; d->samp_lin = d->samp_pt = nil;
    d->enh_color = d->enh_depth = nil;
    d->queue = nil; d->mtl = nil;
    free(d->fvfd); free(d->pal); free(d->st.lights);
    if (d->sy) { pthread_mutex_destroy(&d->sy->mu); pthread_cond_destroy(&d->sy->cv); free(d->sy); }
    free(d);
}
static id<MTLTexture> dummy_tex(Device *d, MTLTextureType type) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = type; td.pixelFormat = MTLPixelFormatRGBA8Unorm; td.width = td.height = td.depth = 1;
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [d->mtl newTextureWithDescriptor:td];
    if (!t) return nil;
    static const uint8_t px[4] = {0, 0, 0, 255};
    int n = type == MTLTextureTypeCube ? 6 : 1;
    for (int s = 0; s < n; s++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:(NSUInteger)s withBytes:px bytesPerRow:4 bytesPerImage:4];
    return t;
}
static id<MTLSamplerState> util_sampler(Device *d, int linear) {
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    return [d->mtl newSamplerStateWithDescriptor:sd];
}
// Metal objects of a new device; 0 (logged) if one cannot be created
static int dev_init_metal(Device *d) {
    d->mtl = rt_metal_device();
    if (!d->mtl) { rt_log("[d3d9] no Metal device"); return 0; }
    d->queue = [d->mtl newCommandQueueWithMaxCommandBufferCount:64];
    d->sy = calloc(1, sizeof *d->sy);
    if (!d->queue || !d->sy) { rt_log("[d3d9] Metal command queue not created"); return 0; }
    pthread_mutex_init(&d->sy->mu, NULL);
    pthread_cond_init(&d->sy->cv, NULL);
    d->pso_cache = [NSMutableDictionary new]; d->dss_cache = [NSMutableDictionary new]; d->samp_cache = [NSMutableDictionary new];
    d->ff_fns = [NSMutableDictionary new]; d->util = [NSMutableDictionary new];
    d->curchunk = -1;
    NSError *err = nil;
    d->util_lib = [d->mtl newLibraryWithSource:@(util_msl) options:[MTLCompileOptions new] error:&err];
    if (!d->util_lib) { rt_log("[d3d9] utility shaders did not compile: %s", err.localizedDescription.UTF8String); return 0; }
    d->dummy[2] = dummy_tex(d, MTLTextureType2D); d->dummy[3] = dummy_tex(d, MTLTextureTypeCube); d->dummy[4] = dummy_tex(d, MTLTextureType3D);
    d->vis = [d->mtl newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared];
    d->samp_lin = util_sampler(d, 1); d->samp_pt = util_sampler(d, 0);
    if (!d->dummy[2] || !d->dummy[3] || !d->dummy[4] || !d->vis || !d->samp_lin || !d->samp_pt) { rt_log("[d3d9] Metal resources not created"); return 0; }
    memset(d->vis.contents, 0, VIS_SLOTS * 8);
    d->n_ms = ms_counts(d->mtl, d->caps_ms_counts);
    return 1;
}
// (re)create the implicit swap chain, its back buffer and the automatic depth-stencil surface, and reset the state
static uint32_t dev_reset(Device *d, const uint32_t *pp, uint32_t focus) {
    commit(d);
    wait_serial(d, d->submitted);
    dev_drop_bindings(d);
    uint32_t err;
    if (!d->swap) {
        Swap *sw = swap_new(d, pp, focus, &err);
        if (!sw) return err;
        sw->o.com.refs = 0; sw->o.priv = 1;   // owned by the device
        sw->implicit = 1;
        d->swap = sw;
    } else {
        Swap *sw = d->swap;
        memcpy(sw->pp, pp, sizeof sw->pp);
        sw->hwnd = pp[7] ? pp[7] : focus;
        sw->layer = (__bridge CAMetalLayer *)rt_window_layer(sw->hwnd);
        if ((err = swap_make_bb(d, sw))) return err;
        layer_setup(sw);
    }
    if (d->auto_ds) { Surf *s = d->auto_ds; d->auto_ds = NULL; priv_unref(&s->o); }
    if (pp[9]) {
        uint32_t ms = pp[4] == 1 ? d->caps_ms_counts[pp[5]] : pp[4];
        Surf *s = surf_new(d, pp[0], pp[1], pp[10], USAGE_DEPTHSTENCIL, POOL_DEFAULT, ms, pp[5], 0, 0, &err);
        if (!s) return err;
        s->o.com.refs = 0; s->o.priv = 1;   // owned by the device
        d->auto_ds = s;
    }
    state_defaults(&d->st, pp[9] != 0, pp[0], pp[1]);
    d->st.swvp = (d->behavior & 0x20) != 0;
    REBIND(d->rt[0], d->swap->bb);
    REBIND(d->ds, d->auto_ds);
    mark_dirty(d);
    d->in_scene = 0;
    memcpy(d->pp, pp, sizeof d->pp);
    enh_bb_w = pp[0]; enh_bb_h = pp[1];
    return D3D_OK;
}
METHOD(d3d_CreateDevice) {
    uint32_t a = M(1), type = M(2), focus = M(3), beh = M(4), gpp = M(5), out = M(6);
    if (!out || !gpp) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    if (a != 0) return D3DERR_INVALIDCALL;
    if (type != 1) return D3DERR_NOTAVAILABLE;
    if (!(beh & 0xE0)) return D3DERR_INVALIDCALL;   // D3DCREATE_SOFTWARE / HARDWARE / MIXED_VERTEXPROCESSING
    Device *d = calloc(1, sizeof *d);
    if (!d) return E_OUTOFMEMORY_;
    obj_wrap(&d->o, &c_device, NULL, dev_destroy);
    d->o.last_public = dev_last_public;
    d->d3d = (Obj *)self; priv_ref(d->d3d);
    d->adapter = a; d->devtype = type; d->focus = focus; d->behavior = beh;
    d->ui_k = d->ui_k_up = 1.0f;
    uint32_t pp[14], err = D3DERR_NOTAVAILABLE;
    if (!dev_init_metal(d) || (err = pp_check(d, gpp, focus, pp)) || (err = dev_reset(d, pp, focus))) {
        obj_release(&d->o);
        return err;
    }
    rt_log("[d3d9] device on %s: %ux%u format %u, %s, %u samples, depth format %u", d->mtl.name.UTF8String, pp[0], pp[1], pp[2],
           pp[8] ? "windowed" : "full screen", pp[4], pp[9] ? pp[10] : 0);
    wr32(out, d->o.com.guest);
    return D3D_OK;
}

// ---------------- state blocks ----------------
#define MSET(a, i) ((a)[(i) >> 6] |= 1ull << ((i) & 63))
#define MHAS(a, i) (((a)[(i) >> 6] >> ((i) & 63)) & 1)
// the state that Set* calls change: the recording state block between BeginStateBlock and EndStateBlock
static State *wst(Device *d) { return d->rec ? &d->rec->st : &d->st; }
static Mask *wmk(Device *d) { return d->rec ? &d->rec->mk : NULL; }
static void changed(Device *d) { if (!d->rec) mark_dirty(d); }
static void mask_light(Mask *k, uint32_t idx) {
    for (uint32_t i = 0; i < k->nlight_idx; i++) if (k->light_idx[i] == idx) return;
    if (k->nlight_idx == k->caplight_idx) {
        uint32_t cap = k->caplight_idx ? k->caplight_idx * 2 : 8;
        if (cap > MAX_LIGHTS) return;
        uint32_t *q = realloc(k->light_idx, cap * sizeof *q);
        if (!q) rt_fatal("[d3d9] out of memory recording a state block");
        k->light_idx = q; k->caplight_idx = cap;
    }
    k->light_idx[k->nlight_idx++] = idx;
}
static const uint8_t rs_pixel[] = {7, 8, 9, 14, 15, 16, 19, 20, 23, 24, 25, 26, 36, 37, 38, 27, 195, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    128, 129, 130, 131, 132, 133, 134, 135, 198, 199, 200, 201, 202, 203, 204, 205, 168, 171, 174, 175, 176, 185, 186, 187, 188, 189,
    190, 191, 192, 193, 194, 206, 207, 208, 209};
static const uint8_t rs_vertex[] = {22, 28, 34, 35, 36, 37, 38, 48, 139, 141, 140, 136, 137, 142, 148, 147, 145, 146, 151, 152, 154,
    155, 156, 157, 158, 159, 160, 161, 162, 163, 166, 167, 170, 172, 173, 178, 179, 180, 181, 182, 183, 184, 143, 29, 9};
static void mask_for(Mask *k, uint32_t type) {
    memset(k, 0, offsetof(Mask, light_idx));
    int all = type == 1, px = type == 2, vx = type == 3;
    if (all) {
        memset(k->rs, 0xFF, sizeof k->rs); memset(k->tss, 0xFF, sizeof k->tss);
        for (int s = 0; s < 20; s++) k->samp[s] = 0xFFFFFFFFu;
        k->tex = 0xFFFFF; k->streams = k->freq = 0xFFFF;
        memset(k->vsf, 0xFF, sizeof k->vsf); memset(k->psf, 0xFF, sizeof k->psf); memset(k->xf, 0xFF, sizeof k->xf);
        k->vsi = k->vsb = k->psi = k->psb = 0xFFFF;
        k->vs = k->ps = k->decl = k->ib = k->vp = k->scissor = k->material = k->npatch = 1; k->clip = 0xFF;
        k->lights_all = 1;
        return;
    }
    if (px) {
        for (size_t i = 0; i < sizeof rs_pixel; i++) MSET(k->rs, rs_pixel[i]);
        for (int s = 0; s < 8; s++) k->tss[s] = ~((1ull << TSS_TEXCOORDINDEX) | (1ull << TSS_TEXTURETRANSFORMFLAGS));
        for (int s = 0; s < 20; s++) k->samp[s] = ~(1u << SAMP_DMAPOFFSET);
        k->ps = 1; memset(k->psf, 0xFF, sizeof k->psf); k->psi = k->psb = 0xFFFF;
    }
    if (vx) {
        for (size_t i = 0; i < sizeof rs_vertex; i++) MSET(k->rs, rs_vertex[i]);
        for (int s = 0; s < 8; s++) k->tss[s] = (1ull << TSS_TEXCOORDINDEX) | (1ull << TSS_TEXTURETRANSFORMFLAGS);
        for (int s = 0; s < 20; s++) k->samp[s] = 1u << SAMP_DMAPOFFSET;
        k->vs = 1; k->decl = 1; k->freq = 0xFFFF; memset(k->vsf, 0xFF, sizeof k->vsf); k->vsi = k->vsb = 0xFFFF;
        k->lights_all = 1;
    }
}
static void copy_light(State *to, const Light *l) {
    Light *t = light_get(to, l->idx);
    if (!t) return;
    memcpy(t->data, l->data, sizeof t->data);
    light_enable(to, l->idx, l->enabled);
}
static void sb_transfer(State *to, State *from, const Mask *k) {
    for (int i = 0; i < 256; i++) if (MHAS(k->rs, i)) to->rs[i] = from->rs[i];
    for (int s = 0; s < 8; s++) if (k->tss[s]) for (int j = 0; j < 33; j++) if ((k->tss[s] >> j) & 1) to->tss[s][j] = from->tss[s][j];
    for (int s = 0; s < 20; s++) if (k->samp[s]) for (int j = 0; j < 14; j++) if ((k->samp[s] >> j) & 1) to->samp[s][j] = from->samp[s][j];
    for (int s = 0; s < 20; s++) if ((k->tex >> s) & 1) REBIND(to->tex[s], from->tex[s]);
    if (k->vs) REBIND(to->vs, from->vs);
    if (k->ps) REBIND(to->ps, from->ps);
    if (k->decl) { REBIND(to->decl, from->decl); to->fvf = from->fvf; }
    if (k->ib) REBIND(to->ib, from->ib);
    for (int s = 0; s < 16; s++) {
        if ((k->streams >> s) & 1) { REBIND(to->stream[s].vb, from->stream[s].vb); to->stream[s].off = from->stream[s].off; to->stream[s].stride = from->stream[s].stride; }
        if ((k->freq >> s) & 1) to->stream[s].freq = from->stream[s].freq;
    }
    for (int i = 0; i < 256; i++) if (MHAS(k->vsf, i)) memcpy(to->vsf[i], from->vsf[i], 16);
    for (int i = 0; i < 224; i++) if (MHAS(k->psf, i)) memcpy(to->psf[i], from->psf[i], 16);
    for (int i = 0; i < 16; i++) {
        if ((k->vsi >> i) & 1) memcpy(to->vsi[i], from->vsi[i], 16);
        if ((k->psi >> i) & 1) memcpy(to->psi[i], from->psi[i], 16);
        if ((k->vsb >> i) & 1) to->vsb[i] = from->vsb[i];
        if ((k->psb >> i) & 1) to->psb[i] = from->psb[i];
    }
    for (int i = 0; i < 512; i++) if (MHAS(k->xf, i)) memcpy(to->xf[i], from->xf[i], 64);
    if (k->vp) memcpy(to->vp, from->vp, sizeof to->vp);
    if (k->scissor) memcpy(to->scissor, from->scissor, sizeof to->scissor);
    if (k->material) memcpy(to->material, from->material, sizeof to->material);
    for (int i = 0; i < 8; i++) if ((k->clip >> i) & 1) memcpy(to->clip[i], from->clip[i], 16);
    if (k->npatch) to->npatch = from->npatch;
    if (k->lights_all) { for (uint32_t i = 0; i < from->nlights; i++) copy_light(to, &from->lights[i]); }
    else for (uint32_t i = 0; i < k->nlight_idx; i++) { Light *l = light_find(from, k->light_idx[i]); if (l) copy_light(to, l); }
}
static void sb_destroy(Obj *o) {
    SB *b = (SB *)o;
    state_unbind(&b->st);
    free(b->st.lights);
    free(b->mk.light_idx);
    free(b);
}
static SB *sb_new(Device *d) {
    SB *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    state_defaults(&b->st, 0, 0, 0);
    obj_wrap(&b->o, &c_sb, d, sb_destroy);
    return b;
}
METHOD(sb_Capture) {
    SB *b = (SB *)self; Device *d = b->o.dev;
    if (d->rec) return D3DERR_INVALIDCALL;
    sb_transfer(&b->st, &d->st, &b->mk);
    return D3D_OK;
}
METHOD(sb_Apply) {
    SB *b = (SB *)self; Device *d = b->o.dev;
    if (d->rec) return D3DERR_INVALIDCALL;
    uint32_t srgb = d->st.rs[RS_SRGBWRITEENABLE];
    sb_transfer(&d->st, &b->st, &b->mk);
    if ((d->st.rs[RS_SRGBWRITEENABLE] != 0) != (srgb != 0)) targets_changing(d);
    mark_dirty(d);
    return D3D_OK;
}

// ---------------- device methods ----------------
#define DEVICE Device *d = (Device *)self
static Tex *tex_of(uint32_t g) { static ComClass *const cls[] = {&c_texture, &c_cube, &c_voltex}; return (Tex *)obj_of(g, cls, 3); }
static VB *vb_of(uint32_t g) { static ComClass *const cls[] = {&c_vb}; return (VB *)obj_of(g, cls, 1); }
static VB *ib_of(uint32_t g) { static ComClass *const cls[] = {&c_ib}; return (VB *)obj_of(g, cls, 1); }
static Shader *vs_of(uint32_t g) { static ComClass *const cls[] = {&c_vs}; return (Shader *)obj_of(g, cls, 1); }
static Shader *ps_of(uint32_t g) { static ComClass *const cls[] = {&c_ps}; return (Shader *)obj_of(g, cls, 1); }
static Decl *decl_of(uint32_t g) { static ComClass *const cls[] = {&c_decl}; return (Decl *)obj_of(g, cls, 1); }
// write an interface pointer with a new reference (0 for none)
static void put_obj(uint32_t out, Obj *o) {
    if (o) obj_addref(o);
    wr32(out, o ? o->com.guest : 0);
}

METHOD(dev_TestCooperativeLevel) { return D3D_OK; }
METHOD(dev_GetAvailableTextureMem) {
    DEVICE;
    uint64_t total = d->mtl.recommendedMaxWorkingSetSize, used = d->mtl.currentAllocatedSize;
    uint64_t a = total > used ? total - used : 0;
    if (a > 0x7FC00000u) a = 0x7FC00000u;
    return (uint32_t)(a & ~0xFFFFFull);
}
METHOD(dev_EvictManagedResources) { return D3D_OK; }
METHOD(dev_GetDirect3D) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; put_obj(M(1), d->d3d); return D3D_OK; }
METHOD(dev_GetDeviceCaps) { if (!M(1)) return D3DERR_INVALIDCALL; fill_caps(M(1), 1); return D3D_OK; }
static void display_mode(Device *d, Swap *sw, uint32_t p) {
    if (sw->pp[8]) { int w = 0, h = 0; rt_cocoa_screen_size(&w, &h); write_mode(p, (uint32_t)w, (uint32_t)h); }
    else write_mode(p, sw->pp[0], sw->pp[1]);
}
METHOD(dev_GetDisplayMode) { DEVICE; if (M(1) != 0 || !M(2)) return D3DERR_INVALIDCALL; display_mode(d, d->swap, M(2)); return D3D_OK; }
METHOD(dev_GetCreationParameters) {
    DEVICE; uint32_t p = M(1);
    if (!p) return D3DERR_INVALIDCALL;
    wr32(p, d->adapter); wr32(p + 4, d->devtype); wr32(p + 8, d->focus); wr32(p + 12, d->behavior);
    return D3D_OK;
}
METHOD(dev_SetCursorProperties) {
    DEVICE; int32_t hx = (int32_t)M(1), hy = (int32_t)M(2);
    Surf *s = surf_of(M(3));
    if (!s || s->tex->fmt != FMT_A8R8G8B8 || s->w > 256 || s->h > 256 || hx < 0 || hy < 0 || (uint32_t)hx >= s->w || (uint32_t)hy >= s->h)
        return D3DERR_INVALIDCALL;
    Tex *t = s->tex;
    uint32_t pitch = s->w * 4;
    uint8_t *px = malloc((size_t)pitch * s->h);
    if (!px) return E_OUTOFMEMORY_;
    if (t->sys) {
        const uint8_t *src = G2H(t->sys[s->face * t->levels + s->level]);
        memcpy(px, src, (size_t)pitch * s->h);
    } else tex_read(d, t, s->face, s->level, 0, 0, 0, s->w, s->h, 1, px, pitch, 0);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:s->w height:s->h mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> ct = [d->mtl newTextureWithDescriptor:td];
    if (!ct) { free(px); return D3DERR_OUTOFVIDEOMEMORY; }
    [ct replaceRegion:MTLRegionMake2D(0, 0, s->w, s->h) mipmapLevel:0 withBytes:px bytesPerRow:pitch];
    free(px);
    d->cursor = ct; d->cur_w = (int)s->w; d->cur_h = (int)s->h; d->hot_x = hx; d->hot_y = hy;
    return D3D_OK;
}
METHOD(dev_SetCursorPosition) {
    DEVICE;
    d->cur_x = (int32_t)M(1); d->cur_y = (int32_t)M(2);
    d->last_mx = rt_mouse_x; d->last_my = rt_mouse_y;
    return D3D_OK;
}
METHOD(dev_ShowCursor) { DEVICE; int was = d->cursor_show; d->cursor_show = M(1) != 0; return (uint32_t)was; }
METHOD(dev_CreateAdditionalSwapChain) {
    DEVICE; uint32_t gpp = M(1), out = M(2);
    if (!gpp || !out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    uint32_t pp[14], err = pp_check(d, gpp, d->focus, pp);
    if (err) return err;
    Swap *sw = swap_new(d, pp, d->focus, &err);
    if (!sw) return err;
    wr32(out, sw->o.com.guest);
    return D3D_OK;
}
METHOD(dev_GetSwapChain) {
    DEVICE;
    if (!M(2)) return D3DERR_INVALIDCALL;
    if (M(1) != 0) { wr32(M(2), 0); return D3DERR_INVALIDCALL; }
    put_obj(M(2), &d->swap->o);
    return D3D_OK;
}
METHOD(dev_GetNumberOfSwapChains) { return 1; }
METHOD(dev_Reset) {
    DEVICE; uint32_t gpp = M(1);
    if (!gpp || d->rec) return D3DERR_INVALIDCALL;
    uint32_t pp[14], err = pp_check(d, gpp, d->focus, pp);
    if (err) return err;
    err = dev_reset(d, pp, d->focus);
    if (err) rt_log("[d3d9] Reset to %ux%u failed: 0x%x", pp[0], pp[1], err);
    return err;
}
METHOD(dev_Present) { DEVICE; uint32_t r = swap_present(d, d->swap, M(1), M(2), M(3)); rt_pause_point(); return r; }
static uint32_t get_bb(Swap *sw, uint32_t idx, uint32_t out) {
    if (!out) return D3DERR_INVALIDCALL;
    if (idx >= sw->pp[3]) { wr32(out, 0); return D3DERR_INVALIDCALL; }
    if (idx > 0) LOG_ONCE("[d3d9] back buffer %u is the same surface as back buffer 0", idx);
    put_obj(out, &sw->bb->o);
    return D3D_OK;
}
METHOD(dev_GetBackBuffer) { DEVICE; if (M(1) != 0) return D3DERR_INVALIDCALL; return get_bb(d->swap, M(2), M(4)); }
static void raster_status(Swap *sw, uint32_t p) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t ns = mach_absolute_time() * tb.numer / tb.denom, period = 1000000000ull / 60;
    uint32_t lines = sw->pp[1] + 45, line = (uint32_t)((ns % period) * lines / period);
    wr32(p, line >= sw->pp[1] ? 1 : 0);
    wr32(p + 4, line >= sw->pp[1] ? 0 : line);
}
METHOD(dev_GetRasterStatus) { DEVICE; if (M(1) != 0 || !M(2)) return D3DERR_INVALIDCALL; raster_status(d->swap, M(2)); return D3D_OK; }
METHOD(dev_SetDialogBoxMode) { return D3D_OK; }
METHOD(dev_SetGammaRamp) {
    DEVICE;
    if (M(1) == 0 && M(3)) memcpy(d->swap->ramp, G2H(M(3)), sizeof d->swap->ramp);
    return 0;
}
METHOD(dev_GetGammaRamp) {
    DEVICE;
    if (M(1) == 0 && M(2)) memcpy(G2H(M(2)), d->swap->ramp, sizeof d->swap->ramp);
    return 0;
}
static uint32_t create_tex(Device *d, uint32_t w, uint32_t h, uint32_t dd, uint32_t levels, uint32_t usage, uint32_t fmt,
                           uint32_t pool, int cube, int volume, uint32_t out, uint32_t shared) {
    if (!out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    if (shared || pool > POOL_SCRATCH) return D3DERR_INVALIDCALL;
    uint32_t err;
    Tex *t = tex_new(d, w, h, dd, levels, usage, fmt, pool, cube, volume, 1, 0, 0, 0, 0, &err);
    if (!t) return err;
    obj_wrap(&t->o, volume ? &c_voltex : cube ? &c_cube : &c_texture, d, tex_destroy);
    t->o.rtype = volume ? RTYPE_VOLUMETEXTURE : cube ? RTYPE_CUBETEXTURE : RTYPE_TEXTURE;
    wr32(out, t->o.com.guest);
    return D3D_OK;
}
METHOD(dev_CreateTexture) { DEVICE; return create_tex(d, M(1), M(2), 1, M(3), M(4), M(5), M(6), 0, 0, M(7), M(8)); }
METHOD(dev_CreateVolumeTexture) { DEVICE; return create_tex(d, M(1), M(2), M(3), M(4), M(5), M(6), M(7), 0, 1, M(8), M(9)); }
METHOD(dev_CreateCubeTexture) { DEVICE; return create_tex(d, M(1), M(1), 1, M(2), M(3), M(4), M(5), 1, 0, M(6), M(7)); }
static uint32_t create_buf(Device *d, uint32_t len, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out, uint32_t shared, int index) {
    if (!out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    if (shared || !len || pool > POOL_SCRATCH || pool == POOL_SCRATCH) return D3DERR_INVALIDCALL;
    if (index && fmt != FMT_INDEX16 && fmt != FMT_INDEX32) return D3DERR_INVALIDCALL;
    VB *b = calloc(1, sizeof *b);
    if (!b) return E_OUTOFMEMORY_;
    if (!store_alloc(d, len, &b->st)) { free(b); return D3DERR_OUTOFVIDEOMEMORY; }
    b->len = len; b->usage = usage; b->fmt = index ? fmt : FMT_VERTEXDATA; b->pool = pool;
    obj_wrap(&b->o, index ? &c_ib : &c_vb, d, vb_destroy);
    b->o.rtype = index ? RTYPE_INDEXBUFFER : RTYPE_VERTEXBUFFER;
    b->fvf = index ? 0 : fmt;
    wr32(out, b->o.com.guest);
    return D3D_OK;
}
METHOD(dev_CreateVertexBuffer) { DEVICE; return create_buf(d, M(1), M(2), M(3), M(4), M(5), M(6), 0); }
METHOD(dev_CreateIndexBuffer) { DEVICE; return create_buf(d, M(1), M(2), M(3), M(4), M(5), M(6), 1); }
static int ms_valid(Device *d, uint32_t type, uint32_t q, uint32_t *count) {
    if (type == 0) { *count = 1; return 1; }
    if (type == 1) { if (q >= d->n_ms) return 0; *count = d->caps_ms_counts[q]; return 1; }
    for (uint32_t i = 0; i < d->n_ms; i++) if (d->caps_ms_counts[i] == type) { *count = type; return q == 0; }
    return 0;
}
static uint32_t create_surf(Device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t mst, uint32_t q,
                            int lockable, uint32_t out, uint32_t shared) {
    if (!out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    uint32_t count;
    if (shared || !ms_valid(d, mst, q, &count)) return D3DERR_INVALIDCALL;
    uint32_t err;
    Surf *s = surf_new(d, w, h, fmt, usage, pool, count, q, lockable, 0, &err);
    if (!s) return err;
    wr32(out, s->o.com.guest);
    return D3D_OK;
}
METHOD(dev_CreateRenderTarget) { DEVICE; return create_surf(d, M(1), M(2), M(3), USAGE_RENDERTARGET, POOL_DEFAULT, M(4), M(5), M(6) != 0, M(7), M(8)); }
METHOD(dev_CreateDepthStencilSurface) { DEVICE; return create_surf(d, M(1), M(2), M(3), USAGE_DEPTHSTENCIL, POOL_DEFAULT, M(4), M(5), 0, M(7), M(8)); }
METHOD(dev_CreateOffscreenPlainSurface) {
    DEVICE;
    if (M(4) == POOL_MANAGED) return D3DERR_INVALIDCALL;
    return create_surf(d, M(1), M(2), M(3), 0, M(4), 0, 0, 1, M(5), M(6));
}
METHOD(dev_UpdateSurface) {
    DEVICE; Surf *s = surf_of(M(1)), *t = surf_of(M(3));
    if (!s || !t) return D3DERR_INVALIDCALL;
    return update_surface(d, s, M(2), t, M(4));
}
METHOD(dev_UpdateTexture) {
    DEVICE; Tex *s = tex_of(M(1)), *t = tex_of(M(2));
    if (!s || !t) return D3DERR_INVALIDCALL;
    return update_texture(d, s, t);
}
METHOD(dev_GetRenderTargetData) {
    DEVICE; Surf *s = surf_of(M(1)), *t = surf_of(M(2));
    if (!s || !t) return D3DERR_INVALIDCALL;
    return get_rt_data(d, s, t);
}
static uint32_t front_data(Device *d, Swap *sw, Surf *t) {
    Tex *dt = t->tex;
    if (!dt->sys || dt->fmt != FMT_A8R8G8B8) return D3DERR_INVALIDCALL;
    Tex *bt = sw->bb->tex;
    uint8_t *dst = G2H(dt->sys[t->face * dt->levels + t->level]);
    uint32_t dpitch = t->w * 4;
    memset(dst, 0, (size_t)dpitch * t->h);
    if (!sw->front) return D3D_OK;   // nothing presented yet: black
    uint32_t w = bt->w < t->w ? bt->w : t->w, h = bt->h < t->h ? bt->h : t->h;
    if (bt->w != t->w || bt->h != t->h) LOG_ONCE("[d3d9] GetFrontBufferData: %ux%u front buffer copied into a %ux%u surface", bt->w, bt->h, t->w, t->h);
    uint32_t bpp = bt->f->bpp, spitch = bt->w * bpp;
    int half = sw->front.pixelFormat == MTLPixelFormatRGBA16Float;
    id<MTLBuffer> b = [d->mtl newBufferWithLength:(NSUInteger)spitch * bt->h options:MTLResourceStorageModeShared];
    if (!b) return E_OUTOFMEMORY_;
    [get_blit(d) copyFromTexture:sw->front sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(bt->w, bt->h, 1)
                        toBuffer:b destinationOffset:0 destinationBytesPerRow:spitch destinationBytesPerImage:(NSUInteger)spitch * bt->h];
    wait_serial(d, cur_serial(d));
    const uint8_t *src = b.contents;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *o = dst + (size_t)y * dpitch;
        const uint8_t *i = src + (size_t)y * spitch;
        for (uint32_t x = 0; x < w; x++) {
            if (bpp == 4) { memcpy(o + x * 4, i + x * 4, 4); o[x * 4 + 3] = 0xFF; }
            else if (bpp == 8 && half) {   // RGBA16Float (rt_enh.h reduced banding, a back buffer) -> B, G, R, A bytes
                __fp16 v[4]; memcpy(v, i + x * 8, 8);
                for (int c = 0; c < 3; c++) {
                    float fv = (float)v[c];
                    fv = fv > 0.0f ? (fv < 1.0f ? fv : 1.0f) : 0.0f;   // NaN -> 0
                    o[x * 4 + 2 - c] = (uint8_t)(fv * 255.0f + 0.5f);
                }
                o[x * 4 + 3] = 0xFF;
            } else if (bpp == 8) {   // RGBA16Unorm (rt_enh.h reduced banding) -> B, G, R, A bytes
                uint16_t v[4]; memcpy(v, i + x * 8, 8);
                for (int c = 0; c < 3; c++) o[x * 4 + 2 - c] = (uint8_t)(((uint32_t)v[c] * 255u + 32767u) / 65535u);
                o[x * 4 + 3] = 0xFF;
            } else {
                uint16_t v; memcpy(&v, i + x * 2, 2);
                uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31;
                o[x * 4] = (uint8_t)(bl * 255 / 31); o[x * 4 + 1] = (uint8_t)(g * 255 / 63); o[x * 4 + 2] = (uint8_t)(r * 255 / 31); o[x * 4 + 3] = 0xFF;
            }
        }
    }
    return D3D_OK;
}
METHOD(dev_GetFrontBufferData) { DEVICE; Surf *t = surf_of(M(2)); if (M(1) != 0 || !t) return D3DERR_INVALIDCALL; return front_data(d, d->swap, t); }
METHOD(dev_StretchRect) {
    DEVICE; Surf *s = surf_of(M(1)), *t = surf_of(M(3));
    if (!s || !t) return D3DERR_INVALIDCALL;
    if (s->swap || t->swap) enh_apply(d);
    return stretch_rect(d, s, M(2), t, M(4), M(5));
}
METHOD(dev_ColorFill) { DEVICE; Surf *s = surf_of(M(1)); if (!s) return D3DERR_INVALIDCALL; return color_fill(d, s, M(2), M(3)); }
METHOD(dev_SetRenderTarget) {
    DEVICE; uint32_t i = M(1);
    if (i >= 4) return D3DERR_INVALIDCALL;
    Surf *s = M(2) ? surf_of(M(2)) : NULL;
    if (M(2) && !s) return D3DERR_INVALIDCALL;
    if (!s && i == 0) return D3DERR_INVALIDCALL;
    if (s && (!s->tex->mtl || !(s->tex->usage & USAGE_RENDERTARGET) || s->volume)) return D3DERR_INVALIDCALL;
    if (s != d->rt[i] && !(i == 0 && s && (s->swap || enh_is_light_target(s)))) enh_apply(d);
    targets_changing(d);
    REBIND(d->rt[i], s);
    if (i == 0) {
        State *S = &d->st;
        S->vp[0] = S->vp[1] = 0; S->vp[2] = s->w; S->vp[3] = s->h; S->vp[4] = 0; S->vp[5] = ubits(1.0f);
        S->scissor[0] = S->scissor[1] = 0; S->scissor[2] = (int32_t)s->w; S->scissor[3] = (int32_t)s->h;
    }
    mark_dirty(d);
    return D3D_OK;
}
METHOD(dev_GetRenderTarget) {
    DEVICE; uint32_t i = M(1), out = M(2);
    if (i >= 4 || !out) return D3DERR_INVALIDCALL;
    put_obj(out, d->rt[i] ? &d->rt[i]->o : NULL);
    return d->rt[i] ? D3D_OK : D3DERR_NOTFOUND;
}
METHOD(dev_SetDepthStencilSurface) {
    DEVICE; Surf *s = M(1) ? surf_of(M(1)) : NULL;
    if (M(1) && (!s || !s->tex->f->kind)) return D3DERR_INVALIDCALL;
    // the scene ends when its depth buffer is replaced or removed while the back buffer is the target; binding a
    // depth buffer where there was none does not end it (the game binds its scene depth buffer again during the scene)
    if (s != d->ds && d->rt[0] && d->rt[0]->swap && d->ds && d->ds->tex->mtl == d->enh_depth) enh_apply(d);
    targets_changing(d);
    REBIND(d->ds, s);
    mark_dirty(d);
    return D3D_OK;
}
METHOD(dev_GetDepthStencilSurface) {
    DEVICE; uint32_t out = M(1);
    if (!out) return D3DERR_INVALIDCALL;
    put_obj(out, d->ds ? &d->ds->o : NULL);
    return d->ds ? D3D_OK : D3DERR_NOTFOUND;
}
METHOD(dev_BeginScene) { DEVICE; if (d->in_scene) return D3DERR_INVALIDCALL; d->in_scene = 1; return D3D_OK; }
METHOD(dev_EndScene) { DEVICE; if (!d->in_scene) return D3DERR_INVALIDCALL; d->in_scene = 0; return D3D_OK; }
METHOD(dev_Clear) {
    DEVICE; uint32_t count = M(1), rects = M(2), flags = M(3) & 7, color = M(4), stencil = M(6);
    float z = MF(5);
    if (rects && !count) rects = 0;
    if (!rects && count) return D3DERR_INVALIDCALL;
    if ((flags & 6) && !d->ds) return D3DERR_INVALIDCALL;
    if ((flags & 4) && d->ds->tex->f->kind != 2) flags &= ~4u;
    if (!flags) return D3D_OK;
    if ((flags & 2) && d->rt[0] && d->rt[0]->swap) enh_apply(d);
    State *S = &d->st;
    Surf *t0 = d->rt[0];
    int64_t W = t0->w, H = t0->h;
    int64_t x0 = S->vp[0], y0 = S->vp[1], x1 = (int64_t)S->vp[0] + S->vp[2], y1 = (int64_t)S->vp[1] + S->vp[3];
    if (S->rs[RS_SCISSORTESTENABLE]) {
        if (S->scissor[0] > x0) x0 = S->scissor[0];
        if (S->scissor[1] > y0) y0 = S->scissor[1];
        if (S->scissor[2] < x1) x1 = S->scissor[2];
        if (S->scissor[3] < y1) y1 = S->scissor[3];
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return D3D_OK;
    float col[4]; color4(color, col);
    z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    int full = !rects && x0 == 0 && y0 == 0 && x1 == W && y1 == H &&
               (!(flags & 6) || (d->ds->w == (uint32_t)W && d->ds->h == (uint32_t)H));
    if (full) {
        // a clear of whole targets becomes the load action of the next render pass
        end_pass(d);
        if (!d->pend.active) d->pend.flags = 0;
        d->pend.active = 1;
        d->pend.flags |= flags;
        if (flags & 1) memcpy(d->pend.color, col, sizeof col);
        if (flags & 2) d->pend.z = z;
        if (flags & 4) d->pend.stencil = stencil;
        return D3D_OK;
    }
    for (uint32_t i = 0; i < (rects ? count : 1); i++) {
        int64_t a0 = x0, b0 = y0, a1 = x1, b1 = y1;
        if (rects) {
            int32_t r[4]; memcpy(r, G2H(rects + 16 * i), 16);
            if (r[0] > a0) a0 = r[0];
            if (r[1] > b0) b0 = r[1];
            if (r[2] < a1) a1 = r[2];
            if (r[3] < b1) b1 = r[3];
            if (a1 <= a0 || b1 <= b0) continue;
        }
        int64_t sc = t0->tex->sc;   // the storage of the target can be larger than its Direct3D size
        clear_quad(d, flags, col, z, stencil, (int32_t)(a0 * sc), (int32_t)(b0 * sc), (int32_t)(a1 * sc), (int32_t)(b1 * sc));
    }
    d->cur_pso = nil; d->cur_dss = nil;
    return D3D_OK;
}
static int xf_index(uint32_t st) { return st < 512 ? (int)st : -1; }
METHOD(dev_SetTransform) {
    DEVICE; int i = xf_index(M(1));
    if (i < 0 || !M(2)) return D3DERR_INVALIDCALL;
    State *S = wst(d); memcpy(S->xf[i], G2H(M(2)), 64);
    if (d->rec) MSET(d->rec->mk.xf, i);
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetTransform) {
    DEVICE; int i = xf_index(M(1));
    if (i < 0 || !M(2)) return D3DERR_INVALIDCALL;
    memcpy(G2H(M(2)), d->st.xf[i], 64);
    return D3D_OK;
}
METHOD(dev_MultiplyTransform) {
    DEVICE; int i = xf_index(M(1));
    if (i < 0 || !M(2)) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    float m_[16]; memcpy(m_, G2H(M(2)), 64);
    mat_mul(S->xf[i], m_, S->xf[i]);
    if (d->rec) MSET(d->rec->mk.xf, i);
    changed(d);
    return D3D_OK;
}
METHOD(dev_SetViewport) {
    DEVICE;
    if (!M(1)) return D3DERR_INVALIDCALL;
    uint32_t v[6]; memcpy(v, G2H(M(1)), 24);
    if (!v[2] || !v[3]) return D3DERR_INVALIDCALL;
    State *S = wst(d); memcpy(S->vp, v, sizeof v);
    if (d->rec) d->rec->mk.vp = 1;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetViewport) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; memcpy(G2H(M(1)), d->st.vp, 24); return D3D_OK; }
METHOD(dev_SetMaterial) {
    DEVICE;
    if (!M(1)) return D3DERR_INVALIDCALL;
    State *S = wst(d); memcpy(S->material, G2H(M(1)), sizeof S->material);
    if (d->rec) d->rec->mk.material = 1;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetMaterial) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; memcpy(G2H(M(1)), d->st.material, sizeof d->st.material); return D3D_OK; }
METHOD(dev_SetLight) {
    DEVICE; uint32_t idx = M(1), p = M(2);
    if (!p) return D3DERR_INVALIDCALL;
    uint32_t v[26]; memcpy(v, G2H(p), sizeof v);
    if (v[0] < 1 || v[0] > 3) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    Light *l = light_get(S, idx);
    if (!l) return E_OUTOFMEMORY_;
    memcpy(l->data, v, sizeof v);
    if (d->rec) mask_light(&d->rec->mk, idx);
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetLight) {
    DEVICE; Light *l = light_find(&d->st, M(1));
    if (!l || !M(2)) return D3DERR_INVALIDCALL;
    memcpy(G2H(M(2)), l->data, sizeof l->data);
    return D3D_OK;
}
METHOD(dev_LightEnable) {
    DEVICE; State *S = wst(d);
    light_enable(S, M(1), M(2) != 0);
    if (d->rec) mask_light(&d->rec->mk, M(1));
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetLightEnable) {
    DEVICE; Light *l = light_find(&d->st, M(1));
    if (!l || !M(2)) return D3DERR_INVALIDCALL;
    wr32(M(2), l->enabled ? 128 : 0);
    return D3D_OK;
}
METHOD(dev_SetClipPlane) {
    DEVICE; uint32_t i = M(1);
    if (i >= 8 || !M(2)) return D3DERR_INVALIDCALL;
    State *S = wst(d); memcpy(S->clip[i], G2H(M(2)), 16);
    if (d->rec) d->rec->mk.clip |= (uint8_t)(1u << i);
    return D3D_OK;
}
METHOD(dev_GetClipPlane) {
    DEVICE; uint32_t i = M(1);
    if (i >= 8 || !M(2)) return D3DERR_INVALIDCALL;
    memcpy(G2H(M(2)), d->st.clip[i], 16);
    return D3D_OK;
}
METHOD(dev_SetRenderState) {
    DEVICE; uint32_t s = M(1), v = M(2);
    if (s >= 256) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    if (!d->rec && s == RS_SRGBWRITEENABLE && (S->rs[s] != 0) != (v != 0)) targets_changing(d);
    S->rs[s] = v;
    if (d->rec) MSET(d->rec->mk.rs, s);
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetRenderState) {
    DEVICE; uint32_t s = M(1);
    if (s >= 256 || !M(2)) return D3DERR_INVALIDCALL;
    wr32(M(2), d->st.rs[s]);
    return D3D_OK;
}
METHOD(dev_CreateStateBlock) {
    DEVICE; uint32_t type = M(1), out = M(2);
    if (!out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    if (type < 1 || type > 3 || d->rec) return D3DERR_INVALIDCALL;
    SB *b = sb_new(d);
    if (!b) return E_OUTOFMEMORY_;
    b->type = type;
    mask_for(&b->mk, type);
    sb_transfer(&b->st, &d->st, &b->mk);
    wr32(out, b->o.com.guest);
    return D3D_OK;
}
METHOD(dev_BeginStateBlock) {
    DEVICE;
    if (d->rec) return D3DERR_INVALIDCALL;
    SB *b = sb_new(d);
    if (!b) return E_OUTOFMEMORY_;
    b->o.com.refs = 0; b->o.priv = 1;   // held by the device until EndStateBlock
    d->rec = b;
    return D3D_OK;
}
METHOD(dev_EndStateBlock) {
    DEVICE; uint32_t out = M(1);
    if (!out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    if (!d->rec) return D3DERR_INVALIDCALL;
    SB *b = d->rec; d->rec = NULL;
    b->o.com.refs = 1; b->o.priv = 0;
    wr32(out, b->o.com.guest);
    return D3D_OK;
}
METHOD(dev_SetClipStatus) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; memcpy(d->st.clipstatus, G2H(M(1)), 8); return D3D_OK; }
METHOD(dev_GetClipStatus) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; memcpy(G2H(M(1)), d->st.clipstatus, 8); return D3D_OK; }
METHOD(dev_GetTexture) {
    DEVICE; int s = sampler_index(M(1));
    if (s < 0 || !M(2)) return D3DERR_INVALIDCALL;
    put_obj(M(2), d->st.tex[s]);
    return D3D_OK;
}
METHOD(dev_SetTexture) {
    DEVICE; int s = sampler_index(M(1));
    if (s < 0) return D3DERR_INVALIDCALL;
    Tex *t = M(2) ? tex_of(M(2)) : NULL;
    if (M(2) && !t) return D3DERR_INVALIDCALL;
    if (t && !t->mtl) { LOG_ONCE("[d3d9] SetTexture of a SYSTEMMEM or SCRATCH texture: the stage reads no texture"); t = NULL; }
    State *S = wst(d);
    REBIND(S->tex[s], t);
    if (d->rec) d->rec->mk.tex |= 1u << s;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetTextureStageState) {
    DEVICE; uint32_t st = M(1), ty = M(2);
    if (st >= 8 || ty >= 33 || !M(3)) return D3DERR_INVALIDCALL;
    wr32(M(3), d->st.tss[st][ty]);
    return D3D_OK;
}
METHOD(dev_SetTextureStageState) {
    DEVICE; uint32_t st = M(1), ty = M(2);
    if (st >= 8 || ty >= 33) return D3DERR_INVALIDCALL;
    State *S = wst(d); S->tss[st][ty] = M(3);
    if (d->rec) d->rec->mk.tss[st] |= 1ull << ty;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetSamplerState) {
    DEVICE; int s = sampler_index(M(1)); uint32_t ty = M(2);
    if (s < 0 || ty >= 14 || !M(3)) return D3DERR_INVALIDCALL;
    wr32(M(3), d->st.samp[s][ty]);
    return D3D_OK;
}
METHOD(dev_SetSamplerState) {
    DEVICE; int s = sampler_index(M(1)); uint32_t ty = M(2);
    if (s < 0 || ty >= 14) return D3DERR_INVALIDCALL;
    State *S = wst(d); S->samp[s][ty] = M(3);
    if (d->rec) d->rec->mk.samp[s] |= 1u << ty;
    changed(d);
    return D3D_OK;
}
METHOD(dev_ValidateDevice) { if (M(1)) wr32(M(1), 1); return D3D_OK; }
#define MAX_PALETTES 4096u
METHOD(dev_SetPaletteEntries) {
    DEVICE; uint32_t n = M(1), p = M(2);
    if (n >= MAX_PALETTES || !p) return D3DERR_INVALIDCALL;
    if (n >= d->npal) {
        if (n >= d->cappal) {
            uint32_t cap = d->cappal ? d->cappal : 16;
            while (cap <= n) cap *= 2;
            void *q = realloc(d->pal, cap * sizeof *d->pal);
            if (!q) return E_OUTOFMEMORY_;
            d->pal = q; d->cappal = cap;
        }
        memset(&d->pal[d->npal], 0, (n + 1 - d->npal) * sizeof *d->pal);
        d->npal = n + 1;
    }
    memcpy(d->pal[n].e, G2H(p), 1024);
    d->pal[n].n = 1;
    return D3D_OK;
}
METHOD(dev_GetPaletteEntries) {
    DEVICE; uint32_t n = M(1), p = M(2);
    if (n >= d->npal || !d->pal[n].n || !p) return D3DERR_INVALIDCALL;
    memcpy(G2H(p), d->pal[n].e, 1024);
    return D3D_OK;
}
METHOD(dev_SetCurrentTexturePalette) { DEVICE; d->st.cur_palette = M(1); return D3D_OK; }
METHOD(dev_GetCurrentTexturePalette) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; wr32(M(1), d->st.cur_palette); return D3D_OK; }
METHOD(dev_SetScissorRect) {
    DEVICE;
    if (!M(1)) return D3DERR_INVALIDCALL;
    State *S = wst(d); memcpy(S->scissor, G2H(M(1)), 16);
    if (d->rec) d->rec->mk.scissor = 1;
    return D3D_OK;
}
METHOD(dev_GetScissorRect) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; memcpy(G2H(M(1)), d->st.scissor, 16); return D3D_OK; }
METHOD(dev_SetSoftwareVertexProcessing) { DEVICE; d->st.swvp = M(1) != 0; return D3D_OK; }
METHOD(dev_GetSoftwareVertexProcessing) { DEVICE; return (uint32_t)d->st.swvp; }
METHOD(dev_SetNPatchMode) {
    DEVICE; State *S = wst(d); S->npatch = MF(1);
    if (d->rec) d->rec->mk.npatch = 1;
    if (S->npatch != 0.0f) LOG_ONCE("[d3d9] N-patches are not supported: geometry is drawn untessellated");
    return D3D_OK;
}
METHOD(dev_GetNPatchMode) { DEVICE; FPUSH(c, d->st.npatch); return 0; }

static uint32_t draw_up(Device *d, uint32_t pt, uint32_t prims, uint32_t data, uint32_t stride, int indexed, uint32_t minv,
                        uint32_t numv, uint32_t idx, uint32_t ifmt) {
    MTLPrimitiveType mt; uint32_t n;
    if (!data || !stride || !prim_count(pt, prims, &mt, &n)) return D3DERR_INVALIDCALL;
    if (!prims) return D3D_OK;
    if (prims > 0x555555u) return D3DERR_INVALIDCALL;
    uint64_t verts = indexed ? (uint64_t)minv + numv : n;
    uint64_t vbytes = verts * stride;
    if (vbytes > (64u << 20)) return D3DERR_INVALIDCALL;
    UpStream up;
    void *vp = ring_alloc(d, (uint32_t)vbytes, &up.buf, &up.off);
    memcpy(vp, G2H(data), (size_t)vbytes);
    up.stride = stride; up.on = 1; up.len = (uint32_t)vbytes; up.cpu = vp;
    uint32_t r;
    if (indexed) {
        if (!idx || (ifmt != FMT_INDEX16 && ifmt != FMT_INDEX32)) return D3DERR_INVALIDCALL;
        int i32 = ifmt == FMT_INDEX32;
        uint32_t ibytes = n * (i32 ? 4u : 2u);
        __unsafe_unretained id<MTLBuffer> ib; uint32_t io;
        void *ip = ring_alloc(d, ibytes, &ib, &io);
        memcpy(ip, G2H(idx), ibytes);
        r = draw(d, pt, prims, 1, 0, 0, ib, io, i32, G2H(idx), &up);
    } else r = draw(d, pt, prims, 0, 0, 0, nil, 0, 0, NULL, &up);
    // Direct3D 9 unbinds stream 0 (and the indices) after a user-pointer draw
    State *S = &d->st;
    REBIND(S->stream[0].vb, NULL); S->stream[0].off = 0; S->stream[0].stride = 0;
    if (indexed) REBIND(S->ib, NULL);
    return r;
}
METHOD(dev_DrawPrimitive) { DEVICE; return draw(d, M(1), M(3), 0, M(2), 0, nil, 0, 0, NULL, NULL); }
METHOD(dev_DrawIndexedPrimitive) {
    DEVICE; uint32_t pt = M(1), start = M(5), prims = M(6); int32_t base = (int32_t)M(2);
    VB *ib = d->st.ib;
    if (!ib) return D3DERR_INVALIDCALL;
    MTLPrimitiveType mt; uint32_t n;
    if (!prim_count(pt, prims, &mt, &n)) return D3DERR_INVALIDCALL;
    int i32 = ib->fmt == FMT_INDEX32;
    uint64_t end = ((uint64_t)start + n) * (i32 ? 4u : 2u);
    if (end > ib->len) { LOG_ONCE("[d3d9] DrawIndexedPrimitive reads past the end of the index buffer"); return D3DERR_INVALIDCALL; }
    uint32_t r = draw(d, pt, prims, 1, start, base, (__bridge id<MTLBuffer>)ib->st.buf, ib->st.off, i32, G2H(ib->st.g), NULL);
    if (d->cmd) ib->last_use = d->serial;
    return r;
}
METHOD(dev_DrawPrimitiveUP) { DEVICE; return draw_up(d, M(1), M(2), M(3), M(4), 0, 0, 0, 0, 0); }
METHOD(dev_DrawIndexedPrimitiveUP) { DEVICE; return draw_up(d, M(1), M(4), M(7), M(8), 1, M(2), M(3), M(5), M(6)); }
METHOD(dev_ProcessVertices) { LOG_ONCE("[d3d9] ProcessVertices is not supported"); return D3DERR_INVALIDCALL; }
METHOD(dev_CreateVertexDeclaration) {
    DEVICE; uint32_t p = M(1), out = M(2);
    if (!p || !out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    Decl *dc = calloc(1, sizeof *dc);
    if (!dc) return E_OUTOFMEMORY_;
    uint32_t n = 0;
    for (;; n++) {
        if (n == 64) { free(dc); return D3DERR_INVALIDCALL; }
        const uint8_t *e = G2H(p + 8 * n);
        uint16_t stream, off; memcpy(&stream, e, 2); memcpy(&off, e + 2, 2);
        if (stream == 0xFF && e[4] == DT_UNUSED) break;
        if (stream > 15 || e[4] >= DT_UNUSED || e[6] > DU_SAMPLE) { free(dc); return D3DERR_INVALIDCALL; }
        dc->el[n].stream = stream; dc->el[n].offset = off; dc->el[n].type = e[4]; dc->el[n].method = e[5];
        dc->el[n].usage = e[6]; dc->el[n].index = e[7];
    }
    dc->n = n;
    obj_wrap(&dc->o, &c_decl, d, decl_destroy);
    wr32(out, dc->o.com.guest);
    return D3D_OK;
}
METHOD(dev_SetVertexDeclaration) {
    DEVICE; Decl *dc = M(1) ? decl_of(M(1)) : NULL;
    if (M(1) && !dc) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    REBIND(S->decl, dc);
    S->fvf = dc ? dc->fvf : 0;
    if (d->rec) d->rec->mk.decl = 1;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetVertexDeclaration) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; put_obj(M(1), d->st.decl ? &d->st.decl->o : NULL); return D3D_OK; }
#define MAX_FVF_DECLS 1024u
METHOD(dev_SetFVF) {
    DEVICE; uint32_t fvf = M(1);
    State *S = wst(d);
    if (!fvf) { S->fvf = 0; if (d->rec) d->rec->mk.decl = 1; return D3D_OK; }
    Decl *dc = NULL;
    for (uint32_t i = 0; i < d->nfvfd; i++) if (d->fvfd[i].fvf == fvf) { dc = d->fvfd[i].decl; break; }
    if (!dc) {
        if (d->nfvfd >= MAX_FVF_DECLS) { rt_log("[d3d9] more than %u FVF codes in use", MAX_FVF_DECLS); return D3DERR_INVALIDCALL; }
        dc = calloc(1, sizeof *dc);
        if (!dc) return E_OUTOFMEMORY_;
        if (!fvf_to_decl(fvf, dc)) { free(dc); rt_log("[d3d9] FVF 0x%x is not valid", fvf); return D3DERR_INVALIDCALL; }
        if (d->nfvfd == d->capfvfd) {
            uint32_t cap = d->capfvfd ? d->capfvfd * 2 : 16;
            void *q = realloc(d->fvfd, cap * sizeof *d->fvfd);
            if (!q) { free(dc); return E_OUTOFMEMORY_; }
            d->fvfd = q; d->capfvfd = cap;
        }
        obj_wrap(&dc->o, &c_decl, d, decl_destroy);
        dc->o.com.refs = 0; dc->o.priv = 1;   // held by the FVF cache
        d->fvfd[d->nfvfd].fvf = fvf; d->fvfd[d->nfvfd].decl = dc; d->nfvfd++;
    }
    REBIND(S->decl, dc);
    S->fvf = fvf;
    if (d->rec) d->rec->mk.decl = 1;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetFVF) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; wr32(M(1), d->st.fvf); return D3D_OK; }
static uint32_t create_shader(Device *d, uint32_t p, uint32_t out, int ps) {
    if (!p || !out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    const uint32_t *tok = G2H(p);
    size_t n = d3d9_shader_length(tok, 1u << 20);
    if (!n) return D3DERR_INVALIDCALL;
    uint32_t ver = tok[0];
    if ((ver >> 16) != (ps ? 0xFFFFu : 0xFFFEu)) return D3DERR_INVALIDCALL;
    Shader *s = calloc(1, sizeof *s);
    if (!s) return E_OUTOFMEMORY_;
    s->ps = ps;
    s->tok = malloc(n * 4);
    if (!s->tok) { free(s); return E_OUTOFMEMORY_; }
    memcpy(s->tok, tok, n * 4);
    s->ntok = (uint32_t)n;
    if (!d3d9_translate_shader(s->tok, n, NULL, &s->info)) {
        rt_log("[d3d9] %s shader %u.%u not translated: %s", ps ? "pixel" : "vertex", (ver >> 8) & 0xFF, ver & 0xFF, s->info.error);
        free(s->info.msl); free(s->tok); free(s);
        return D3DERR_INVALIDCALL;
    }
    if (ps) {
        s->var = calloc(MAX_PVAR, sizeof *s->var);
        if (!s->var) { free(s->info.msl); free(s->tok); free(s); return E_OUTOFMEMORY_; }
    } else {
        s->fns = [NSMutableDictionary new];
        enh_light_shader(s);
        enh_ui_shader(s);
    }
    obj_wrap(&s->o, ps ? &c_ps : &c_vs, d, shader_destroy);
    wr32(out, s->o.com.guest);
    return D3D_OK;
}
METHOD(dev_CreateVertexShader) { DEVICE; return create_shader(d, M(1), M(2), 0); }
METHOD(dev_SetVertexShader) {
    DEVICE; Shader *s = M(1) ? vs_of(M(1)) : NULL;
    if (M(1) && !s) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    REBIND(S->vs, s);
    if (d->rec) d->rec->mk.vs = 1;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetVertexShader) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; put_obj(M(1), d->st.vs ? &d->st.vs->o : NULL); return D3D_OK; }
// shader constants: kind 0 float (4 floats), 1 int (4 ints), 2 bool (1 BOOL)
static uint32_t set_consts(Device *d, int ps, int kind, uint32_t start, uint32_t p, uint32_t count) {
    uint32_t lim = kind ? 16 : ps ? 224 : 256;
    if ((uint64_t)start + count > lim || (count && !p)) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    Mask *k = wmk(d);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t r = start + i;
        if (kind == 0) {
            memcpy(ps ? S->psf[r] : S->vsf[r], G2H(p + 16 * i), 16);
            if (k) MSET(ps ? k->psf : k->vsf, r);
        } else if (kind == 1) {
            memcpy(ps ? S->psi[r] : S->vsi[r], G2H(p + 16 * i), 16);
            if (k) { if (ps) k->psi |= (uint16_t)(1u << r); else k->vsi |= (uint16_t)(1u << r); }
        } else {
            uint32_t v = rd32(p + 4 * i) != 0;
            if (ps) S->psb[r] = v; else S->vsb[r] = v;
            if (k) { if (ps) k->psb |= (uint16_t)(1u << r); else k->vsb |= (uint16_t)(1u << r); }
        }
    }
    changed(d);
    return D3D_OK;
}
static uint32_t get_consts(Device *d, int ps, int kind, uint32_t start, uint32_t p, uint32_t count) {
    uint32_t lim = kind ? 16 : ps ? 224 : 256;
    if ((uint64_t)start + count > lim || (count && !p)) return D3DERR_INVALIDCALL;
    State *S = &d->st;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t r = start + i;
        if (kind == 0) memcpy(G2H(p + 16 * i), ps ? S->psf[r] : S->vsf[r], 16);
        else if (kind == 1) memcpy(G2H(p + 16 * i), ps ? S->psi[r] : S->vsi[r], 16);
        else wr32(p + 4 * i, ps ? S->psb[r] : S->vsb[r]);
    }
    return D3D_OK;
}
METHOD(dev_SetVertexShaderConstantF) { DEVICE; return set_consts(d, 0, 0, M(1), M(2), M(3)); }
METHOD(dev_GetVertexShaderConstantF) { DEVICE; return get_consts(d, 0, 0, M(1), M(2), M(3)); }
METHOD(dev_SetVertexShaderConstantI) { DEVICE; return set_consts(d, 0, 1, M(1), M(2), M(3)); }
METHOD(dev_GetVertexShaderConstantI) { DEVICE; return get_consts(d, 0, 1, M(1), M(2), M(3)); }
METHOD(dev_SetVertexShaderConstantB) { DEVICE; return set_consts(d, 0, 2, M(1), M(2), M(3)); }
METHOD(dev_GetVertexShaderConstantB) { DEVICE; return get_consts(d, 0, 2, M(1), M(2), M(3)); }
METHOD(dev_SetPixelShaderConstantF) { DEVICE; return set_consts(d, 1, 0, M(1), M(2), M(3)); }
METHOD(dev_GetPixelShaderConstantF) { DEVICE; return get_consts(d, 1, 0, M(1), M(2), M(3)); }
METHOD(dev_SetPixelShaderConstantI) { DEVICE; return set_consts(d, 1, 1, M(1), M(2), M(3)); }
METHOD(dev_GetPixelShaderConstantI) { DEVICE; return get_consts(d, 1, 1, M(1), M(2), M(3)); }
METHOD(dev_SetPixelShaderConstantB) { DEVICE; return set_consts(d, 1, 2, M(1), M(2), M(3)); }
METHOD(dev_GetPixelShaderConstantB) { DEVICE; return get_consts(d, 1, 2, M(1), M(2), M(3)); }
METHOD(dev_SetStreamSource) {
    DEVICE; uint32_t n = M(1);
    if (n >= 16) return D3DERR_INVALIDCALL;
    VB *b = M(2) ? vb_of(M(2)) : NULL;
    if (M(2) && !b) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    REBIND(S->stream[n].vb, b);
    S->stream[n].off = M(3); S->stream[n].stride = M(4);
    if (d->rec) d->rec->mk.streams |= 1u << n;
    return D3D_OK;
}
METHOD(dev_GetStreamSource) {
    DEVICE; uint32_t n = M(1);
    if (n >= 16 || !M(2) || !M(3) || !M(4)) return D3DERR_INVALIDCALL;
    put_obj(M(2), d->st.stream[n].vb ? &d->st.stream[n].vb->o : NULL);
    wr32(M(3), d->st.stream[n].off); wr32(M(4), d->st.stream[n].stride);
    return D3D_OK;
}
METHOD(dev_SetStreamSourceFreq) {
    DEVICE; uint32_t n = M(1), v = M(2);
    if (n >= 16) return D3DERR_INVALIDCALL;
    if ((v & 0xC0000000u) == 0xC0000000u || (n == 0 && (v & 0x80000000u)) || !(v & 0x3FFFFFFFu)) return D3DERR_INVALIDCALL;
    State *S = wst(d); S->stream[n].freq = v;
    if (d->rec) d->rec->mk.freq |= 1u << n;
    return D3D_OK;
}
METHOD(dev_GetStreamSourceFreq) { DEVICE; if (M(1) >= 16 || !M(2)) return D3DERR_INVALIDCALL; wr32(M(2), d->st.stream[M(1)].freq); return D3D_OK; }
METHOD(dev_SetIndices) {
    DEVICE; VB *b = M(1) ? ib_of(M(1)) : NULL;
    if (M(1) && !b) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    REBIND(S->ib, b);
    if (d->rec) d->rec->mk.ib = 1;
    return D3D_OK;
}
METHOD(dev_GetIndices) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; put_obj(M(1), d->st.ib ? &d->st.ib->o : NULL); return D3D_OK; }
METHOD(dev_CreatePixelShader) { DEVICE; return create_shader(d, M(1), M(2), 1); }
METHOD(dev_SetPixelShader) {
    DEVICE; Shader *s = M(1) ? ps_of(M(1)) : NULL;
    if (M(1) && !s) return D3DERR_INVALIDCALL;
    State *S = wst(d);
    REBIND(S->ps, s);
    if (d->rec) d->rec->mk.ps = 1;
    changed(d);
    return D3D_OK;
}
METHOD(dev_GetPixelShader) { DEVICE; if (!M(1)) return D3DERR_INVALIDCALL; put_obj(M(1), d->st.ps ? &d->st.ps->o : NULL); return D3D_OK; }
METHOD(dev_DrawPatch) { LOG_ONCE("[d3d9] patches are not supported"); return D3DERR_INVALIDCALL; }
METHOD(dev_DeletePatch) { return D3DERR_INVALIDCALL; }
METHOD(dev_CreateQuery) {
    DEVICE; uint32_t type = M(1), out = M(2);
    if (type != QUERY_EVENT && type != QUERY_OCCLUSION) { if (out) wr32(out, 0); return D3DERR_NOTAVAILABLE; }
    if (!out) return D3D_OK;   // support check
    Query *q = calloc(1, sizeof *q);
    if (!q) { wr32(out, 0); return E_OUTOFMEMORY_; }
    q->type = type;
    obj_wrap(&q->o, &c_query, d, query_destroy);
    wr32(out, q->o.com.guest);
    return D3D_OK;
}

// ---------------- resources ----------------
#define OBJ Obj *o = (Obj *)self
METHOD(res_GetDevice) { OBJ; if (!M(1)) return D3DERR_INVALIDCALL; put_obj(M(1), &o->dev->o); return D3D_OK; }
#define MAX_PRIVATE_DATA (1u << 20)
METHOD(res_SetPrivateData) {
    OBJ; uint32_t g = M(1), data = M(2), size = M(3), flags = M(4);
    if (!g || (!data && size) || size > MAX_PRIVATE_DATA || ((flags & SPD_IUNKNOWN) && size != 4)) return D3DERR_INVALIDCALL;
    for (PrivData **pp = &o->pd; *pp; pp = &(*pp)->next)
        if (!memcmp((*pp)->guid, G2H(g), 16)) { PrivData *x = *pp; *pp = x->next; pd_free(x); break; }
    PrivData *p = calloc(1, sizeof *p);
    if (!p) return E_OUTOFMEMORY_;
    memcpy(p->guid, G2H(g), 16);
    p->flags = flags & SPD_IUNKNOWN; p->size = size;
    if (p->flags) { p->unk = data; if (data) guest_iunknown_call(data, 1); }
    else if (size) {
        p->data = malloc(size);
        if (!p->data) { free(p); return E_OUTOFMEMORY_; }
        memcpy(p->data, G2H(data), size);
    }
    p->next = o->pd; o->pd = p;
    return D3D_OK;
}
static PrivData *pd_find(Obj *o, uint32_t g) {
    if (!g) return NULL;
    for (PrivData *p = o->pd; p; p = p->next) if (!memcmp(p->guid, G2H(g), 16)) return p;
    return NULL;
}
METHOD(res_GetPrivateData) {
    OBJ; uint32_t data = M(2), psize = M(3);
    PrivData *p = pd_find(o, M(1));
    if (!p) return D3DERR_NOTFOUND;
    if (!psize) return D3DERR_INVALIDCALL;
    uint32_t have = rd32(psize);
    wr32(psize, p->size);
    if (!data) return D3D_OK;
    if (have < p->size) return D3DERR_MOREDATA;
    if (p->flags) { wr32(data, p->unk); if (p->unk) guest_iunknown_call(p->unk, 1); }
    else if (p->size) memcpy(G2H(data), p->data, p->size);
    return D3D_OK;
}
METHOD(res_FreePrivateData) {
    OBJ; uint32_t g = M(1);
    if (!g) return D3DERR_INVALIDCALL;
    for (PrivData **pp = &o->pd; *pp; pp = &(*pp)->next)
        if (!memcmp((*pp)->guid, G2H(g), 16)) { PrivData *x = *pp; *pp = x->next; pd_free(x); return D3D_OK; }
    return D3DERR_NOTFOUND;
}
METHOD(res_SetPriority) { OBJ; uint32_t old = o->priority; o->priority = M(1); return old; }
METHOD(res_GetPriority) { OBJ; return o->priority; }
METHOD(res_PreLoad) { return 0; }
METHOD(res_GetType) { OBJ; return (uint32_t)o->rtype; }

// ---- textures ----
#define TEX Tex *t = (Tex *)self; Device *d = t->o.dev
METHOD(tex_SetLOD) {
    TEX; (void)d;
    if (t->pool != POOL_MANAGED) return 0;
    uint32_t old = t->lod, l = M(1);
    t->lod = l < t->levels ? l : t->levels - 1;
    return old;
}
METHOD(tex_GetLOD) { TEX; (void)d; return t->pool == POOL_MANAGED ? t->lod : 0; }
METHOD(tex_GetLevelCount) { TEX; (void)d; return t->levels; }
METHOD(tex_SetAutoGenFilterType) {
    TEX; (void)d;
    if (M(1) == 0) return D3DERR_INVALIDCALL;
    t->filter = M(1);
    if (!(t->usage & USAGE_AUTOGENMIPMAP)) return D3D_OK;
    if (M(1) != 1 && M(1) != 2) LOG_ONCE("[d3d9] automatic mipmaps use a box filter (filter type %u requested)", M(1));
    return D3D_OK;
}
METHOD(tex_GetAutoGenFilterType) { TEX; (void)d; return t->filter; }
METHOD(tex_GenerateMipSubLevels) { TEX; if ((t->usage & USAGE_AUTOGENMIPMAP) && t->mtl) tex_gen_mips(d, t); return 0; }
static void surf_desc(uint32_t p, Tex *t, uint32_t w, uint32_t h, uint32_t rtype) {
    wr32(p, t->fmt); wr32(p + 4, rtype); wr32(p + 8, t->usage); wr32(p + 12, t->pool);
    wr32(p + 16, t->ms > 1 ? t->ms : 0); wr32(p + 20, t->msq); wr32(p + 24, w); wr32(p + 28, h);
}
static void vol_desc(uint32_t p, Tex *t, uint32_t level) {
    wr32(p, t->fmt); wr32(p + 4, RTYPE_VOLUME); wr32(p + 8, t->usage); wr32(p + 12, t->pool);
    wr32(p + 16, mip(t->w, level)); wr32(p + 20, mip(t->h, level)); wr32(p + 24, mip(t->d, level));
}
METHOD(tex_GetLevelDesc) {
    TEX; (void)d; uint32_t l = M(1), p = M(2);
    if (l >= t->levels || !p) return D3DERR_INVALIDCALL;
    if (t->volume) vol_desc(p, t, l);
    else surf_desc(p, t, mip(t->w, l), mip(t->h, l), RTYPE_SURFACE);
    return D3D_OK;
}
static uint32_t get_sub(Tex *t, uint32_t face, uint32_t level, uint32_t out) {
    if (!out) return D3DERR_INVALIDCALL;
    if (face >= t->faces || level >= t->levels) { wr32(out, 0); return D3DERR_INVALIDCALL; }
    Surf *s = tex_sub(t, face, level);
    if (!s) { wr32(out, 0); return E_OUTOFMEMORY_; }
    put_obj(out, &s->o);
    return D3D_OK;
}
METHOD(tex_GetSurfaceLevel) { TEX; (void)d; return get_sub(t, 0, M(1), M(2)); }
METHOD(cube_GetCubeMapSurface) { TEX; (void)d; return get_sub(t, M(1), M(2), M(3)); }
METHOD(voltex_GetVolumeLevel) { TEX; (void)d; return get_sub(t, 0, M(1), M(2)); }
static uint32_t lock_rect(Device *d, Tex *t, uint32_t face, uint32_t level, uint32_t out, uint32_t rp, uint32_t flags) {
    if (!out) return D3DERR_INVALIDCALL;
    int32_t r[4];
    if (rp) memcpy(r, G2H(rp), 16);
    return sub_lock(d, t, face, level, rp ? r : NULL, NULL, flags, out, 0);
}
static uint32_t lock_box(Device *d, Tex *t, uint32_t level, uint32_t out, uint32_t bp, uint32_t flags) {
    if (!out) return D3DERR_INVALIDCALL;
    int32_t b[6];
    if (bp) memcpy(b, G2H(bp), 24);
    return sub_lock(d, t, 0, level, NULL, bp ? b : NULL, flags, out, 1);
}
METHOD(tex_LockRect) { TEX; return lock_rect(d, t, 0, M(1), M(2), M(3), M(4)); }
METHOD(tex_UnlockRect) { TEX; return sub_unlock(d, t, 0, M(1)); }
METHOD(cube_LockRect) { TEX; return lock_rect(d, t, M(1), M(2), M(3), M(4), M(5)); }
METHOD(cube_UnlockRect) { TEX; return sub_unlock(d, t, M(1), M(2)); }
METHOD(voltex_LockBox) { TEX; return lock_box(d, t, M(1), M(2), M(3), M(4)); }
METHOD(voltex_UnlockBox) { TEX; return sub_unlock(d, t, 0, M(1)); }
METHOD(tex_AddDirty) { return D3D_OK; }

// ---- surfaces and volumes ----
#define SURF Surf *s = (Surf *)self; Device *d = s->o.dev
static int iid_in(const void *g, const Guid *const *ids, int n) {
    if (guid_eq(g, &IID_Unknown)) return 1;
    for (int i = 0; i < n; i++) if (guid_eq(g, ids[i])) return 1;
    return 0;
}
METHOD(surf_GetContainer) {
    SURF; uint32_t g = M(1), out = M(2);
    if (!g || !out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    const void *id = G2H(g);
    if (s->o.container) {
        Tex *t = s->tex;
        const Guid *kind = t->volume ? &IID_VolTex : t->cube ? &IID_Cube : &IID_Tex;
        const Guid *ids[] = {&IID_Resource, &IID_BaseTex, kind};
        if (!iid_in(id, ids, 3)) return E_NOINTERFACE_;
        put_obj(out, &t->o);
        return D3D_OK;
    }
    if (s->swap) {
        const Guid *ids[] = {&IID_Swap};
        if (!iid_in(id, ids, 1)) return E_NOINTERFACE_;
        put_obj(out, &s->swap->o);
        return D3D_OK;
    }
    const Guid *ids[] = {&IID_Device};
    if (!iid_in(id, ids, 1)) return E_NOINTERFACE_;
    put_obj(out, &d->o);
    return D3D_OK;
}
METHOD(surf_GetDesc) {
    SURF; (void)d;
    if (!M(1)) return D3DERR_INVALIDCALL;
    if (s->volume) vol_desc(M(1), s->tex, s->level);
    else surf_desc(M(1), s->tex, s->w, s->h, RTYPE_SURFACE);
    return D3D_OK;
}
METHOD(surf_LockRect) { SURF; return lock_rect(d, s->tex, s->face, s->level, M(1), M(2), M(3)); }
METHOD(surf_UnlockRect) { SURF; return sub_unlock(d, s->tex, s->face, s->level); }
METHOD(vol_LockBox) { SURF; return lock_box(d, s->tex, s->level, M(1), M(2), M(3)); }
METHOD(vol_UnlockBox) { SURF; return sub_unlock(d, s->tex, 0, s->level); }
METHOD(surf_GetDC) { LOG_ONCE("[d3d9] IDirect3DSurface9::GetDC is not supported"); if (M(1)) wr32(M(1), 0); return D3DERR_INVALIDCALL; }
METHOD(surf_ReleaseDC) { return D3DERR_INVALIDCALL; }

// ---- vertex and index buffers ----
METHOD(buf_Lock) {
    VB *b = (VB *)self; Device *d = b->o.dev;
    uint32_t off = M(1), size = M(2), out = M(3), flags = M(4);
    if (!out) return D3DERR_INVALIDCALL;
    if (off > b->len || (size && (uint64_t)off + size > b->len)) { wr32(out, 0); return D3DERR_INVALIDCALL; }
    // storage the GPU may still read is replaced (DISCARD: new contents; otherwise a copy) unless NOOVERWRITE or READONLY
    if (!(flags & (LOCK_NOOVERWRITE | LOCK_READONLY)) && b->last_use && b->last_use > done_serial(d)) {
        Store n;
        if (!store_alloc(d, b->len, &n)) { wr32(out, 0); return D3DERR_OUTOFVIDEOMEMORY; }
        if (!(flags & LOCK_DISCARD)) memcpy(G2H(n.g), G2H(b->st.g), b->len);
        store_free(d, &b->st, b->last_use);
        b->st = n;
        b->last_use = 0;
    }
    b->nlocked++;
    wr32(out, b->st.g + off);
    return D3D_OK;
}
METHOD(buf_Unlock) { VB *b = (VB *)self; if (b->nlocked > 0) b->nlocked--; return D3D_OK; }
METHOD(vb_GetDesc) {
    VB *b = (VB *)self; uint32_t p = M(1);
    if (!p) return D3DERR_INVALIDCALL;
    wr32(p, FMT_VERTEXDATA); wr32(p + 4, RTYPE_VERTEXBUFFER); wr32(p + 8, b->usage); wr32(p + 12, b->pool); wr32(p + 16, b->len); wr32(p + 20, b->fvf);
    return D3D_OK;
}
METHOD(ib_GetDesc) {
    VB *b = (VB *)self; uint32_t p = M(1);
    if (!p) return D3DERR_INVALIDCALL;
    wr32(p, b->fmt); wr32(p + 4, RTYPE_INDEXBUFFER); wr32(p + 8, b->usage); wr32(p + 12, b->pool); wr32(p + 16, b->len);
    return D3D_OK;
}

// ---- declarations and shaders ----
METHOD(decl_GetDeclaration) {
    Decl *dc = (Decl *)self; uint32_t p = M(1), pn = M(2);
    if (!pn) return D3DERR_INVALIDCALL;
    if (p) {
        for (uint32_t i = 0; i <= dc->n; i++) {
            uint8_t *e = G2H(p + 8 * i);
            if (i == dc->n) { static const uint8_t end[8] = {0xFF, 0, 0, 0, DT_UNUSED, 0, 0, 0}; memcpy(e, end, 8); break; }
            memcpy(e, &dc->el[i].stream, 2); memcpy(e + 2, &dc->el[i].offset, 2);
            e[4] = dc->el[i].type; e[5] = dc->el[i].method; e[6] = dc->el[i].usage; e[7] = dc->el[i].index;
        }
    }
    wr32(pn, dc->n + 1);
    return D3D_OK;
}
METHOD(shader_GetFunction) {
    Shader *s = (Shader *)self; uint32_t p = M(1), ps = M(2);
    if (!ps) return D3DERR_INVALIDCALL;
    uint32_t size = s->ntok * 4;
    if (p) {
        if (rd32(ps) < size) return D3DERR_INVALIDCALL;
        memcpy(G2H(p), s->tok, size);
    }
    wr32(ps, size);
    return D3D_OK;
}

// ---- queries ----
static void occl_end(Device *d, Query *q) {
    if (d->enc && d->occl == q) [d->enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    q->active = 0;
    if (d->occl == q) d->occl = NULL;
    q->serial = q->nslots ? cur_serial(d) : 0;
    q->issued = 1;
}
METHOD(q_GetType) { return ((Query *)self)->type; }
METHOD(q_GetDataSize) { return 4; }
METHOD(q_Issue) {
    Query *q = (Query *)self; Device *d = q->o.dev; uint32_t f = M(1);
    if (q->type == QUERY_EVENT) {
        if (f & 1) { q->serial = cur_serial(d); q->issued = 1; }
        return D3D_OK;
    }
    if (f & 2) {   // D3DISSUE_BEGIN
        if (d->occl && d->occl != q) occl_end(d, d->occl);
        for (uint32_t k = 0; k < q->nslots; k++) d->vis_owner[q->slots[k]] = NULL;
        q->nslots = 0; q->acc = 0; q->issued = 0; q->active = 1;
        d->occl = q;
        if (d->enc) {
            uint32_t slot = vis_slot(d);
            if (d->enc && d->occl == q) {
                [d->enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:slot * 8];
                query_add_slot(d, q, slot);
            }
        }
    } else if (f & 1) {   // D3DISSUE_END
        if (q->active) occl_end(d, q);
        else { q->serial = 0; q->issued = 1; q->nslots = 0; q->acc = 0; }
    }
    return D3D_OK;
}
METHOD(q_GetData) {
    Query *q = (Query *)self; Device *d = q->o.dev; uint32_t p = M(1), size = M(2);
    if (q->active) return S_FALSE_;
    if (q->issued && q->serial > done_serial(d)) {
        if (d->cmd && q->serial >= d->serial) commit(d);
        return S_FALSE_;
    }
    if (p && size >= 4) {
        if (q->type == QUERY_EVENT) wr32(p, 1);
        else {
            uint64_t n = q->acc;
            for (uint32_t k = 0; k < q->nslots; k++) n += ((uint64_t *)d->vis.contents)[q->slots[k]];
            wr32(p, n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n);
        }
    }
    return D3D_OK;
}

// ---- swap chains ----
#define SWAP Swap *sw = (Swap *)self; Device *d = sw->o.dev
METHOD(swap_Present) { SWAP; uint32_t r = swap_present(d, sw, M(1), M(2), M(3)); rt_pause_point(); return r; }
METHOD(swap_GetFrontBufferData) { SWAP; Surf *t = surf_of(M(1)); if (!t) return D3DERR_INVALIDCALL; return front_data(d, sw, t); }
METHOD(swap_GetBackBuffer) { SWAP; (void)d; return get_bb(sw, M(1), M(3)); }
METHOD(swap_GetRasterStatus) { SWAP; (void)d; if (!M(1)) return D3DERR_INVALIDCALL; raster_status(sw, M(1)); return D3D_OK; }
METHOD(swap_GetDisplayMode) { SWAP; if (!M(1)) return D3DERR_INVALIDCALL; display_mode(d, sw, M(1)); return D3D_OK; }
METHOD(swap_GetPresentParameters) {
    SWAP; (void)d;
    if (!M(1)) return D3DERR_INVALIDCALL;
    for (int i = 0; i < 14; i++) wr32(M(1) + 4u * (uint32_t)i, sw->pp[i]);
    return D3D_OK;
}

// ---------------- interface tables (vtable order of d3d9.h) ----------------
#define MD(name, n, fn) {name, n, fn, 0, NULL, 0}
#define UNKNOWN(qif) MD("QueryInterface", 3, qif), MD("AddRef", 1, m_AddRef), MD("Release", 1, m_Release)
#define RESOURCE MD("GetDevice", 2, res_GetDevice), MD("SetPrivateData", 5, res_SetPrivateData), \
    MD("GetPrivateData", 4, res_GetPrivateData), MD("FreePrivateData", 2, res_FreePrivateData), \
    MD("SetPriority", 2, res_SetPriority), MD("GetPriority", 1, res_GetPriority), MD("PreLoad", 1, res_PreLoad), \
    MD("GetType", 1, res_GetType)
#define BASETEX MD("SetLOD", 2, tex_SetLOD), MD("GetLOD", 1, tex_GetLOD), MD("GetLevelCount", 1, tex_GetLevelCount), \
    MD("SetAutoGenFilterType", 2, tex_SetAutoGenFilterType), MD("GetAutoGenFilterType", 1, tex_GetAutoGenFilterType), \
    MD("GenerateMipSubLevels", 1, tex_GenerateMipSubLevels)
static const ComMethod d3d9_m[] = {
    UNKNOWN(qi_d3d9),
    MD("RegisterSoftwareDevice", 2, d3d_RegisterSoftwareDevice), MD("GetAdapterCount", 1, d3d_GetAdapterCount),
    MD("GetAdapterIdentifier", 4, d3d_GetAdapterIdentifier), MD("GetAdapterModeCount", 3, d3d_GetAdapterModeCount),
    MD("EnumAdapterModes", 5, d3d_EnumAdapterModes), MD("GetAdapterDisplayMode", 3, d3d_GetAdapterDisplayMode),
    MD("CheckDeviceType", 6, d3d_CheckDeviceType), MD("CheckDeviceFormat", 7, d3d_CheckDeviceFormat),
    MD("CheckDeviceMultiSampleType", 7, d3d_CheckDeviceMultiSampleType), MD("CheckDepthStencilMatch", 6, d3d_CheckDepthStencilMatch),
    MD("CheckDeviceFormatConversion", 5, d3d_CheckDeviceFormatConversion), MD("GetDeviceCaps", 4, d3d_GetDeviceCaps),
    MD("GetAdapterMonitor", 2, d3d_GetAdapterMonitor), MD("CreateDevice", 7, d3d_CreateDevice),
};
static const ComMethod device_m[] = {
    UNKNOWN(qi_device),
    MD("TestCooperativeLevel", 1, dev_TestCooperativeLevel), MD("GetAvailableTextureMem", 1, dev_GetAvailableTextureMem),
    MD("EvictManagedResources", 1, dev_EvictManagedResources), MD("GetDirect3D", 2, dev_GetDirect3D),
    MD("GetDeviceCaps", 2, dev_GetDeviceCaps), MD("GetDisplayMode", 3, dev_GetDisplayMode),
    MD("GetCreationParameters", 2, dev_GetCreationParameters), MD("SetCursorProperties", 4, dev_SetCursorProperties),
    MD("SetCursorPosition", 4, dev_SetCursorPosition), MD("ShowCursor", 2, dev_ShowCursor),
    MD("CreateAdditionalSwapChain", 3, dev_CreateAdditionalSwapChain), MD("GetSwapChain", 3, dev_GetSwapChain),
    MD("GetNumberOfSwapChains", 1, dev_GetNumberOfSwapChains), MD("Reset", 2, dev_Reset), MD("Present", 5, dev_Present),
    MD("GetBackBuffer", 5, dev_GetBackBuffer), MD("GetRasterStatus", 3, dev_GetRasterStatus),
    MD("SetDialogBoxMode", 2, dev_SetDialogBoxMode), MD("SetGammaRamp", 4, dev_SetGammaRamp), MD("GetGammaRamp", 3, dev_GetGammaRamp),
    MD("CreateTexture", 9, dev_CreateTexture), MD("CreateVolumeTexture", 10, dev_CreateVolumeTexture),
    MD("CreateCubeTexture", 8, dev_CreateCubeTexture), MD("CreateVertexBuffer", 7, dev_CreateVertexBuffer),
    MD("CreateIndexBuffer", 7, dev_CreateIndexBuffer), MD("CreateRenderTarget", 9, dev_CreateRenderTarget),
    MD("CreateDepthStencilSurface", 9, dev_CreateDepthStencilSurface), MD("UpdateSurface", 5, dev_UpdateSurface),
    MD("UpdateTexture", 3, dev_UpdateTexture), MD("GetRenderTargetData", 3, dev_GetRenderTargetData),
    MD("GetFrontBufferData", 3, dev_GetFrontBufferData), MD("StretchRect", 6, dev_StretchRect), MD("ColorFill", 4, dev_ColorFill),
    MD("CreateOffscreenPlainSurface", 7, dev_CreateOffscreenPlainSurface), MD("SetRenderTarget", 3, dev_SetRenderTarget),
    MD("GetRenderTarget", 3, dev_GetRenderTarget), MD("SetDepthStencilSurface", 2, dev_SetDepthStencilSurface),
    MD("GetDepthStencilSurface", 2, dev_GetDepthStencilSurface), MD("BeginScene", 1, dev_BeginScene), MD("EndScene", 1, dev_EndScene),
    MD("Clear", 7, dev_Clear), MD("SetTransform", 3, dev_SetTransform), MD("GetTransform", 3, dev_GetTransform),
    MD("MultiplyTransform", 3, dev_MultiplyTransform), MD("SetViewport", 2, dev_SetViewport), MD("GetViewport", 2, dev_GetViewport),
    MD("SetMaterial", 2, dev_SetMaterial), MD("GetMaterial", 2, dev_GetMaterial), MD("SetLight", 3, dev_SetLight),
    MD("GetLight", 3, dev_GetLight), MD("LightEnable", 3, dev_LightEnable), MD("GetLightEnable", 3, dev_GetLightEnable),
    MD("SetClipPlane", 3, dev_SetClipPlane), MD("GetClipPlane", 3, dev_GetClipPlane), MD("SetRenderState", 3, dev_SetRenderState),
    MD("GetRenderState", 3, dev_GetRenderState), MD("CreateStateBlock", 3, dev_CreateStateBlock),
    MD("BeginStateBlock", 1, dev_BeginStateBlock), MD("EndStateBlock", 2, dev_EndStateBlock), MD("SetClipStatus", 2, dev_SetClipStatus),
    MD("GetClipStatus", 2, dev_GetClipStatus), MD("GetTexture", 3, dev_GetTexture), MD("SetTexture", 3, dev_SetTexture),
    MD("GetTextureStageState", 4, dev_GetTextureStageState), MD("SetTextureStageState", 4, dev_SetTextureStageState),
    MD("GetSamplerState", 4, dev_GetSamplerState), MD("SetSamplerState", 4, dev_SetSamplerState),
    MD("ValidateDevice", 2, dev_ValidateDevice), MD("SetPaletteEntries", 3, dev_SetPaletteEntries),
    MD("GetPaletteEntries", 3, dev_GetPaletteEntries), MD("SetCurrentTexturePalette", 2, dev_SetCurrentTexturePalette),
    MD("GetCurrentTexturePalette", 2, dev_GetCurrentTexturePalette), MD("SetScissorRect", 2, dev_SetScissorRect),
    MD("GetScissorRect", 2, dev_GetScissorRect), MD("SetSoftwareVertexProcessing", 2, dev_SetSoftwareVertexProcessing),
    MD("GetSoftwareVertexProcessing", 1, dev_GetSoftwareVertexProcessing), MD("SetNPatchMode", 2, dev_SetNPatchMode),
    MD("GetNPatchMode", 1, dev_GetNPatchMode), MD("DrawPrimitive", 4, dev_DrawPrimitive),
    MD("DrawIndexedPrimitive", 7, dev_DrawIndexedPrimitive), MD("DrawPrimitiveUP", 5, dev_DrawPrimitiveUP),
    MD("DrawIndexedPrimitiveUP", 9, dev_DrawIndexedPrimitiveUP), MD("ProcessVertices", 7, dev_ProcessVertices),
    MD("CreateVertexDeclaration", 3, dev_CreateVertexDeclaration), MD("SetVertexDeclaration", 2, dev_SetVertexDeclaration),
    MD("GetVertexDeclaration", 2, dev_GetVertexDeclaration), MD("SetFVF", 2, dev_SetFVF), MD("GetFVF", 2, dev_GetFVF),
    MD("CreateVertexShader", 3, dev_CreateVertexShader), MD("SetVertexShader", 2, dev_SetVertexShader),
    MD("GetVertexShader", 2, dev_GetVertexShader), MD("SetVertexShaderConstantF", 4, dev_SetVertexShaderConstantF),
    MD("GetVertexShaderConstantF", 4, dev_GetVertexShaderConstantF), MD("SetVertexShaderConstantI", 4, dev_SetVertexShaderConstantI),
    MD("GetVertexShaderConstantI", 4, dev_GetVertexShaderConstantI), MD("SetVertexShaderConstantB", 4, dev_SetVertexShaderConstantB),
    MD("GetVertexShaderConstantB", 4, dev_GetVertexShaderConstantB), MD("SetStreamSource", 5, dev_SetStreamSource),
    MD("GetStreamSource", 5, dev_GetStreamSource), MD("SetStreamSourceFreq", 3, dev_SetStreamSourceFreq),
    MD("GetStreamSourceFreq", 3, dev_GetStreamSourceFreq), MD("SetIndices", 2, dev_SetIndices), MD("GetIndices", 2, dev_GetIndices),
    MD("CreatePixelShader", 3, dev_CreatePixelShader), MD("SetPixelShader", 2, dev_SetPixelShader),
    MD("GetPixelShader", 2, dev_GetPixelShader), MD("SetPixelShaderConstantF", 4, dev_SetPixelShaderConstantF),
    MD("GetPixelShaderConstantF", 4, dev_GetPixelShaderConstantF), MD("SetPixelShaderConstantI", 4, dev_SetPixelShaderConstantI),
    MD("GetPixelShaderConstantI", 4, dev_GetPixelShaderConstantI), MD("SetPixelShaderConstantB", 4, dev_SetPixelShaderConstantB),
    MD("GetPixelShaderConstantB", 4, dev_GetPixelShaderConstantB), MD("DrawRectPatch", 4, dev_DrawPatch),
    MD("DrawTriPatch", 4, dev_DrawPatch), MD("DeletePatch", 2, dev_DeletePatch), MD("CreateQuery", 3, dev_CreateQuery),
};
static const ComMethod texture_m[] = { UNKNOWN(qi_texture), RESOURCE, BASETEX,
    MD("GetLevelDesc", 3, tex_GetLevelDesc), MD("GetSurfaceLevel", 3, tex_GetSurfaceLevel), MD("LockRect", 5, tex_LockRect),
    MD("UnlockRect", 2, tex_UnlockRect), MD("AddDirtyRect", 2, tex_AddDirty) };
static const ComMethod cube_m[] = { UNKNOWN(qi_cube), RESOURCE, BASETEX,
    MD("GetLevelDesc", 3, tex_GetLevelDesc), MD("GetCubeMapSurface", 4, cube_GetCubeMapSurface), MD("LockRect", 6, cube_LockRect),
    MD("UnlockRect", 3, cube_UnlockRect), MD("AddDirtyRect", 3, tex_AddDirty) };
static const ComMethod voltex_m[] = { UNKNOWN(qi_voltex), RESOURCE, BASETEX,
    MD("GetLevelDesc", 3, tex_GetLevelDesc), MD("GetVolumeLevel", 3, voltex_GetVolumeLevel), MD("LockBox", 5, voltex_LockBox),
    MD("UnlockBox", 2, voltex_UnlockBox), MD("AddDirtyBox", 2, tex_AddDirty) };
static const ComMethod volume_m[] = { UNKNOWN(qi_volume), MD("GetDevice", 2, res_GetDevice),
    MD("SetPrivateData", 5, res_SetPrivateData), MD("GetPrivateData", 4, res_GetPrivateData),
    MD("FreePrivateData", 2, res_FreePrivateData), MD("GetContainer", 3, surf_GetContainer), MD("GetDesc", 2, surf_GetDesc),
    MD("LockBox", 4, vol_LockBox), MD("UnlockBox", 1, vol_UnlockBox) };
static const ComMethod surface_m[] = { UNKNOWN(qi_surface), RESOURCE,
    MD("GetContainer", 3, surf_GetContainer), MD("GetDesc", 2, surf_GetDesc), MD("LockRect", 4, surf_LockRect),
    MD("UnlockRect", 1, surf_UnlockRect), MD("GetDC", 2, surf_GetDC), MD("ReleaseDC", 2, surf_ReleaseDC) };
static const ComMethod vb_m[] = { UNKNOWN(qi_vb), RESOURCE, MD("Lock", 5, buf_Lock), MD("Unlock", 1, buf_Unlock), MD("GetDesc", 2, vb_GetDesc) };
static const ComMethod ib_m[] = { UNKNOWN(qi_ib), RESOURCE, MD("Lock", 5, buf_Lock), MD("Unlock", 1, buf_Unlock), MD("GetDesc", 2, ib_GetDesc) };
static const ComMethod decl_m[] = { UNKNOWN(qi_decl), MD("GetDevice", 2, res_GetDevice), MD("GetDeclaration", 3, decl_GetDeclaration) };
static const ComMethod vs_m[] = { UNKNOWN(qi_vs), MD("GetDevice", 2, res_GetDevice), MD("GetFunction", 3, shader_GetFunction) };
static const ComMethod ps_m[] = { UNKNOWN(qi_ps), MD("GetDevice", 2, res_GetDevice), MD("GetFunction", 3, shader_GetFunction) };
static const ComMethod query_m[] = { UNKNOWN(qi_query), MD("GetDevice", 2, res_GetDevice), MD("GetType", 1, q_GetType),
    MD("GetDataSize", 1, q_GetDataSize), MD("Issue", 2, q_Issue), MD("GetData", 4, q_GetData) };
static const ComMethod sb_m[] = { UNKNOWN(qi_sb), MD("GetDevice", 2, res_GetDevice), MD("Capture", 1, sb_Capture), MD("Apply", 1, sb_Apply) };
static const ComMethod swap_m[] = { UNKNOWN(qi_swap), MD("Present", 6, swap_Present), MD("GetFrontBufferData", 2, swap_GetFrontBufferData),
    MD("GetBackBuffer", 4, swap_GetBackBuffer), MD("GetRasterStatus", 2, swap_GetRasterStatus),
    MD("GetDisplayMode", 2, swap_GetDisplayMode), MD("GetDevice", 2, res_GetDevice),
    MD("GetPresentParameters", 2, swap_GetPresentParameters) };
#define CLS(var, name, tbl) static ComClass var = {name, (int)(sizeof tbl / sizeof tbl[0]), tbl, 0, NULL}
CLS(c_d3d9, "IDirect3D9", d3d9_m);
CLS(c_device, "IDirect3DDevice9", device_m);
CLS(c_texture, "IDirect3DTexture9", texture_m);
CLS(c_cube, "IDirect3DCubeTexture9", cube_m);
CLS(c_voltex, "IDirect3DVolumeTexture9", voltex_m);
CLS(c_volume, "IDirect3DVolume9", volume_m);
CLS(c_surface, "IDirect3DSurface9", surface_m);
CLS(c_vb, "IDirect3DVertexBuffer9", vb_m);
CLS(c_ib, "IDirect3DIndexBuffer9", ib_m);
CLS(c_decl, "IDirect3DVertexDeclaration9", decl_m);
CLS(c_vs, "IDirect3DVertexShader9", vs_m);
CLS(c_ps, "IDirect3DPixelShader9", ps_m);
CLS(c_query, "IDirect3DQuery9", query_m);
CLS(c_sb, "IDirect3DStateBlock9", sb_m);
CLS(c_swap, "IDirect3DSwapChain9", swap_m);
_Static_assert(sizeof device_m / sizeof device_m[0] == 119, "IDirect3DDevice9 has 119 methods");
_Static_assert(sizeof d3d9_m / sizeof d3d9_m[0] == 17, "IDirect3D9 has 17 methods");
_Static_assert(sizeof texture_m / sizeof texture_m[0] == 22, "IDirect3DTexture9 has 22 methods");
_Static_assert(sizeof surface_m / sizeof surface_m[0] == 17, "IDirect3DSurface9 has 17 methods");
_Static_assert(sizeof vb_m / sizeof vb_m[0] == 14, "IDirect3DVertexBuffer9 has 14 methods");
_Static_assert(sizeof volume_m / sizeof volume_m[0] == 11, "IDirect3DVolume9 has 11 methods");
_Static_assert(sizeof swap_m / sizeof swap_m[0] == 10, "IDirect3DSwapChain9 has 10 methods");

// ---------------- exports ----------------
SHIM(d3d9_Direct3DCreate9, 1) {
    pthread_once(&g_once, g_init);
    uint32_t r = 0;
    pthread_mutex_lock(&g_mu);
    @autoreleasepool {
        if (!rt_metal_device()) rt_log("[d3d9] Direct3DCreate9: this Mac has no Metal device");
        else {
            D3D9 *o = calloc(1, sizeof *o);
            if (o) r = obj_wrap(&o->o, &c_d3d9, NULL, d3d9_destroy);
        }
    }
    pthread_mutex_unlock(&g_mu);
    rt_log("[d3d9] Direct3DCreate9(%u) -> 0x%x (Metal)", ARG(0), r);
    return r;
}
SHIM(d3d9_D3DPERF_GetStatus, 0) { return 0; }
SHIM(d3d9_D3DPERF_SetOptions, 1) { return 0; }
