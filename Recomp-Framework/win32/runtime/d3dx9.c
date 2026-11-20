// D3DX 9 (d3dx9_NN.dll) for 32-bit guests: the math helpers and texture creation.
//
// Textures are made through the guest-visible IDirect3DDevice9 (CreateTexture, LockRect, UnlockRect, UpdateTexture),
// so they work with whichever d3d9 backend the device is. Image files: DDS (DXT1-5, uncompressed RGB/luminance/
// alpha masks; the first face or slice of cube and volume files), TGA (types 1, 2, 3, 9, 10, 11) and BMP (1/4/8-bit
// palettes, 16/24/32-bit, BI_RGB and BI_BITFIELDS). The D3DX_DEFAULT rules follow the documented D3DX behaviour:
// sizes from the file rounded up to powers of two, a full mip chain built with a box filter, the format from the
// file. DXT targets are encoded on the CPU when the file's blocks can't be copied as they are.
//
// Not implemented: JPG/PNG/HDR/PPM/DIB files, palettized (P8) textures, dithering (D3DX_FILTER_DITHER is ignored).
#include "com32.h"
#include <math.h>

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DXERR_INVALIDDATA 0x88760B59u
#define E_OUTOFMEMORY 0x8007000Eu
#define D3DX_DEFAULT 0xFFFFFFFFu
#define D3DX_DEFAULT_NONPOW2 0xFFFFFFFEu
#define D3DX_FROM_FILE 0xFFFFFFFDu
#define FILTER_NONE 1
#define FILTER_POINT 2
#define FILTER_LINEAR 3
#define FILTER_TRIANGLE 4
#define FILTER_BOX 5
#define POOL_DEFAULT 0
#define POOL_SYSTEMMEM 2
#define USAGE_DYNAMIC 0x200u

enum { F_UNKNOWN = 0, F_R8G8B8 = 20, F_A8R8G8B8 = 21, F_X8R8G8B8 = 22, F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25,
       F_A4R4G4B4 = 26, F_A8 = 28, F_X4R4G4B4 = 30, F_A8B8G8R8 = 32, F_X8B8G8R8 = 33, F_P8 = 41, F_L8 = 50, F_A8L8 = 51 };
#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define F_DXT1 FCC('D', 'X', 'T', '1')
#define F_DXT2 FCC('D', 'X', 'T', '2')
#define F_DXT3 FCC('D', 'X', 'T', '3')
#define F_DXT4 FCC('D', 'X', 'T', '4')
#define F_DXT5 FCC('D', 'X', 'T', '5')
static int is_dxt(uint32_t f) { return f == F_DXT1 || f == F_DXT2 || f == F_DXT3 || f == F_DXT4 || f == F_DXT5; }
static uint32_t block_bytes(uint32_t f) { return f == F_DXT1 ? 8 : 16; }
static int fmt_bpp(uint32_t f) {   // bytes per pixel of the uncompressed targets; 0 unsupported
    switch (f) {
    case F_A8R8G8B8: case F_X8R8G8B8: case F_A8B8G8R8: case F_X8B8G8R8: return 4;
    case F_R8G8B8: return 3;
    case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_A4R4G4B4: case F_X4R4G4B4: case F_A8L8: return 2;
    case F_A8: case F_L8: return 1;
    default: return 0;
    }
}

// guest COM call
static uint32_t vc(CPU *c, uint32_t obj, int slot, int n, const uint32_t *args) {
    uint32_t a[10];
    a[0] = obj;
    for (int i = 0; i < n && i < 9; i++) a[i + 1] = args[i];
    return guest_call(c, rd32(rd32(obj) + 4u * (uint32_t)slot), n + 1, a);
}
enum { DEV_CreateTexture = 23, DEV_UpdateTexture = 31, TEX_Release = 2, TEX_GetLevelCount = 13, TEX_LockRect = 19, TEX_UnlockRect = 20 };

// ---------------- math ----------------
typedef struct { float m[4][4]; } Mat;
static Mat rdmat(uint32_t a) { Mat r; for (int i = 0; i < 16; i++) r.m[i / 4][i % 4] = rdf(a + 4u * (uint32_t)i); return r; }
static void wrmat(uint32_t a, const Mat *x) { for (int i = 0; i < 16; i++) { float f = x->m[i / 4][i % 4]; uint32_t u; memcpy(&u, &f, 4); wr32(a + 4u * (uint32_t)i, u); } }
static void wrfl(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(a, u); }
static Mat mul(const Mat *a, const Mat *b) {
    Mat r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    return r;
}
static Mat ident(void) { Mat r = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}; return r; }
typedef struct { float x, y, z; } V3;
static V3 rdv3(uint32_t a) { return (V3){rdf(a), rdf(a + 4), rdf(a + 8)}; }
static V3 sub3(V3 a, V3 b) { return (V3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 cross3(V3 a, V3 b) { return (V3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm3(V3 a) { float l = sqrtf(dot3(a, a)); return l ? (V3){a.x / l, a.y / l, a.z / l} : (V3){0, 0, 0}; }

SHIM(d3dx9_28_D3DXMatrixMultiply, 3) {   // (out, m1, m2): out = m1 * m2
    Mat a = rdmat(ARG(1)), b = rdmat(ARG(2)), r = mul(&a, &b);
    wrmat(ARG(0), &r);
    return ARG(0);
}
SHIM(d3dx9_28_D3DXMatrixTranspose, 2) {
    Mat a = rdmat(ARG(1)), r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = a.m[j][i];
    wrmat(ARG(0), &r);
    return ARG(0);
}
SHIM(d3dx9_28_D3DXMatrixTranslation, 4) {
    Mat r = ident();
    r.m[3][0] = ARGF(1); r.m[3][1] = ARGF(2); r.m[3][2] = ARGF(3);
    wrmat(ARG(0), &r);
    return ARG(0);
}
// (out, *determinant or NULL, m): NULL when m is singular (out untouched)
SHIM(d3dx9_28_D3DXMatrixInverse, 3) {
    Mat a = rdmat(ARG(2));
    const float *m = &a.m[0][0];
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (ARG(1)) wrfl(ARG(1), det);
    if (det == 0.0f) return 0;
    Mat r;
    for (int i = 0; i < 16; i++) r.m[i / 4][i % 4] = inv[i] / det;
    wrmat(ARG(0), &r);
    return ARG(0);
}
static uint32_t look_at(uint32_t out, uint32_t eye_a, uint32_t at_a, uint32_t up_a, int rh) {
    V3 eye = rdv3(eye_a), at = rdv3(at_a), up = rdv3(up_a);
    V3 z = norm3(rh ? sub3(eye, at) : sub3(at, eye)), x = norm3(cross3(up, z)), y = cross3(z, x);
    Mat r = {{{x.x, y.x, z.x, 0}, {x.y, y.y, z.y, 0}, {x.z, y.z, z.z, 0}, {-dot3(x, eye), -dot3(y, eye), -dot3(z, eye), 1}}};
    wrmat(out, &r);
    return out;
}
SHIM(d3dx9_28_D3DXMatrixLookAtLH, 4) { return look_at(ARG(0), ARG(1), ARG(2), ARG(3), 0); }
SHIM(d3dx9_28_D3DXMatrixLookAtRH, 4) { return look_at(ARG(0), ARG(1), ARG(2), ARG(3), 1); }
static uint32_t ortho(uint32_t out, float w, float h, float zn, float zf, int rh) {
    Mat r = ident();
    r.m[0][0] = 2 / w; r.m[1][1] = 2 / h;
    r.m[2][2] = rh ? 1 / (zn - zf) : 1 / (zf - zn);
    r.m[3][2] = zn / (zn - zf);
    wrmat(out, &r);
    return out;
}
SHIM(d3dx9_28_D3DXMatrixOrthoLH, 5) { return ortho(ARG(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4), 0); }
SHIM(d3dx9_28_D3DXMatrixOrthoRH, 5) { return ortho(ARG(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4), 1); }
SHIM(d3dx9_28_D3DXMatrixPerspectiveFovRH, 5) {   // (out, fovy, aspect, zn, zf)
    float ys = 1.0f / tanf(ARGF(1) / 2), xs = ys / ARGF(2), zn = ARGF(3), zf = ARGF(4);
    Mat r = {{{xs, 0, 0, 0}, {0, ys, 0, 0}, {0, 0, zf / (zn - zf), -1}, {0, 0, zn * zf / (zn - zf), 0}}};
    wrmat(ARG(0), &r);
    return ARG(0);
}
SHIM(d3dx9_28_D3DXMatrixRotationYawPitchRoll, 4) {   // roll (z), then pitch (x), then yaw (y)
    float y = ARGF(1), p = ARGF(2), ro = ARGF(3);
    float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(ro), sr = sinf(ro);
    Mat rz = {{{cr, sr, 0, 0}, {-sr, cr, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    Mat rx = {{{1, 0, 0, 0}, {0, cp, sp, 0}, {0, -sp, cp, 0}, {0, 0, 0, 1}}};
    Mat ry = {{{cy, 0, -sy, 0}, {0, 1, 0, 0}, {sy, 0, cy, 0}, {0, 0, 0, 1}}};
    Mat t = mul(&rz, &rx), r = mul(&t, &ry);
    wrmat(ARG(0), &r);
    return ARG(0);
}
static void plane_norm(float p[4]) {
    float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (l) { p[0] /= l; p[1] /= l; p[2] /= l; p[3] /= l; } else p[0] = p[1] = p[2] = p[3] = 0;
}
SHIM(d3dx9_28_D3DXMatrixReflect, 2) {   // (out, plane): reflection about the normalized plane
    float p[4] = {rdf(ARG(1)), rdf(ARG(1) + 4), rdf(ARG(1) + 8), rdf(ARG(1) + 12)};
    plane_norm(p);
    float pa = p[0], pb = p[1], pc = p[2], pd = p[3];
    Mat r = {{{-2 * pa * pa + 1, -2 * pb * pa, -2 * pc * pa, 0}, {-2 * pa * pb, -2 * pb * pb + 1, -2 * pc * pb, 0},
              {-2 * pa * pc, -2 * pb * pc, -2 * pc * pc + 1, 0}, {-2 * pa * pd, -2 * pb * pd, -2 * pc * pd, 1}}};
    wrmat(ARG(0), &r);
    return ARG(0);
}
SHIM(d3dx9_28_D3DXPlaneNormalize, 2) {
    float p[4] = {rdf(ARG(1)), rdf(ARG(1) + 4), rdf(ARG(1) + 8), rdf(ARG(1) + 12)};
    plane_norm(p);
    for (int i = 0; i < 4; i++) wrfl(ARG(0) + 4u * (uint32_t)i, p[i]);
    return ARG(0);
}
static void vec4_mul(uint32_t out, const float v[4], uint32_t m_a) {
    Mat m = rdmat(m_a);
    float r[4];
    for (int j = 0; j < 4; j++) r[j] = v[0] * m.m[0][j] + v[1] * m.m[1][j] + v[2] * m.m[2][j] + v[3] * m.m[3][j];
    for (int j = 0; j < 4; j++) wrfl(out + 4u * (uint32_t)j, r[j]);
}
SHIM(d3dx9_28_D3DXPlaneTransform, 3) {
    float v[4] = {rdf(ARG(1)), rdf(ARG(1) + 4), rdf(ARG(1) + 8), rdf(ARG(1) + 12)};
    vec4_mul(ARG(0), v, ARG(2));
    return ARG(0);
}
SHIM(d3dx9_28_D3DXVec4Transform, 3) {
    float v[4] = {rdf(ARG(1)), rdf(ARG(1) + 4), rdf(ARG(1) + 8), rdf(ARG(1) + 12)};
    vec4_mul(ARG(0), v, ARG(2));
    return ARG(0);
}
SHIM(d3dx9_28_D3DXVec3Transform, 3) {   // (D3DXVECTOR4 *out, const D3DXVECTOR3 *v, m): (x, y, z, 1) * m
    float v[4] = {rdf(ARG(1)), rdf(ARG(1) + 4), rdf(ARG(1) + 8), 1};
    vec4_mul(ARG(0), v, ARG(2));
    return ARG(0);
}
SHIM(d3dx9_28_D3DXVec3Normalize, 2) {
    V3 v = norm3(rdv3(ARG(1)));
    wrfl(ARG(0), v.x); wrfl(ARG(0) + 4, v.y); wrfl(ARG(0) + 8, v.z);
    return ARG(0);
}
SHIM(d3dx9_28_D3DXGetFVFVertexSize, 1) {
    uint32_t fvf = ARG(0), size = 0;
    switch (fvf & 0x400E) {   // D3DFVF_POSITION_MASK
    case 0x002: size = 12; break; case 0x004: size = 16; break; case 0x006: size = 16; break; case 0x008: size = 20; break;
    case 0x00A: size = 24; break; case 0x00C: size = 28; break; case 0x00E: size = 32; break; case 0x4002: size = 16; break;
    default: break;
    }
    if (fvf & 0x10) size += 12;   // NORMAL
    if (fvf & 0x20) size += 4;    // PSIZE
    if (fvf & 0x40) size += 4;    // DIFFUSE
    if (fvf & 0x80) size += 4;    // SPECULAR
    uint32_t ntex = (fvf >> 8) & 0xF;
    static const uint32_t tsz[4] = {8, 12, 16, 4};   // D3DFVF_TEXTUREFORMAT2, 3, 4, 1
    for (uint32_t i = 0; i < ntex && i < 8; i++) size += tsz[(fvf >> (16 + 2 * i)) & 3];
    return size;
}

// ---------------- images ----------------
// Pixels are 0xAARRGGBB words (D3DFMT_A8R8G8B8 in memory).
typedef struct {
    uint32_t w, h;
    uint32_t *px;                // the top level, decoded
    uint32_t file_fmt;           // the format D3DXIMAGE_INFO reports
    uint32_t tex_fmt;            // the texture format D3DFMT_UNKNOWN picks
    uint32_t file_mips;
    int file_type;               // D3DXIMAGE_FILEFORMAT: BMP 0, TGA 2, DDS 4
    // DDS block data (DXT files): per level, kept for a direct copy
    const uint8_t *blocks; uint32_t block_fmt;
} Image;

static inline uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }
static inline uint32_t ex5(uint32_t v) { return v << 3 | v >> 2; }
static inline uint32_t ex6(uint32_t v) { return v << 2 | v >> 4; }
static inline uint32_t ex4(uint32_t v) { return v << 4 | v; }
static uint32_t c565(uint16_t c) { return argb(255, ex5(c >> 11 & 31), ex6(c >> 5 & 63), ex5(c & 31)); }

// DXT block decode into 16 pixels
static void dxt_decode_block(uint32_t fmt, const uint8_t *b, uint32_t out[16]) {
    const uint8_t *cb = fmt == F_DXT1 ? b : b + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint32_t p[4]; p[0] = c565(c0); p[1] = c565(c1);
    uint32_t r0 = p[0] >> 16 & 255, g0 = p[0] >> 8 & 255, b0 = p[0] & 255, r1 = p[1] >> 16 & 255, g1 = p[1] >> 8 & 255, b1 = p[1] & 255;
    if (fmt != F_DXT1 || c0 > c1) {
        p[2] = argb(255, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
        p[3] = argb(255, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
    } else {
        p[2] = argb(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        p[3] = 0;   // transparent black
    }
    uint32_t idx = (uint32_t)cb[4] | (uint32_t)cb[5] << 8 | (uint32_t)cb[6] << 16 | (uint32_t)cb[7] << 24;
    for (int i = 0; i < 16; i++) out[i] = p[idx >> (2 * i) & 3];
    if (fmt == F_DXT2 || fmt == F_DXT3) {
        for (int i = 0; i < 16; i++) { uint32_t a = (b[i / 2] >> (4 * (i & 1))) & 15; out[i] = (out[i] & 0xFFFFFF) | ex4(a) << 24; }
    } else if (fmt == F_DXT4 || fmt == F_DXT5) {
        uint32_t a[8]; a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - (uint32_t)i) * a[0] + (uint32_t)i * a[1]) / 7;
        else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - (uint32_t)i) * a[0] + (uint32_t)i * a[1]) / 5; a[6] = 0; a[7] = 255; }
        uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) out[i] = (out[i] & 0xFFFFFF) | a[bits >> (3 * i) & 7] << 24;
    }
}
static uint32_t *dxt_decode(uint32_t fmt, const uint8_t *data, uint32_t w, uint32_t h) {
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = block_bytes(fmt), blk[16];
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        dxt_decode_block(fmt, data + ((size_t)by * bw + bx) * bb, blk);
        for (int i = 0; i < 16; i++) {
            uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
            if (x < w && y < h) px[(size_t)y * w + x] = blk[i];
        }
    }
    return px;
}

// DXT encode: endpoints along the block's principal colour axis (from the bounding box diagonal with the larger
// spread), quantized to 5:6:5, each pixel the nearest palette entry
static uint16_t to565(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}
static void color_block(const uint32_t px[16], int dxt1_alpha, uint8_t *o) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0}, n = 0;
    for (int i = 0; i < 16; i++) {
        if (dxt1_alpha && (px[i] >> 24) < 128) continue;
        int c[3] = {(int)(px[i] >> 16 & 255), (int)(px[i] >> 8 & 255), (int)(px[i] & 255)};
        for (int k = 0; k < 3; k++) { if (c[k] < mn[k]) mn[k] = c[k]; if (c[k] > mx[k]) mx[k] = c[k]; }
        n++;
    }
    if (!n) { mn[0] = mn[1] = mn[2] = mx[0] = mx[1] = mx[2] = 0; }
    // orient the diagonal by the covariance sign of the channels against the widest one
    int wide = (mx[1] - mn[1] >= mx[0] - mn[0] && mx[1] - mn[1] >= mx[2] - mn[2]) ? 1 : (mx[0] - mn[0] >= mx[2] - mn[2] ? 0 : 2);
    double mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) { if (dxt1_alpha && (px[i] >> 24) < 128) continue; mean[0] += px[i] >> 16 & 255; mean[1] += px[i] >> 8 & 255; mean[2] += px[i] & 255; }
    if (n) for (int k = 0; k < 3; k++) mean[k] /= n;
    double cov[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (dxt1_alpha && (px[i] >> 24) < 128) continue;
        double c[3] = {(double)(px[i] >> 16 & 255) - mean[0], (double)(px[i] >> 8 & 255) - mean[1], (double)(px[i] & 255) - mean[2]};
        for (int k = 0; k < 3; k++) cov[k] += c[k] * c[wide];
    }
    int e0[3], e1[3];
    for (int k = 0; k < 3; k++) {
        if (k == wide || cov[k] >= 0) { e0[k] = mx[k]; e1[k] = mn[k]; } else { e0[k] = mn[k]; e1[k] = mx[k]; }
        int inset = (e0[k] - e1[k]) / 16;   // pull the ends in slightly: less error for the interpolated entries
        e0[k] -= inset; e1[k] += inset;
    }
    uint16_t c0 = to565(e0[0], e0[1], e0[2]), c1 = to565(e1[0], e1[1], e1[2]);
    int three = dxt1_alpha && n < 16;
    if (three ? c0 > c1 : c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (!three && c0 == c1) { if (c1 > 0) c1--; else c0++; }   // four-colour mode needs c0 > c1
    uint32_t pal[4]; pal[0] = c565(c0); pal[1] = c565(c1);
    int pr[4][3];
    for (int k = 0; k < 2; k++) { pr[k][0] = (int)(pal[k] >> 16 & 255); pr[k][1] = (int)(pal[k] >> 8 & 255); pr[k][2] = (int)(pal[k] & 255); }
    int npal = three ? 3 : 4;
    for (int k = 0; k < 3; k++) {
        if (three) { pr[2][k] = (pr[0][k] + pr[1][k]) / 2; }
        else { pr[2][k] = (2 * pr[0][k] + pr[1][k]) / 3; pr[3][k] = (pr[0][k] + 2 * pr[1][k]) / 3; }
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        if (three && (px[i] >> 24) < 128) best = 3;
        else {
            int r = (int)(px[i] >> 16 & 255), g = (int)(px[i] >> 8 & 255), b = (int)(px[i] & 255), bd = 1 << 30;
            for (int k = 0; k < npal; k++) {
                int d = (r - pr[k][0]) * (r - pr[k][0]) + (g - pr[k][1]) * (g - pr[k][1]) + (b - pr[k][2]) * (b - pr[k][2]);
                if (d < bd) { bd = d; best = k; }
            }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    o[0] = (uint8_t)c0; o[1] = (uint8_t)(c0 >> 8); o[2] = (uint8_t)c1; o[3] = (uint8_t)(c1 >> 8);
    o[4] = (uint8_t)idx; o[5] = (uint8_t)(idx >> 8); o[6] = (uint8_t)(idx >> 16); o[7] = (uint8_t)(idx >> 24);
}
static void alpha_block5(const uint32_t px[16], uint8_t *o) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) { int a = (int)(px[i] >> 24); if (a < mn) mn = a; if (a > mx) mx = a; }
    o[0] = (uint8_t)mx; o[1] = (uint8_t)mn;
    int pal[8]; pal[0] = mx; pal[1] = mn;
    if (mx > mn) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * mx + i * mn) / 7;
    else for (int i = 2; i < 8; i++) pal[i] = mx;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int a = (int)(px[i] >> 24), best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) { int d = abs(a - pal[k]); if (d < bd) { bd = d; best = k; } }
        bits |= (uint64_t)best << (3 * i);
    }
    for (int i = 0; i < 6; i++) o[2 + i] = (uint8_t)(bits >> (8 * i));
}
static void dxt_encode(uint32_t fmt, const uint32_t *px, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, blk[16];
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        for (int i = 0; i < 16; i++) {   // edge blocks repeat the last row/column
            uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
            if (x >= w) x = w - 1;
            if (y >= h) y = h - 1;
            blk[i] = px[(size_t)y * w + x];
        }
        uint8_t *o = dst + (size_t)by * pitch + (size_t)bx * block_bytes(fmt);
        if (fmt == F_DXT1) color_block(blk, 1, o);
        else if (fmt == F_DXT2 || fmt == F_DXT3) {
            for (int i = 0; i < 8; i++) {
                uint32_t a0 = (blk[2 * i] >> 24) * 15 + 127, a1 = (blk[2 * i + 1] >> 24) * 15 + 127;
                o[i] = (uint8_t)((a0 / 255) | (a1 / 255) << 4);
            }
            color_block(blk, 0, o + 8);
        } else { alpha_block5(blk, o); color_block(blk, 0, o + 8); }
    }
}

// one pixel into a target format
static void put_px(uint32_t fmt, uint8_t *d, uint32_t p) {
    uint32_t a = p >> 24, r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
    switch (fmt) {
    case F_A8R8G8B8: memcpy(d, &p, 4); break;
    case F_X8R8G8B8: { uint32_t v = p | 0xFF000000u; memcpy(d, &v, 4); break; }
    case F_A8B8G8R8: { uint32_t v = a << 24 | b << 16 | g << 8 | r; memcpy(d, &v, 4); break; }
    case F_X8B8G8R8: { uint32_t v = 0xFF000000u | b << 16 | g << 8 | r; memcpy(d, &v, 4); break; }
    case F_R8G8B8: d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; break;
    case F_R5G6B5: { uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); memcpy(d, &v, 2); break; }
    case F_X1R5G5B5: { uint16_t v = (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(d, &v, 2); break; }
    case F_A1R5G5B5: { uint16_t v = (uint16_t)((a >= 128) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(d, &v, 2); break; }
    case F_A4R4G4B4: { uint16_t v = (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); memcpy(d, &v, 2); break; }
    case F_X4R4G4B4: { uint16_t v = (uint16_t)(0xF000 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); memcpy(d, &v, 2); break; }
    case F_A8: d[0] = (uint8_t)a; break;
    case F_L8: d[0] = (uint8_t)((r * 77 + g * 150 + b * 29 + 128) >> 8); break;   // luminance: BT.601 weights
    case F_A8L8: d[0] = (uint8_t)((r * 77 + g * 150 + b * 29 + 128) >> 8); d[1] = (uint8_t)a; break;
    default: break;
    }
}

// resampling: point; bilinear (upscaling, D3DX_FILTER_LINEAR); area average (downscaling with triangle/box)
static uint32_t *resample(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, int filter) {
    uint32_t *d = calloc((size_t)dw * dh, 4);
    if (!d) return NULL;
    if (filter == FILTER_NONE) {   // no scaling: the image at the top-left, the rest transparent black
        for (uint32_t y = 0; y < dh && y < sh; y++) memcpy(d + (size_t)y * dw, src + (size_t)y * sw, (size_t)(dw < sw ? dw : sw) * 4);
        return d;
    }
    for (uint32_t y = 0; y < dh; y++) for (uint32_t x = 0; x < dw; x++) {
        if (filter == FILTER_POINT) { d[(size_t)y * dw + x] = src[(size_t)(y * sh / dh) * sw + x * sw / dw]; continue; }
        double fx0 = (double)x * sw / dw, fx1 = (double)(x + 1) * sw / dw, fy0 = (double)y * sh / dh, fy1 = (double)(y + 1) * sh / dh;
        double acc[4] = {0, 0, 0, 0}, wsum = 0;
        if (fx1 - fx0 <= 1.0 && fy1 - fy0 <= 1.0) {   // magnification: bilinear between texel centres
            double sx = (x + 0.5) * sw / dw - 0.5, sy = (y + 0.5) * sh / dh - 0.5;
            int x0 = (int)floor(sx), y0 = (int)floor(sy);
            double tx = sx - x0, ty = sy - y0;
            for (int k = 0; k < 4; k++) {
                int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                xx = xx < 0 ? 0 : xx >= (int)sw ? (int)sw - 1 : xx; yy = yy < 0 ? 0 : yy >= (int)sh ? (int)sh - 1 : yy;
                double wgt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
                uint32_t p = src[(size_t)yy * sw + (size_t)xx];
                acc[0] += wgt * (p >> 24); acc[1] += wgt * (p >> 16 & 255); acc[2] += wgt * (p >> 8 & 255); acc[3] += wgt * (p & 255);
                wsum += wgt;
            }
        } else {                                       // minification: the covered area's average
            for (uint32_t yy = (uint32_t)fy0; yy < sh && yy < (uint32_t)ceil(fy1); yy++)
                for (uint32_t xx = (uint32_t)fx0; xx < sw && xx < (uint32_t)ceil(fx1); xx++) {
                    double wx = fmin(fx1, xx + 1.0) - fmax(fx0, (double)xx), wy = fmin(fy1, yy + 1.0) - fmax(fy0, (double)yy);
                    double wgt = wx * wy;
                    if (wgt <= 0) continue;
                    uint32_t p = src[(size_t)yy * sw + xx];
                    acc[0] += wgt * (p >> 24); acc[1] += wgt * (p >> 16 & 255); acc[2] += wgt * (p >> 8 & 255); acc[3] += wgt * (p & 255);
                    wsum += wgt;
                }
        }
        if (wsum <= 0) wsum = 1;
        d[(size_t)y * dw + x] = argb((uint32_t)(acc[0] / wsum + 0.5), (uint32_t)(acc[1] / wsum + 0.5), (uint32_t)(acc[2] / wsum + 0.5), (uint32_t)(acc[3] / wsum + 0.5));
    }
    return d;
}
static uint32_t *half(const uint32_t *s, uint32_t w, uint32_t h, uint32_t *nw, uint32_t *nh) {   // box-filtered next level
    uint32_t dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
    uint32_t *d = malloc((size_t)dw * dh * 4);
    if (!d) return NULL;
    for (uint32_t y = 0; y < dh; y++) for (uint32_t x = 0; x < dw; x++) {
        uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t xx = x * 2 + (k & 1), yy = y * 2 + (k >> 1);
            if (xx >= w || yy >= h) continue;
            uint32_t p = s[(size_t)yy * w + xx];
            acc[0] += p >> 24; acc[1] += p >> 16 & 255; acc[2] += p >> 8 & 255; acc[3] += p & 255; n++;
        }
        d[(size_t)y * dw + x] = argb((acc[0] + n / 2) / n, (acc[1] + n / 2) / n, (acc[2] + n / 2) / n, (acc[3] + n / 2) / n);
    }
    *nw = dw; *nh = dh;
    return d;
}

// ---- file decoders ----
static uint32_t rd16le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t mask_get(uint32_t v, uint32_t mask) {   // a mask field scaled to 8 bits
    if (!mask) return 0;
    int sh = __builtin_ctz(mask), bits = __builtin_popcount(mask);
    uint32_t x = (v & mask) >> sh, max = (bits >= 32) ? 0xFFFFFFFFu : (1u << bits) - 1;
    return (uint32_t)(((uint64_t)x * 255 + max / 2) / max);
}
static int image_size_ok(uint32_t w, uint32_t h) { return w && h && w <= 16384 && h <= 16384; }

static uint32_t load_dds(const uint8_t *d, size_t n, Image *im) {
    if (n < 128 || rd32le(d + 4) != 124) return D3DXERR_INVALIDDATA;
    uint32_t h = rd32le(d + 12), w = rd32le(d + 16), mips = rd32le(d + 28);
    uint32_t pff = rd32le(d + 80), fcc = rd32le(d + 84), bits = rd32le(d + 88);
    uint32_t rm = rd32le(d + 92), gm = rd32le(d + 96), bm = rd32le(d + 100), am = rd32le(d + 104);
    if (!image_size_ok(w, h)) return D3DXERR_INVALIDDATA;
    im->w = w; im->h = h; im->file_type = 4; im->file_mips = mips ? mips : 1;
    const uint8_t *p = d + 128;
    size_t left = n - 128;
    if (pff & 4) {   // DDPF_FOURCC
        if (!is_dxt(fcc)) { rt_log("[d3dx] DDS: FourCC %.4s is not supported", (const char *)&fcc); return D3DXERR_INVALIDDATA; }
        size_t lvl0 = (size_t)((w + 3) / 4) * ((h + 3) / 4) * block_bytes(fcc);
        if (left < lvl0) return D3DXERR_INVALIDDATA;
        // the mip levels present in the file
        size_t need = 0; uint32_t lw = w, lh = h, have = 0;
        for (uint32_t i = 0; i < im->file_mips; i++) {
            size_t sz = (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * block_bytes(fcc);
            if (need + sz > left) break;
            need += sz; have++;
            lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        }
        im->file_mips = have;
        im->px = dxt_decode(fcc, p, w, h);
        if (!im->px) return E_OUTOFMEMORY;
        im->file_fmt = im->tex_fmt = fcc;
        im->blocks = p; im->block_fmt = fcc;
        return D3D_OK;
    }
    uint32_t bpp = bits / 8;
    if (bpp < 1 || bpp > 4) return D3DXERR_INVALIDDATA;
    if ((uint64_t)w * h * bpp > left) return D3DXERR_INVALIDDATA;
    im->px = malloc((size_t)w * h * 4);
    if (!im->px) return E_OUTOFMEMORY;
    int lum = (pff & 0x20000) != 0, alpha_only = (pff & 2) && !(pff & 0x40) && !lum, has_a = (pff & 1) || alpha_only;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < bpp; k++) v |= (uint32_t)p[i * bpp + k] << (8 * k);
        uint32_t a = has_a ? mask_get(v, am) : 255;
        if (alpha_only) im->px[i] = argb(a, 0, 0, 0);
        else if (lum) { uint32_t l = mask_get(v, rm); im->px[i] = argb(a, l, l, l); }
        else im->px[i] = argb(a, mask_get(v, rm), mask_get(v, gm), mask_get(v, bm));
    }
    if (alpha_only) im->file_fmt = F_A8;
    else if (lum) im->file_fmt = has_a ? F_A8L8 : F_L8;
    else if (bpp == 4) im->file_fmt = has_a ? (rm == 0xFF ? F_A8B8G8R8 : F_A8R8G8B8) : (rm == 0xFF ? F_X8B8G8R8 : F_X8R8G8B8);
    else if (bpp == 3) im->file_fmt = F_R8G8B8;
    else if (bpp == 2) im->file_fmt = gm == 0x7E0 ? F_R5G6B5 : am == 0xF000 ? F_A4R4G4B4 : has_a ? F_A1R5G5B5 : F_X1R5G5B5;
    else im->file_fmt = F_L8;
    im->tex_fmt = im->file_fmt == F_R8G8B8 ? F_X8R8G8B8 : im->file_fmt;
    im->file_mips = 1;   // uncompressed levels below the top are rebuilt from it
    return D3D_OK;
}

static uint32_t load_tga(const uint8_t *d, size_t n, Image *im) {
    if (n < 18) return D3DXERR_INVALIDDATA;
    uint32_t idlen = d[0], cmtype = d[1], type = d[2], cmfirst = rd16le(d + 3), cmlen = rd16le(d + 5), cmbits = d[7];
    uint32_t w = rd16le(d + 12), h = rd16le(d + 14), bits = d[16], desc = d[17];
    int rle = type >= 9, base = type & 7;
    if ((base != 1 && base != 2 && base != 3) || !image_size_ok(w, h)) return D3DXERR_INVALIDDATA;
    if (base == 1 && (cmtype != 1 || bits != 8)) return D3DXERR_INVALIDDATA;
    if (base == 2 && bits != 15 && bits != 16 && bits != 24 && bits != 32) return D3DXERR_INVALIDDATA;
    if (base == 3 && bits != 8) return D3DXERR_INVALIDDATA;
    size_t pos = 18 + idlen;
    uint32_t cmap[256] = {0};
    if (cmtype == 1) {
        uint32_t eb = (cmbits + 7) / 8;
        if (eb < 2 || eb > 4 || pos + (size_t)cmlen * eb > n) return D3DXERR_INVALIDDATA;
        for (uint32_t i = 0; i < cmlen; i++) {
            const uint8_t *e = d + pos + (size_t)i * eb;
            uint32_t v = eb == 2 ? argb(255, ex5(rd16le(e) >> 10 & 31), ex5(rd16le(e) >> 5 & 31), ex5(rd16le(e) & 31))
                                 : argb(eb == 4 ? e[3] : 255, e[2], e[1], e[0]);
            if (cmfirst + i < 256) cmap[cmfirst + i] = v;
        }
        pos += (size_t)cmlen * eb;
    }
    uint32_t bpp = (bits + 7) / 8, abits = desc & 15;
    im->px = malloc((size_t)w * h * 4);
    if (!im->px) return E_OUTOFMEMORY;
    size_t total = (size_t)w * h, i = 0;
    uint8_t pixel[4] = {0, 0, 0, 0};
    while (i < total) {
        size_t run = 1; int raw = 1;
        if (rle) {
            if (pos >= n) break;
            uint8_t hd = d[pos++];
            run = (size_t)(hd & 127) + 1; raw = !(hd & 128);
            if (!raw) { if (pos + bpp > n) break; memcpy(pixel, d + pos, bpp); pos += bpp; }
        }
        for (size_t k = 0; k < run && i < total; k++, i++) {
            if (raw) { if (pos + bpp > n) { i = total; break; } memcpy(pixel, d + pos, bpp); pos += bpp; }
            uint32_t v;
            if (base == 1) v = cmap[pixel[0]];
            else if (base == 3) v = argb(255, pixel[0], pixel[0], pixel[0]);
            else if (bpp == 2) { uint32_t s = rd16le(pixel); v = argb((bits == 16 && abits) ? ((s & 0x8000) ? 255 : 0) : 255, ex5(s >> 10 & 31), ex5(s >> 5 & 31), ex5(s & 31)); }
            else v = argb(bpp == 4 ? pixel[3] : 255, pixel[2], pixel[1], pixel[0]);
            // the file's origin: bottom-left unless bit 5; right-to-left when bit 4
            size_t x = i % w, y = i / w;
            if (desc & 0x10) x = w - 1 - x;
            if (!(desc & 0x20)) y = h - 1 - y;
            im->px[y * w + x] = v;
        }
    }
    if (i < total) { free(im->px); im->px = NULL; return D3DXERR_INVALIDDATA; }
    im->w = w; im->h = h; im->file_type = 2; im->file_mips = 1;
    if (base == 1) { im->file_fmt = F_P8; im->tex_fmt = cmbits == 32 ? F_A8R8G8B8 : F_X8R8G8B8; }
    else if (base == 3) im->file_fmt = im->tex_fmt = F_L8;
    else if (bpp == 4) im->file_fmt = im->tex_fmt = F_A8R8G8B8;
    else if (bpp == 3) { im->file_fmt = F_R8G8B8; im->tex_fmt = F_X8R8G8B8; }
    else im->file_fmt = im->tex_fmt = (bits == 16 && abits) ? F_A1R5G5B5 : F_X1R5G5B5;
    return D3D_OK;
}

static uint32_t load_bmp(const uint8_t *d, size_t n, Image *im) {
    if (n < 26 || d[0] != 'B' || d[1] != 'M') return D3DXERR_INVALIDDATA;
    uint32_t off = rd32le(d + 10), hs = rd32le(d + 14);
    int32_t w, h; uint32_t bits, comp = 0, used = 0, palent = 4;
    if (hs == 12) { w = (int32_t)rd16le(d + 18); h = (int16_t)rd16le(d + 20); bits = rd16le(d + 24); palent = 3; }
    else {
        if (hs < 40 || n < 14 + (size_t)hs) return D3DXERR_INVALIDDATA;
        w = (int32_t)rd32le(d + 18); h = (int32_t)rd32le(d + 22); bits = rd16le(d + 28); comp = rd32le(d + 30); used = rd32le(d + 46);
    }
    int topdown = h < 0;
    uint32_t uw = (uint32_t)w, uh = (uint32_t)(h < 0 ? -h : h);
    if (w <= 0 || !image_size_ok(uw, uh)) return D3DXERR_INVALIDDATA;
    if (comp != 0 && comp != 3) { rt_log("[d3dx] BMP: compression %u is not supported", comp); return D3DXERR_INVALIDDATA; }
    uint32_t rm = 0, gm = 0, bm = 0;
    if (comp == 3) {
        size_t mo = 14 + (hs >= 52 ? 40 : hs);
        if (mo + 12 > n) return D3DXERR_INVALIDDATA;
        rm = rd32le(d + mo); gm = rd32le(d + mo + 4); bm = rd32le(d + mo + 8);
    } else if (bits == 16) { rm = 0x7C00; gm = 0x3E0; bm = 0x1F; }
    else if (bits == 32) { rm = 0xFF0000; gm = 0xFF00; bm = 0xFF; }
    uint32_t pal[256] = {0};
    if (bits <= 8) {
        if (bits != 1 && bits != 4 && bits != 8) return D3DXERR_INVALIDDATA;
        uint32_t cnt = used && used <= 256 ? used : 1u << bits;
        size_t po = 14 + hs;
        if (po + (size_t)cnt * palent > n) return D3DXERR_INVALIDDATA;
        for (uint32_t i = 0; i < cnt; i++) { const uint8_t *e = d + po + (size_t)i * palent; pal[i] = argb(255, e[2], e[1], e[0]); }
    } else if (bits != 16 && bits != 24 && bits != 32) return D3DXERR_INVALIDDATA;
    size_t stride = ((size_t)uw * bits + 31) / 32 * 4;
    if (off > n || (size_t)off + stride * uh > n) return D3DXERR_INVALIDDATA;
    im->px = malloc((size_t)uw * uh * 4);
    if (!im->px) return E_OUTOFMEMORY;
    for (uint32_t y = 0; y < uh; y++) {
        const uint8_t *row = d + off + stride * (topdown ? y : uh - 1 - y);
        for (uint32_t x = 0; x < uw; x++) {
            uint32_t v;
            if (bits == 1) v = pal[row[x / 8] >> (7 - x % 8) & 1];
            else if (bits == 4) v = pal[row[x / 2] >> (x & 1 ? 0 : 4) & 15];
            else if (bits == 8) v = pal[row[x]];
            else if (bits == 24) v = argb(255, row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
            else { uint32_t s = bits == 16 ? rd16le(row + x * 2) : rd32le(row + x * 4); v = argb(255, mask_get(s, rm), mask_get(s, gm), mask_get(s, bm)); }
            im->px[(size_t)y * uw + x] = v;
        }
    }
    im->w = uw; im->h = uh; im->file_type = 0; im->file_mips = 1;
    if (bits <= 8) { im->file_fmt = F_P8; im->tex_fmt = F_X8R8G8B8; }
    else if (bits == 16) im->file_fmt = im->tex_fmt = gm == 0x7E0 ? F_R5G6B5 : F_X1R5G5B5;
    else if (bits == 24) { im->file_fmt = F_R8G8B8; im->tex_fmt = F_X8R8G8B8; }
    else im->file_fmt = im->tex_fmt = F_X8R8G8B8;
    return D3D_OK;
}

static uint32_t load_image(const uint8_t *d, size_t n, Image *im) {
    memset(im, 0, sizeof *im);
    if (n >= 4 && !memcmp(d, "DDS ", 4)) return load_dds(d, n, im);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return load_bmp(d, n, im);
    if (n >= 18) {   // TGA has no signature: a known image type with a sane header
        uint32_t t = d[2];
        if ((t == 1 || t == 2 || t == 3 || t == 9 || t == 10 || t == 11) && d[1] <= 1) return load_tga(d, n, im);
    }
    rt_log("[d3dx] unrecognized image data (%zu bytes, starts %02x %02x %02x %02x)", n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0, n > 2 ? d[2] : 0, n > 3 ? d[3] : 0);
    return D3DXERR_INVALIDDATA;
}

// ---------------- textures ----------------
static uint32_t pow2_up(uint32_t v) { uint32_t p = 1; while (p < v && p < 0x80000000u) p <<= 1; return p; }
static uint32_t full_chain(uint32_t w, uint32_t h) { uint32_t n = 1, m = w > h ? w : h; while (m > 1) { m >>= 1; n++; } return n; }
static uint32_t level_rows(uint32_t fmt, uint32_t h) { return is_dxt(fmt) ? (h + 3) / 4 : h; }

// LockRect level `lv` (D3DLOCKED_RECT: INT Pitch, void *pBits) and fill it with the level's pixels or blocks
static uint32_t fill_level(CPU *c, uint32_t tex, uint32_t lv, uint32_t fmt, uint32_t w, uint32_t h, const uint32_t *px, const uint8_t *blocks) {
    uint32_t lr = gcalloc(1, 8);
    if (!lr) return E_OUTOFMEMORY;
    uint32_t a[4] = {lv, lr, 0, 0};
    uint32_t hr = vc(c, tex, TEX_LockRect, 4, a);
    if ((int32_t)hr < 0) { gfree(lr); rt_log("[d3dx] LockRect(level %u) failed (0x%x)", lv, hr); return hr; }
    uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
    uint32_t rows = level_rows(fmt, h);
    if (is_dxt(fmt)) {
        uint32_t row_bytes = (w + 3) / 4 * block_bytes(fmt);
        if (blocks) for (uint32_t y = 0; y < rows; y++) memcpy(G2H(bits + y * pitch), blocks + (size_t)y * row_bytes, row_bytes);
        else {
            uint8_t *tmp = malloc((size_t)row_bytes * rows);
            if (!tmp) { uint32_t ua[1] = {lv}; vc(c, tex, TEX_UnlockRect, 1, ua); gfree(lr); return E_OUTOFMEMORY; }
            dxt_encode(fmt, px, w, h, tmp, row_bytes);
            for (uint32_t y = 0; y < rows; y++) memcpy(G2H(bits + y * pitch), tmp + (size_t)y * row_bytes, row_bytes);
            free(tmp);
        }
    } else {
        int bpp = fmt_bpp(fmt);
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = G2H(bits + y * pitch);
            for (uint32_t x = 0; x < w; x++) put_px(fmt, row + (size_t)x * (size_t)bpp, px[(size_t)y * w + x]);
        }
    }
    uint32_t ua[1] = {lv};
    vc(c, tex, TEX_UnlockRect, 1, ua);
    gfree(lr);
    return D3D_OK;
}

// D3DXCreateTextureFromFileInMemoryEx semantics for every loader
static uint32_t create_from_image(CPU *c, uint32_t dev, Image *im, uint32_t w, uint32_t h, uint32_t mips, uint32_t usage,
                                  uint32_t fmt, uint32_t pool, uint32_t filter, uint32_t mipfilter, uint32_t colorkey,
                                  uint32_t info, uint32_t out) {
    if (info) {   // D3DXIMAGE_INFO: Width, Height, Depth, MipLevels, Format, ResourceType, ImageFileFormat
        wr32(info, im->w); wr32(info + 4, im->h); wr32(info + 8, 1); wr32(info + 12, im->file_mips);
        wr32(info + 16, im->file_fmt); wr32(info + 20, 3); wr32(info + 24, (uint32_t)im->file_type);
    }
    if (w == D3DX_DEFAULT_NONPOW2 || w == D3DX_FROM_FILE) w = im->w; else if (w == 0 || w == D3DX_DEFAULT) w = pow2_up(im->w);
    if (h == D3DX_DEFAULT_NONPOW2 || h == D3DX_FROM_FILE) h = im->h; else if (h == 0 || h == D3DX_DEFAULT) h = pow2_up(im->h);
    if (fmt == F_UNKNOWN || fmt == D3DX_FROM_FILE) fmt = im->tex_fmt;
    if (!is_dxt(fmt) && !fmt_bpp(fmt)) { rt_log("[d3dx] texture format %u is not supported", fmt); return D3DERR_INVALIDCALL; }
    if (is_dxt(fmt)) { if (w > 2) w = (w + 3) & ~3u; if (h > 2) h = (h + 3) & ~3u; }
    if (!image_size_ok(w, h)) return D3DERR_INVALIDCALL;
    uint32_t chain = full_chain(w, h);
    if (mips == D3DX_FROM_FILE) mips = im->file_mips;
    else if (mips == 0 || mips == D3DX_DEFAULT) mips = chain;
    if (mips > chain) mips = chain;
    uint32_t f_main = (filter == D3DX_DEFAULT ? FILTER_TRIANGLE : filter & 0xFF), f_mip = (mipfilter == D3DX_DEFAULT ? FILTER_BOX : mipfilter & 0xFF);
    if (f_main < FILTER_NONE || f_main > FILTER_BOX || f_mip < FILTER_NONE || f_mip > FILTER_BOX) return D3DERR_INVALIDCALL;
    if (colorkey) for (size_t i = 0; i < (size_t)im->w * im->h; i++) if (im->px[i] == colorkey) im->px[i] = 0;
    // the file's DXT blocks are copied when nothing changes them
    int copy_blocks = im->blocks && fmt == im->block_fmt && w == im->w && h == im->h && !colorkey;

    // a D3DPOOL_DEFAULT texture can't be locked (unless dynamic): fill a system-memory copy and UpdateTexture
    int staged = pool == POOL_DEFAULT && !(usage & USAGE_DYNAMIC);
    uint32_t pp = gcalloc(1, 8);
    if (!pp) return E_OUTOFMEMORY;
    uint32_t ca[8] = {w, h, mips, usage, fmt, pool, pp, 0};
    uint32_t hr = vc(c, dev, DEV_CreateTexture, 8, ca);
    if ((int32_t)hr < 0) { gfree(pp); rt_log("[d3dx] CreateTexture(%ux%u, %u levels, usage 0x%x, format %u, pool %u) failed (0x%x)", w, h, mips, usage, fmt, pool, hr); return hr; }
    uint32_t tex = rd32(pp), fill = tex;
    if (staged) {
        uint32_t sa[8] = {w, h, mips, 0, fmt, POOL_SYSTEMMEM, pp + 4, 0};
        hr = vc(c, dev, DEV_CreateTexture, 8, sa);
        if ((int32_t)hr < 0) { vc(c, tex, TEX_Release, 0, NULL); gfree(pp); rt_log("[d3dx] the staging texture failed (0x%x)", hr); return hr; }
        fill = rd32(pp + 4);
    }
    uint32_t levels = vc(c, fill, TEX_GetLevelCount, 0, NULL);
    if (levels > mips) levels = mips;

    // level 0
    uint32_t *cur = NULL, cw = w, ch = h;
    if (!(w == im->w && h == im->h)) {
        cur = resample(im->px, im->w, im->h, w, h, (int)f_main);
        if (!cur) hr = E_OUTOFMEMORY;
    }
    const uint32_t *lvpx = cur ? cur : im->px;
    const uint8_t *blk = copy_blocks ? im->blocks : NULL;
    if ((int32_t)hr >= 0) hr = fill_level(c, fill, 0, fmt, w, h, lvpx, blk);
    // further levels: the file's blocks while it has them, then box-filtered from the level above
    uint32_t *owned = cur;
    const uint32_t *prev = lvpx;
    for (uint32_t lv = 1; lv < levels && (int32_t)hr >= 0; lv++) {
        uint32_t nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
        if (blk) {
            blk += (size_t)((cw + 3) / 4) * ((ch + 3) / 4) * block_bytes(fmt);
            if (lv < im->file_mips) {
                hr = fill_level(c, fill, lv, fmt, nw, nh, NULL, blk);
                cw = nw; ch = nh;
                if (lv + 1 < levels && lv + 1 >= im->file_mips) {   // the next level is generated: decode this one
                    uint32_t *dec = dxt_decode(fmt, blk, nw, nh);
                    free(owned); owned = dec; prev = dec;
                    if (!dec) hr = E_OUTOFMEMORY;
                    blk = NULL;
                }
                continue;
            }
            blk = NULL;
        }
        if (f_mip == FILTER_NONE) break;   // D3DX_FILTER_NONE: the lower levels are left as created
        uint32_t *next = f_mip == FILTER_BOX ? half(prev, cw, ch, &nw, &nh) : resample(prev, cw, ch, nw, nh, (int)f_mip);
        if (!next) { hr = E_OUTOFMEMORY; break; }
        free(owned); owned = next; prev = next;
        cw = nw; ch = nh;
        hr = fill_level(c, fill, lv, fmt, cw, ch, prev, NULL);
    }
    free(owned);
    if (staged) {
        if ((int32_t)hr >= 0) {
            uint32_t ua[2] = {fill, tex};
            hr = vc(c, dev, DEV_UpdateTexture, 2, ua);
            if ((int32_t)hr < 0) rt_log("[d3dx] UpdateTexture failed (0x%x)", hr);
        }
        vc(c, fill, TEX_Release, 0, NULL);
    }
    if ((int32_t)hr < 0) { vc(c, tex, TEX_Release, 0, NULL); gfree(pp); return hr; }
    wr32(out, tex);
    gfree(pp);
    return D3D_OK;
}

static uint32_t from_memory(CPU *c, uint32_t dev, const uint8_t *data, size_t n, uint32_t w, uint32_t h, uint32_t mips, uint32_t usage,
                            uint32_t fmt, uint32_t pool, uint32_t filter, uint32_t mipfilter, uint32_t colorkey, uint32_t info, uint32_t out) {
    if (!dev || !out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    if (!data || !n) return D3DERR_INVALIDCALL;
    Image im;
    uint32_t hr = load_image(data, n, &im);
    if (hr != D3D_OK) return hr;
    hr = create_from_image(c, dev, &im, w, h, mips, usage, fmt, pool, filter, mipfilter, colorkey, info, out);
    free(im.px);
    return hr;
}
static uint32_t from_file(CPU *c, uint32_t dev, const char *path, uint32_t w, uint32_t h, uint32_t mips, uint32_t usage,
                          uint32_t fmt, uint32_t pool, uint32_t filter, uint32_t mipfilter, uint32_t colorkey, uint32_t info, uint32_t out) {
    if (!dev || !path || !out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    char host[1200];
    FILE *f = fopen(rt_host_path(path, host, sizeof host), "rb");
    if (!f) { rt_log("[d3dx] can't open %s", path); return D3DXERR_INVALIDDATA; }
    fseeko(f, 0, SEEK_END);
    off_t sz = ftello(f);
    fseeko(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (off_t)(256u << 20)) { fclose(f); rt_log("[d3dx] %s: size %lld is out of range", path, (long long)sz); return D3DXERR_INVALIDDATA; }
    uint8_t *d = malloc((size_t)sz);
    size_t got = d ? fread(d, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (!d) return E_OUTOFMEMORY;
    if (got != (size_t)sz) { free(d); rt_log("[d3dx] %s: short read", path); return D3DXERR_INVALIDDATA; }
    uint32_t hr = from_memory(c, dev, d, got, w, h, mips, usage, fmt, pool, filter, mipfilter, colorkey, info, out);
    if (hr != D3D_OK) rt_log("[d3dx] %s: 0x%x", path, hr);
    free(d);
    return hr;
}

// D3DXCreateTexture(device, width, height, mips, usage, format, pool, **texture)
SHIM(d3dx9_28_D3DXCreateTexture, 8) {
    uint32_t dev = ARG(0), w = ARG(1), h = ARG(2), mips = ARG(3), usage = ARG(4), fmt = ARG(5), pool = ARG(6), out = ARG(7);
    if (!dev || !out) return D3DERR_INVALIDCALL;
    wr32(out, 0);
    // D3DXCheckTextureRequirements: 0 means 1; D3DX_DEFAULT takes the other side or 256
    w = w ? w : 1; h = h ? h : 1;
    if (w == D3DX_DEFAULT && h == D3DX_DEFAULT) w = h = 256;
    else if (w == D3DX_DEFAULT) w = h;
    else if (h == D3DX_DEFAULT) h = w;
    if (fmt == F_UNKNOWN) fmt = F_A8R8G8B8;
    if (is_dxt(fmt)) { if (w > 2) w = (w + 3) & ~3u; if (h > 2) h = (h + 3) & ~3u; }
    if (!image_size_ok(w, h)) return D3DERR_INVALIDCALL;
    uint32_t chain = full_chain(w, h);
    if (mips == 0 || mips == D3DX_DEFAULT || mips > chain) mips = chain;
    uint32_t a[8] = {w, h, mips, usage, fmt, pool, out, 0};
    uint32_t hr = vc(c, dev, DEV_CreateTexture, 8, a);
    if ((int32_t)hr < 0) rt_log("[d3dx] D3DXCreateTexture(%ux%u, %u levels, usage 0x%x, format %u, pool %u) failed (0x%x)", w, h, mips, usage, fmt, pool, hr);
    return hr;
}
SHIM(d3dx9_28_D3DXCreateTextureFromFileA, 3) {
    return from_file(c, ARG(0), STR(1), D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, F_UNKNOWN, 1, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, ARG(2));
}
// (device, file, width, height, mips, usage, format, pool, filter, mipfilter, colorkey, *srcinfo, *palette, **texture)
SHIM(d3dx9_28_D3DXCreateTextureFromFileExA, 14) {
    return from_file(c, ARG(0), STR(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8), ARG(9), ARG(10), ARG(11), ARG(13));
}
SHIM(d3dx9_28_D3DXCreateTextureFromFileInMemory, 4) {
    return from_memory(c, ARG(0), ARG(1) ? G2H(ARG(1)) : NULL, ARG(2), D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, F_UNKNOWN, 1,
                       D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, ARG(3));
}
// (device, data, size, width, height, mips, usage, format, pool, filter, mipfilter, colorkey, *srcinfo, *palette, **texture)
SHIM(d3dx9_28_D3DXCreateTextureFromFileInMemoryEx, 15) {
    return from_memory(c, ARG(0), ARG(1) ? G2H(ARG(1)) : NULL, ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8), ARG(9), ARG(10),
                       ARG(11), ARG(12), ARG(14));
}
