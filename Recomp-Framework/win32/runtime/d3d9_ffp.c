// Direct3D 9 fixed-function pipeline -> Metal Shading Language.
//
// The device builds a key from the render and texture stage states that change the code (d3d9_metal.m) and wraps
// the generated body functions like translated shaders: the vertex body reads the fixed input registers FF_IN_*
// (filled from the vertex declaration) and writes VSRegs; the pixel body runs the texture stages.
//
// Vertex stage (D3D9 fixed-function vertex processing): world/view/projection transform with up to 4 blend
// matrices, normals, lighting with up to 8 lights and material color sources, vertex fog, texture coordinate
// generation and texture transforms, point size. Pretransformed (XYZRHW) vertices are mapped back to clip space.
// Pixel stage: the D3DTOP_* operations of up to 8 texture stages, temp register, specular add.
#include "d3d9_internal.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *s; size_t n, cap; int oom; } Buf;
static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(Buf *b, const char *fmt, ...) {
    if (b->oom) return;
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) { va_end(ap2); b->oom = 1; return; }
    size_t need = b->n + (size_t)n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { va_end(ap2); b->oom = 1; return; }
            cap *= 2;
        }
        char *p = realloc(b->s, cap);
        if (!p) { va_end(ap2); b->oom = 1; return; }
        b->s = p; b->cap = cap;
    }
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap2); va_end(ap2);
    b->n += (size_t)n;
}
static int finish(Buf *b, ShaderInfo *out) {
    if (b->oom) { free(b->s); snprintf(out->error, sizeof out->error, "out of memory"); return 0; }
    out->msl = b->s;
    return 1;
}
static uint32_t hash_key(const void *k, size_t n) {
    const uint8_t *p = k; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// ---------------- vertex stage ----------------
enum { MCS_MATERIAL = 0, MCS_COLOR1 = 1, MCS_COLOR2 = 2 };
enum { LIGHT_POINT = 1, LIGHT_SPOT = 2, LIGHT_DIRECTIONAL = 3 };
enum { FOG_EXP = 1, FOG_EXP2 = 2, FOG_LINEAR = 3 };
enum { TCI_PASSTHRU = 0, TCI_CAMERASPACENORMAL = 1, TCI_CAMERASPACEPOSITION = 2, TCI_CAMERASPACEREFLECTIONVECTOR = 3,
       TCI_SPHEREMAP = 4 };

// a material color: the material's own or a vertex color
static const char *mat_color(const FFVSKey *k, int which, const char *material) {
    switch (k->mat_src[which]) {
    case MCS_COLOR1: return "c0";
    case MCS_COLOR2: return "c1";
    default: return material;
    }
}

int d3d9_ffvs(const FFVSKey *k, ShaderInfo *out) {
    memset(out, 0, sizeof *out);
    out->hash = hash_key(k, sizeof *k);
    out->writes_fog = 1;
    Buf b = {0};
    bput(&b, "static void vs_ff(thread float4 *v, thread VSRegs &o, constant FFVSConsts &k) {\n");
    bput(&b, "    float4 c0 = %s; float4 c1 = %s;\n", k->has_color0 ? "v[5]" : "float4(1.0)", k->has_color1 ? "v[6]" : "float4(0.0)");
    if (k->pretransformed) {
        // screen x, y, depth z and 1/w back to clip space (the viewport maps them forward again)
        bput(&b, "    float4 p = v[0]; float w = p.w == 0.0 ? 1.0 : 1.0 / p.w;\n"
                 "    float z = k.zr.y == 0.0 ? 0.0 : (p.z - k.zr.x) / k.zr.y;\n"
                 "    o.position = float4((p.x - k.vp.x) * k.vp.z - 1.0, 1.0 - (p.y - k.vp.y) * k.vp.w, z, 1.0) * w;\n"
                 "    o.COLOR0 = c0; o.COLOR1 = c1;\n"
                 // the fog factor of pretransformed vertices is the specular alpha
                 "    o.FOG0 = float4(%s);\n", k->has_color1 ? "c1.w" : "1.0");
        bput(&b, "    float3 vpos = float3(0.0), vnrm = float3(0.0, 0.0, 1.0);\n");
    } else {
        bput(&b, "    float4 pos = float4(v[0].xyz, 1.0);\n");
        bput(&b, "    float3 n = v[3].xyz;\n");
        if (k->blend == 0) {
            bput(&b, "    float4 vp4 = k.wv[0] * pos;\n");
            bput(&b, "    float3 vnrm = (k.wvn[0] * float4(n, 0.0)).xyz;\n");
        } else {
            // weights 0..blend-1 from the vertex, the last one makes the sum 1
            bput(&b, "    float wt[4]; float wsum = 0.0;\n");
            for (int i = 0; i < k->blend; i++) bput(&b, "    wt[%d] = v[1][%d]; wsum += wt[%d];\n", i, i, i);
            bput(&b, "    wt[%d] = 1.0 - wsum;\n", k->blend);
            bput(&b, "    float4 vp4 = float4(0.0); float3 vnrm = float3(0.0);\n");
            bput(&b, "    for (int i = 0; i <= %d; i++) { vp4 += wt[i] * (k.wv[i] * pos); vnrm += wt[i] * (k.wvn[i] * float4(n, 0.0)).xyz; }\n", k->blend);
        }
        bput(&b, "    float3 vpos = vp4.xyz / (vp4.w == 0.0 ? 1.0 : vp4.w);\n");
        bput(&b, "    o.position = k.proj * vp4;\n");
        if (k->normalize) bput(&b, "    if (any(vnrm != 0.0)) vnrm = normalize(vnrm);\n");
        if (k->lighting) {
            bput(&b, "    float4 md = %s, ms = %s, ma = %s, me = %s;\n",
                 mat_color(k, 0, "k.mat_diffuse"), mat_color(k, 1, "k.mat_specular"),
                 mat_color(k, 2, "k.mat_ambient"), mat_color(k, 3, "k.mat_emissive"));
            bput(&b, "    float3 amb = k.ambient.rgb, dif = float3(0.0), spe = float3(0.0);\n");
            // the direction from the vertex to the viewer: the eye at the origin, or along -z (infinitely far)
            // the direction to the viewer for the halfway vector: from the vertex to the eye, or (0, 0, 1) without
            // D3DRS_LOCALVIEWER (as documented for Direct3D 9 lighting)
            bput(&b, "    float3 eye = %s;\n", k->localviewer ? "-normalize(vpos)" : "float3(0.0, 0.0, 1.0)");
            for (int i = 0; i < 8 && k->light[i]; i++) {
                bput(&b, "    {\n        constant FFLight &L = k.light[%d];\n        float3 ld; float att = 1.0;\n", i);
                if (k->light[i] == LIGHT_DIRECTIONAL) bput(&b, "        ld = -normalize(L.direction.xyz);\n");
                else {
                    bput(&b, "        float3 tl = L.position.xyz - vpos; float d = length(tl); ld = d > 0.0 ? tl / d : float3(0.0);\n"
                             "        att = d > L.att.x ? 0.0 : 1.0 / max(L.att.z + L.att.w * d + L.spot.x * d * d, 1e-30);\n");
                    if (k->light[i] == LIGHT_SPOT)
                        bput(&b, "        float rho = dot(-ld, normalize(L.direction.xyz));\n"
                                 "        float spot = rho > L.spot.y ? 1.0 : rho <= L.spot.z ? 0.0 :\n"
                                 "            pow(max((rho - L.spot.z) / max(L.spot.y - L.spot.z, 1e-30), 0.0), L.att.y);\n"
                                 "        att *= spot;\n");
                }
                bput(&b, "        float nl = max(dot(vnrm, ld), 0.0);\n"
                         "        amb += att * L.ambient.rgb;\n"
                         "        dif += att * nl * L.diffuse.rgb;\n");
                if (k->specular)
                    bput(&b, "        if (nl > 0.0) { float3 h = normalize(eye + ld); float nh = max(dot(vnrm, h), 0.0);\n"
                             "            spe += att * (k.mat_power.x > 0.0 ? pow(nh, k.mat_power.x) : 1.0) * L.specular.rgb; }\n");
                bput(&b, "    }\n");
            }
            bput(&b, "    o.COLOR0 = float4(saturate(me.rgb + ma.rgb * amb + md.rgb * dif), md.a);\n");
            bput(&b, "    o.COLOR1 = float4(saturate(ms.rgb * spe), ms.a);\n");
        } else bput(&b, "    o.COLOR0 = c0; o.COLOR1 = c1;\n");
        if (k->fog_mode) {
            bput(&b, "    float fd = %s;\n", k->range_fog ? "length(vpos)" : "abs(vpos.z)");
            switch (k->fog_mode) {
            case FOG_LINEAR: bput(&b, "    float fr = k.fog.y - k.fog.x; o.FOG0 = float4(fr == 0.0 ? (fd < k.fog.y ? 1.0 : 0.0) : (k.fog.y - fd) / fr);\n"); break;
            case FOG_EXP: bput(&b, "    o.FOG0 = float4(exp(-fd * k.fog.z));\n"); break;
            default: bput(&b, "    float fe = fd * k.fog.z; o.FOG0 = float4(exp(-fe * fe));\n"); break;
            }
            bput(&b, "    o.FOG0 = saturate(o.FOG0);\n");
        } else {
            // no vertex fog mode: the fog factor is the specular alpha of the vertex
            bput(&b, "    o.FOG0 = float4(%s);\n", k->has_color1 ? "c1.w" : "1.0");
        }
    }
    // texture coordinates of the 8 stages
    int need_refl = 0;
    for (int i = 0; i < 8; i++) { int g = k->tci[i] >> 4; if (g == TCI_CAMERASPACEREFLECTIONVECTOR || g == TCI_SPHEREMAP) need_refl = 1; }
    if (need_refl && !k->pretransformed)
        bput(&b, "    float3 rfl; { float3 e = %s; rfl = 2.0 * dot(e, vnrm) * vnrm - e; }\n",
             k->localviewer ? "-normalize(vpos)" : "float3(0.0, 0.0, -1.0)");
    for (int i = 0; i < 8; i++) {
        int g = k->tci[i] >> 4, idx = k->tci[i] & 7, dim = k->tc_dim[idx];
        char in[96];
        if (g == TCI_PASSTHRU || k->pretransformed) {
            if (!dim) snprintf(in, sizeof in, "float4(0.0, 0.0, 0.0, 1.0)");
            // D3D pads a coordinate for the texture matrix with 1 then 0 (2D: translation in the third row)
            else if (k->ttff[i]) snprintf(in, sizeof in, dim == 1 ? "float4(v[%d].x, 1.0, 0.0, 0.0)" : dim == 2 ? "float4(v[%d].xy, 1.0, 0.0)" :
                                                       dim == 3 ? "float4(v[%d].xyz, 1.0)" : "v[%d]", FF_IN_TEXCOORD0 + idx);
            else snprintf(in, sizeof in, dim == 1 ? "float4(v[%d].x, 0.0, 0.0, 1.0)" : dim == 2 ? "float4(v[%d].xy, 0.0, 1.0)" :
                                         dim == 3 ? "float4(v[%d].xyz, 1.0)" : "v[%d]", FF_IN_TEXCOORD0 + idx);
        } else if (g == TCI_CAMERASPACENORMAL) snprintf(in, sizeof in, "float4(vnrm, 1.0)");
        else if (g == TCI_CAMERASPACEPOSITION) snprintf(in, sizeof in, "float4(vpos, 1.0)");
        else if (g == TCI_CAMERASPACEREFLECTIONVECTOR) snprintf(in, sizeof in, "float4(rfl, 1.0)");
        else if (g == TCI_SPHEREMAP)
            snprintf(in, sizeof in, "float4(rfl.xy / (2.0 * max(length(rfl + float3(0.0, 0.0, 1.0)), 1e-30)) + 0.5, 0.0, 1.0)");
        else { snprintf(out->error, sizeof out->error, "texture coordinate generator %d", g); free(b.s); return 0; }
        if (k->ttff[i]) bput(&b, "    o.TEXCOORD%d = k.tex[%d] * %s;\n", i, i, in);
        else bput(&b, "    o.TEXCOORD%d = %s;\n", i, in);
    }
    if (k->points) {
        bput(&b, "    float ps = %s;\n", k->has_psize ? "v[4].x" : "k.point.x");
        if (k->point_scale && !k->pretransformed)
            bput(&b, "    { float d = length(vpos); ps = k.point.w * ps * sqrt(1.0 / max(k.point_abc.x + k.point_abc.y * d + k.point_abc.z * d * d, 1e-30)); }\n");
        bput(&b, "    o.psize4 = float4(clamp(ps, k.point.y, k.point.z));\n");
    }
    bput(&b, "}\n");
    return finish(&b, out);
}

// ---------------- pixel stage ----------------
enum { TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X, TOP_MODULATE4X, TOP_ADD,
       TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH, TOP_BLENDDIFFUSEALPHA, TOP_BLENDTEXTUREALPHA,
       TOP_BLENDFACTORALPHA, TOP_BLENDTEXTUREALPHAPM, TOP_BLENDCURRENTALPHA, TOP_PREMODULATE,
       TOP_MODULATEALPHA_ADDCOLOR, TOP_MODULATECOLOR_ADDALPHA, TOP_MODULATEINVALPHA_ADDCOLOR,
       TOP_MODULATEINVCOLOR_ADDALPHA, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE, TOP_DOTPRODUCT3, TOP_MULTIPLYADD,
       TOP_LERP };
enum { TA_DIFFUSE = 0, TA_CURRENT, TA_TEXTURE, TA_TFACTOR, TA_SPECULAR, TA_TEMP, TA_CONSTANT };
#define TA_COMPLEMENT 0x10
#define TA_ALPHAREPLICATE 0x20

// an argument as a float4 (color) or float (alpha)
static int arg_expr(int a, int stage, int alpha, char *out, size_t on) {
    const char *base;
    char cb[48];
    switch (a & 7) {
    case TA_DIFFUSE: base = "diff"; break;
    case TA_CURRENT: base = "cur"; break;
    case TA_TEXTURE: snprintf(cb, sizeof cb, "tx%d", stage); base = cb; break;
    case TA_TFACTOR: base = "k.tfactor"; break;
    case TA_SPECULAR: base = "spec"; break;
    case TA_TEMP: base = "tmp"; break;
    case TA_CONSTANT: snprintf(cb, sizeof cb, "k.tss_const[%d]", stage); base = cb; break;
    default: return 0;
    }
    char v[96];
    if (alpha || (a & TA_ALPHAREPLICATE)) snprintf(v, sizeof v, alpha ? "%s.w" : "%s.wwww", base);
    else snprintf(v, sizeof v, "%s", base);
    if (a & TA_COMPLEMENT) snprintf(out, on, "(1.0 - %s)", v); else snprintf(out, on, "%s", v);
    return 1;
}
static int uses_texture(int a) { return (a & 7) == TA_TEXTURE; }
static int op_args(int op) {   // which arguments an operation reads: bit 0 arg0, bit 1 arg1, bit 2 arg2
    switch (op) {
    case TOP_DISABLE: return 0;
    case TOP_SELECTARG1: return 2;
    case TOP_SELECTARG2: return 4;
    case TOP_MULTIPLYADD: case TOP_LERP: return 7;
    case TOP_BUMPENVMAP: case TOP_BUMPENVMAPLUMINANCE: return 0;
    default: return 6;
    }
}

// the operation on arguments a0, a1, a2 (strings) for color (float4) or alpha (float)
static int op_expr(int op, const char *a0, const char *a1, const char *a2, int stage, int alpha, char *out, size_t on) {
    char tab[32]; snprintf(tab, sizeof tab, "tx%d.w", stage);
    switch (op) {
    case TOP_SELECTARG1: snprintf(out, on, "%s", a1); break;
    case TOP_SELECTARG2: snprintf(out, on, "%s", a2); break;
    case TOP_MODULATE: snprintf(out, on, "%s * %s", a1, a2); break;
    case TOP_MODULATE2X: snprintf(out, on, "2.0 * %s * %s", a1, a2); break;
    case TOP_MODULATE4X: snprintf(out, on, "4.0 * %s * %s", a1, a2); break;
    case TOP_ADD: snprintf(out, on, "%s + %s", a1, a2); break;
    case TOP_ADDSIGNED: snprintf(out, on, "%s + %s - 0.5", a1, a2); break;
    case TOP_ADDSIGNED2X: snprintf(out, on, "2.0 * (%s + %s - 0.5)", a1, a2); break;
    case TOP_SUBTRACT: snprintf(out, on, "%s - %s", a1, a2); break;
    case TOP_ADDSMOOTH: snprintf(out, on, "%s + %s - %s * %s", a1, a2, a1, a2); break;
    case TOP_BLENDDIFFUSEALPHA: snprintf(out, on, "mix(%s, %s, diff.w)", a2, a1); break;
    case TOP_BLENDTEXTUREALPHA: snprintf(out, on, "mix(%s, %s, %s)", a2, a1, tab); break;
    case TOP_BLENDFACTORALPHA: snprintf(out, on, "mix(%s, %s, k.tfactor.w)", a2, a1); break;
    case TOP_BLENDTEXTUREALPHAPM: snprintf(out, on, "%s + %s * (1.0 - %s)", a1, a2, tab); break;
    case TOP_BLENDCURRENTALPHA: snprintf(out, on, "mix(%s, %s, cur.w)", a2, a1); break;
    case TOP_PREMODULATE: snprintf(out, on, "%s", a1); break;   // the next stage multiplies by its texture
    case TOP_MODULATEALPHA_ADDCOLOR:   // color only: arg1.rgb + arg1.a * arg2.rgb
        if (alpha) snprintf(out, on, "%s", a1); else snprintf(out, on, "float4(%s.rgb + %s.w * %s.rgb, %s.w)", a1, a1, a2, a1);
        break;
    case TOP_MODULATECOLOR_ADDALPHA:   // arg1.rgb * arg2.rgb + arg1.a
        if (alpha) snprintf(out, on, "%s", a1); else snprintf(out, on, "float4(%s.rgb * %s.rgb + %s.w, %s.w)", a1, a2, a1, a1);
        break;
    case TOP_MODULATEINVALPHA_ADDCOLOR:   // (1 - arg1.a) * arg2.rgb + arg1.rgb
        if (alpha) snprintf(out, on, "%s", a1); else snprintf(out, on, "float4((1.0 - %s.w) * %s.rgb + %s.rgb, %s.w)", a1, a2, a1, a1);
        break;
    case TOP_MODULATEINVCOLOR_ADDALPHA:   // (1 - arg1.rgb) * arg2.rgb + arg1.a
        if (alpha) snprintf(out, on, "%s", a1); else snprintf(out, on, "float4((1.0 - %s.rgb) * %s.rgb + %s.w, %s.w)", a1, a2, a1, a1);
        break;
    case TOP_DOTPRODUCT3: snprintf(out, on, alpha ? "saturate(4.0 * dot(%s - 0.5, %s - 0.5))" : "float4(saturate(4.0 * dot((%s).rgb - 0.5, (%s).rgb - 0.5)))", a1, a2); break;
    case TOP_MULTIPLYADD: snprintf(out, on, "%s + %s * %s", a0, a1, a2); break;
    case TOP_LERP: snprintf(out, on, "mix(%s, %s, %s)", a2, a1, a0); break;
    default: return 0;
    }
    return 1;
}

int d3d9_ffps(const FFPSKey *k, ShaderInfo *out) {
    memset(out, 0, sizeof *out);
    out->stage = 1;
    out->hash = hash_key(k, sizeof *k);
    out->color_mask = 1;
    out->version = 0xFFFF0000u;   // fixed function: fog and alpha test apply as for shader model 1-2
    // stages in use: up to the first disabled color operation
    int n = 0;
    while (n < 8 && k->st[n].cop != TOP_DISABLE && k->st[n].cop != 0) n++;
    // textures each stage samples
    uint8_t samples[8] = {0};
    for (int i = 0; i < n; i++) {
        int cm = op_args(k->st[i].cop), am = k->st[i].aop == TOP_DISABLE ? 0 : op_args(k->st[i].aop);
        for (int j = 0; j < 3; j++) {
            if ((cm >> j & 1) && uses_texture(k->st[i].ca[j])) samples[i] = 1;
            if ((am >> j & 1) && uses_texture(k->st[i].aa[j])) samples[i] = 1;
        }
        if ((k->st[i].res & 7) == TA_TEXTURE) samples[i] = 1;
        int op = k->st[i].cop;
        if (op == TOP_BUMPENVMAP || op == TOP_BUMPENVMAPLUMINANCE || op == TOP_BLENDTEXTUREALPHA || op == TOP_BLENDTEXTUREALPHAPM ||
            k->st[i].aop == TOP_BLENDTEXTUREALPHA || k->st[i].aop == TOP_BLENDTEXTUREALPHAPM) samples[i] = 1;
        if (i > 0 && k->st[i - 1].cop == TOP_PREMODULATE) samples[i] = 1;
    }
    Buf b = {0};
    bput(&b, "static void ps_ff(thread PSRegs &in, thread PSORegs &o, constant PSConsts &k");
    for (int i = 0; i < n; i++) if (samples[i] && k->st[i].ttype) {
        int tt = k->st[i].ttype;
        out->samplers |= 1u << i; out->sampler_type[i] = (uint8_t)tt;
        bput(&b, ", %s<float> tex%d, sampler smp%d", tt == 3 ? "texturecube" : tt == 4 ? "texture3d" : "texture2d", i, i);
    }
    bput(&b, ") {\n    float4 diff = saturate(in.COLOR0), spec = saturate(in.COLOR1);\n"
             "    float4 cur = diff, tmp = float4(0.0);\n    float2 bump = float2(0.0); float bumpl = 1.0;\n");
    for (int i = 0; i < n; i++) {
        bput(&b, "    // stage %d\n", i);
        int prev_bump = i > 0 && (k->st[i - 1].cop == TOP_BUMPENVMAP || k->st[i - 1].cop == TOP_BUMPENVMAPLUMINANCE);
        if (samples[i] && !k->st[i].ttype) {
            // no texture bound: the fixed-function stages read (1, 1, 1, 1)
            bput(&b, "    float4 tx%d = float4(1.0);\n", i);
            if (i > 0 && k->st[i - 1].cop == TOP_PREMODULATE) bput(&b, "    cur *= tx%d;\n", i);
        } else if (samples[i]) {
            int tt = k->st[i].ttype;
            const char *crd = tt == 2 ? "xy" : "xyz";
            char c[160];
            if (k->st[i].proj >= 1 && k->st[i].proj <= 4)   // D3DTTFF_PROJECTED: divide by the last coordinate
                snprintf(c, sizeof c, "(in.TEXCOORD%d.%s / in.TEXCOORD%d.%c)", i, crd, i, "xyzw"[k->st[i].proj - 1]);
            else snprintf(c, sizeof c, "in.TEXCOORD%d.%s", i, crd);
            if (prev_bump && tt == 2) bput(&b, "    float4 tx%d = tex%d.sample(smp%d, %s + bump, bias(k.lod_bias[%d][%d])) * bumpl;\n", i, i, i, c, i >> 2, i & 3);
            else bput(&b, "    float4 tx%d = tex%d.sample(smp%d, %s, bias(k.lod_bias[%d][%d]));\n", i, i, i, c, i >> 2, i & 3);
            if (i > 0 && k->st[i - 1].cop == TOP_PREMODULATE) bput(&b, "    cur *= tx%d;\n", i);
        }
        int cop = k->st[i].cop, aop = k->st[i].aop;
        if (cop == TOP_BUMPENVMAP || cop == TOP_BUMPENVMAPLUMINANCE) {
            // this stage's texture holds du/dv (and luminance): it perturbs the next stage's coordinates
            bput(&b, "    bump = float2(k.bumpenv[%d].x * tx%d.x + k.bumpenv[%d].z * tx%d.y, k.bumpenv[%d].y * tx%d.x + k.bumpenv[%d].w * tx%d.y);\n",
                 i, i, i, i, i, i, i, i);
            if (cop == TOP_BUMPENVMAPLUMINANCE) bput(&b, "    bumpl = saturate(tx%d.z * k.bumplum[%d].x + k.bumplum[%d].y);\n", i, i, i);
            else bput(&b, "    bumpl = 1.0;\n");
            continue;
        }
        char ca[3][96], aa[3][96], ce[700], ae[700];
        for (int j = 0; j < 3; j++) {
            if (!arg_expr(k->st[i].ca[j], i, 0, ca[j], sizeof ca[j])) snprintf(ca[j], sizeof ca[j], "cur");
            if (!arg_expr(k->st[i].aa[j], i, 1, aa[j], sizeof aa[j])) snprintf(aa[j], sizeof aa[j], "cur.w");
        }
        if (!op_expr(cop, ca[0], ca[1], ca[2], i, 0, ce, sizeof ce)) {
            snprintf(out->error, sizeof out->error, "texture stage %d color operation %d", i, cop); free(b.s); return 0;
        }
        if (cop == TOP_DOTPRODUCT3) snprintf(ae, sizeof ae, "saturate(4.0 * dot((%s).rgb - 0.5, (%s).rgb - 0.5))", ca[1], ca[2]);   // the dot product goes to alpha too
        else if (aop == TOP_DISABLE || aop == 0) snprintf(ae, sizeof ae, "cur.w");
        else if (!op_expr(aop, aa[0], aa[1], aa[2], i, 1, ae, sizeof ae)) {
            snprintf(out->error, sizeof out->error, "texture stage %d alpha operation %d", i, aop); free(b.s); return 0;
        }
        const char *res = (k->st[i].res & 7) == TA_TEMP ? "tmp" : "cur";
        bput(&b, "    { float4 rc = saturate(float4(%s)); float ra = saturate(float(%s)); %s = float4(rc.rgb, ra); }\n", ce, ae, res);
        // the perturbation applies to the stage after a bump stage only
        if (prev_bump) bput(&b, "    bump = float2(0.0); bumpl = 1.0;\n");
    }
    if (k->specular) bput(&b, "    cur.rgb = saturate(cur.rgb + spec.rgb);\n");
    bput(&b, "    o.color0 = cur;\n}\n");
    return finish(&b, out);
}
