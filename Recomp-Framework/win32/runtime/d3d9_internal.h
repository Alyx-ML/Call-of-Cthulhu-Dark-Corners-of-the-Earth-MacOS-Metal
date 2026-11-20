// Direct3D 9 on Metal: internal types shared by d3d9_metal.m (device, resources, presentation), d3d9_shader.c
// (shader bytecode -> MSL) and d3d9_ffp.c (fixed-function pipeline -> MSL).
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- shaders (d3d9_shader.c) ----
// Translation inputs that come from device state instead of the bytecode. Shader model 1 pixel shaders have no
// sampler declarations, so the texture type bound to each stage decides the sampling function, and ps_1_1-1_3
// `tex` honours D3DTTFF_PROJECTED of its stage.
typedef struct {
    uint8_t sampler_type[16];  // 2 = 2D, 3 = cube, 4 = volume (0: 2D)
    uint8_t projected[8];      // ps_1_1-1_3: 0, or the coordinate (1-4) that `tex` divides by
} ShaderVariant;

typedef struct {
    int stage;                 // 0 vertex, 1 pixel
    uint32_t version;          // 0xFFFE0101...
    uint32_t hash;
    char *msl;                 // the body function `vs_<hash>` / `ps_<hash>` (malloc'd)
    // vertex shader inputs (dcl usage/index per input register), SM3 outputs
    struct { uint8_t used, usage, index; } in[16];
    struct { uint8_t used, usage, index, mask; } out[16];
    uint32_t samplers;         // bitmask of used samplers / texture stages
    uint8_t sampler_type[16];  // 2 = 2D, 3 = cube, 4 = volume
    int max_const_f;           // highest c# read from the constant buffer + 1 (relative addressing: all of them)
    uint8_t color_mask;        // pixel shader: oC# written (shader model 1: r0 -> oC0)
    uint8_t writes_depth;      // pixel shader: oDepth written
    uint8_t writes_fog;        // vertex shader: oFog written
    uint8_t writes_psize;      // vertex shader: oPts written
    char error[256];           // translation failure
} ShaderInfo;
int d3d9_translate_shader(const uint32_t *tokens, size_t ntokens, const ShaderVariant *var, ShaderInfo *out);
// test entry points: complete MSL sources (prelude + body + entry point "vs_main" / "ps_main"). vertex: `vsin` is
// the VSIn struct and `fetch` the statements that fill v[] from `in`; NULL, NULL: no inputs.
char *d3d9_vs_source(const ShaderInfo *vs, const char *vsin, const char *fetch);
char *d3d9_ps_source(const ShaderInfo *ps);
size_t d3d9_shader_length(const uint32_t *tokens, size_t max);   // tokens up to and including the end token, 0 if none

// constant buffer layouts shared with the MSL prelude (d3d9_prelude.h); the float constants come last, so a draw
// uploads the header and only the constants its shader reads
#define VS_CONST_F 256
#define PS_CONST_F 224
typedef struct {
    float pos_fixup[4];        // x, y: half-pixel offset in clip space ((63/64) / width, -(63/64) / height);
                               // z: D3DRS_DEPTHBIAS; w: D3DRS_POINTSIZE
    float clip[8][4];          // user clip planes (clip space for shaders, see d3d9_ffp.c for fixed function)
    int32_t i[16][4];
    uint32_t b[16];
    float c[VS_CONST_F][4];
} VSConsts;
typedef struct {
    float alpha_ref[4];        // x: D3DRS_ALPHAREF (0-255), y: alpha test on, z: D3DRS_ALPHAFUNC
    float fog[4];              // start, end, density
    float fog_color[4];
    float fog_mode[4];         // x: 0 off, 1 EXP, 2 EXP2, 3 LINEAR (vertex fog: any nonzero); y: 0 factor from FOG0,
                               // 1 pixel fog from depth (z), 2 pixel fog from eye distance (w)
    float tfactor[4];          // D3DRS_TEXTUREFACTOR
    float tss_const[8][4];     // D3DTSS_CONSTANT
    float bumpenv[8][4];       // D3DTSS_BUMPENVMAT00, 01, 10, 11
    float bumplum[8][4];       // D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET
    float lod_bias[4][4];      // D3DSAMP_MIPMAPLODBIAS of samplers 0-15
    int32_t i[16][4];
    uint32_t b[16];
    float c[PS_CONST_F][4];
} PSConsts;
#define VS_CONSTS_HEADER offsetof(VSConsts, c)
#define PS_CONSTS_HEADER offsetof(PSConsts, c)

// ---- fixed-function pipeline (d3d9_ffp.c) ----
// Vertex input registers of the fixed-function vertex stage (the device fills them from the declaration)
enum { FF_IN_POSITION = 0, FF_IN_BLENDWEIGHT = 1, FF_IN_BLENDINDICES = 2, FF_IN_NORMAL = 3, FF_IN_PSIZE = 4,
       FF_IN_COLOR0 = 5, FF_IN_COLOR1 = 6, FF_IN_TEXCOORD0 = 7 /* .. 14 */ };
typedef struct {
    uint8_t pretransformed;    // D3DDECLUSAGE_POSITIONT (D3DFVF_XYZRHW): screen-space input
    uint8_t has_normal, has_color0, has_color1, has_psize;
    uint8_t blend;             // D3DRS_VERTEXBLEND weights used (0: world matrix 0 only, 1-3)
    uint8_t tc_dim[8];         // components of input texture coordinate set i (0: absent)
    uint8_t lighting, normalize, localviewer, specular;
    uint8_t mat_src[4];        // D3DMCS_* of the diffuse, specular, ambient, emissive colors (vertex colors resolved)
    uint8_t light[8];          // D3DLIGHTTYPE of active light i, 0 after the last
    uint8_t fog_mode;          // vertex fog D3DFOG_* (0: none)
    uint8_t range_fog;
    uint8_t tci[8];            // D3DTSS_TEXCOORDINDEX of stage i: generator (D3DTSS_TCI_* >> 16) << 4 | index
    uint8_t ttff[8];           // D3DTSS_TEXTURETRANSFORMFLAGS count of stage i (0: no transform)
    uint8_t points, point_scale;
} FFVSKey;
typedef struct {
    // ca/aa: D3DTSS_COLORARG0-2 / ALPHAARG0-2; ttype: bound texture 2 = 2D, 3 = cube, 4 = volume, 0 = none (a texture
    // argument then reads (1, 1, 1, 1)); proj: D3DTTFF_PROJECTED coordinate (1-4) or 0
    struct { uint8_t cop, ca[3], aop, aa[3], res, ttype, proj; } st[8];
    uint8_t specular;          // D3DRS_SPECULARENABLE
} FFPSKey;
typedef struct { float diffuse[4], specular[4], ambient[4], position[4], direction[4];
                 float att[4];   // range, falloff, attenuation0, attenuation1
                 float spot[4];  // attenuation2, cos(theta / 2), cos(phi / 2)
} FFLight;
typedef struct {
    float pos_fixup[4];        // as VSConsts
    float clip[8][4];
    float proj[16];            // matrices are D3D (row-vector) matrices, used as `M * v` in MSL
    float wv[4][16];           // world i * view (vertex blending)
    float wvn[4][16];          // the normal matrices of wv (inverse transpose)
    float tex[8][16];
    float vp[4];               // pretransformed vertices: viewport X, Y, 2 / width, 2 / height
    float zr[4];               // viewport MinZ, MaxZ - MinZ
    float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4], mat_power[4];
    float ambient[4];          // D3DRS_AMBIENT
    float fog[4];              // start, end, density
    float point[4];            // D3DRS_POINTSIZE, _MIN, _MAX, viewport height
    float point_abc[4];        // D3DRS_POINTSCALE_A, _B, _C
    FFLight light[8];
} FFVSConsts;
// body functions with the signatures of translated shaders: `static void vs_ff(thread float4 *v, thread VSRegs &o,
// constant FFVSConsts &k)` and `static void ps_ff(thread PSRegs &in, thread PSORegs &o, constant PSConsts &k, ...)`
int d3d9_ffvs(const FFVSKey *key, ShaderInfo *out);
int d3d9_ffps(const FFPSKey *key, ShaderInfo *out);
