// MSL shared by every D3D9 pipeline: register structs used by the translated body functions (d3d9_shader.c), the
// stage interface structs used by the entry points (d3d9_metal.m, d3d9_ffp.c), constant buffers and D3D-exact helpers.
#pragma once
#define D3D9_VARYINGS(X) X(FOG0) X(COLOR0) X(COLOR1) X(TEXCOORD0) X(TEXCOORD1) X(TEXCOORD2) X(TEXCOORD3) X(TEXCOORD4) \
    X(TEXCOORD5) X(TEXCOORD6) X(TEXCOORD7) X(TEXCOORD8) X(TEXCOORD9) X(NORMAL0) X(TANGENT0) X(BINORMAL0)
#define D3D9_STR_(...) #__VA_ARGS__
#define D3D9_STR(...) D3D9_STR_(__VA_ARGS__)

static const char d3d9_msl_prelude[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"#pragma clang diagnostic ignored \"-Wunused-variable\"\n"
"#pragma clang diagnostic ignored \"-Wunused-parameter\"\n"
"#pragma clang diagnostic ignored \"-Wunused-function\"\n"
// the layouts of VSConsts / PSConsts in d3d9_internal.h
"struct VSConsts { float4 pos_fixup; float4 clip[8]; int4 i[16]; uint b[16]; float4 c[256]; };\n"
"struct PSConsts { float4 alpha_ref; float4 fog; float4 fog_color; float4 fog_mode; float4 tfactor; float4 tss_const[8];\n"
"                  float4 bumpenv[8]; float4 bumplum[8]; float4 lod_bias[4]; int4 i[16]; uint b[16]; float4 c[224]; };\n"
// fixed-function vertex constants (FFVSConsts in d3d9_internal.h); the header matches VSConsts
"struct FFLight { float4 diffuse, specular, ambient, position, direction, att, spot; };\n"
"struct FFVSConsts { float4 pos_fixup; float4 clip[8]; float4x4 proj; float4x4 wv[4]; float4x4 wvn[4]; float4x4 tex[8];\n"
"  float4 vp; float4 zr; float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive, mat_power; float4 ambient; float4 fog;\n"
"  float4 point; float4 point_abc; FFLight light[8]; };\n"
"struct VSRegs { float4 position, psize4, FOG0, COLOR0, COLOR1, TEXCOORD0, TEXCOORD1, TEXCOORD2, TEXCOORD3, TEXCOORD4,\n"
"                TEXCOORD5, TEXCOORD6, TEXCOORD7, TEXCOORD8, TEXCOORD9, NORMAL0, TANGENT0, BINORMAL0; };\n"
"struct PSRegs { float4 vpos; bool face; float4 FOG0, COLOR0, COLOR1, TEXCOORD0, TEXCOORD1, TEXCOORD2, TEXCOORD3, TEXCOORD4,\n"
"                TEXCOORD5, TEXCOORD6, TEXCOORD7, TEXCOORD8, TEXCOORD9, NORMAL0, TANGENT0, BINORMAL0; };\n"
"struct PSORegs { float4 color0, color1, color2, color3, depth4; };\n"
"struct VSOut { float4 position [[position]];\n"
"  float4 FOG0 [[user(FOG0)]]; float4 COLOR0 [[user(COLOR0)]]; float4 COLOR1 [[user(COLOR1)]];\n"
"  float4 TEXCOORD0 [[user(TEXCOORD0)]]; float4 TEXCOORD1 [[user(TEXCOORD1)]]; float4 TEXCOORD2 [[user(TEXCOORD2)]];\n"
"  float4 TEXCOORD3 [[user(TEXCOORD3)]]; float4 TEXCOORD4 [[user(TEXCOORD4)]]; float4 TEXCOORD5 [[user(TEXCOORD5)]];\n"
"  float4 TEXCOORD6 [[user(TEXCOORD6)]]; float4 TEXCOORD7 [[user(TEXCOORD7)]]; float4 TEXCOORD8 [[user(TEXCOORD8)]];\n"
"  float4 TEXCOORD9 [[user(TEXCOORD9)]]; float4 NORMAL0 [[user(NORMAL0)]]; float4 TANGENT0 [[user(TANGENT0)]];\n"
"  float4 BINORMAL0 [[user(BINORMAL0)]]; };\n"
// D3DSHADE_FLAT: the same varyings with flat colors (a fragment input only qualifies interpolation)
"struct PSInFlat { float4 position [[position]];\n"
"  float4 FOG0 [[user(FOG0)]]; float4 COLOR0 [[user(COLOR0), flat]]; float4 COLOR1 [[user(COLOR1), flat]];\n"
"  float4 TEXCOORD0 [[user(TEXCOORD0)]]; float4 TEXCOORD1 [[user(TEXCOORD1)]]; float4 TEXCOORD2 [[user(TEXCOORD2)]];\n"
"  float4 TEXCOORD3 [[user(TEXCOORD3)]]; float4 TEXCOORD4 [[user(TEXCOORD4)]]; float4 TEXCOORD5 [[user(TEXCOORD5)]];\n"
"  float4 TEXCOORD6 [[user(TEXCOORD6)]]; float4 TEXCOORD7 [[user(TEXCOORD7)]]; float4 TEXCOORD8 [[user(TEXCOORD8)]];\n"
"  float4 TEXCOORD9 [[user(TEXCOORD9)]]; float4 NORMAL0 [[user(NORMAL0)]]; float4 TANGENT0 [[user(TANGENT0)]];\n"
"  float4 BINORMAL0 [[user(BINORMAL0)]]; };\n"
"typedef VSOut PSIn;\n"
// point primitives: the same outputs plus the point size
"struct VSOutP { float4 position [[position]];\n"
"  float4 FOG0 [[user(FOG0)]]; float4 COLOR0 [[user(COLOR0)]]; float4 COLOR1 [[user(COLOR1)]];\n"
"  float4 TEXCOORD0 [[user(TEXCOORD0)]]; float4 TEXCOORD1 [[user(TEXCOORD1)]]; float4 TEXCOORD2 [[user(TEXCOORD2)]];\n"
"  float4 TEXCOORD3 [[user(TEXCOORD3)]]; float4 TEXCOORD4 [[user(TEXCOORD4)]]; float4 TEXCOORD5 [[user(TEXCOORD5)]];\n"
"  float4 TEXCOORD6 [[user(TEXCOORD6)]]; float4 TEXCOORD7 [[user(TEXCOORD7)]]; float4 TEXCOORD8 [[user(TEXCOORD8)]];\n"
"  float4 TEXCOORD9 [[user(TEXCOORD9)]]; float4 NORMAL0 [[user(NORMAL0)]]; float4 TANGENT0 [[user(TANGENT0)]];\n"
"  float4 BINORMAL0 [[user(BINORMAL0)]]; float psize [[point_size]]; };\n"
// D3D9 arithmetic: rcp/rsq of 0 are +inf, rsq/log/pow use |x|, results replicate to all four components
"static inline float4 d3d_rcp(float x) { return float4(x == 0.0 ? INFINITY : 1.0 / x); }\n"
"static inline float4 d3d_rsq(float x) { float a = abs(x); return float4(a == 0.0 ? INFINITY : rsqrt(a)); }\n"
"static inline float4 d3d_log(float x) { float a = abs(x); return float4(a == 0.0 ? -INFINITY : log2(a)); }\n"
"static inline float4 d3d_pow(float x, float y) { return float4(pow(abs(x), y)); }\n"
"static inline float4 d3d_nrm(float4 v) { return v * rsqrt(dot(v.xyz, v.xyz)); }\n"
"static inline float4 d3d_lit(float4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;\n"
"  return float4(1.0, d, sp, 1.0); }\n"
// vs_1_1 expp / logp: the partial-precision four-component forms
"static inline float4 d3d_expp1(float s) { float f = floor(s); return float4(exp2(f), s - f, exp2(s), 1.0); }\n"
"static inline float4 d3d_logp1(float s) { float a = abs(s); if (a == 0.0) return float4(-INFINITY, 1.0, -INFINITY, 1.0);\n"
"  float e = floor(log2(a)); return float4(e, a / exp2(e), log2(a), 1.0); }\n"
// relative constant reads outside the register file read zero
"static inline float4 d3d_crel(constant float4 *c, int i, int n) { return (i >= 0 && i < n) ? c[i] : float4(0.0); }\n"
"template <typename T> static inline void ps_regs_from(thread PSRegs &r, T in, bool face) {\n"
"  r.vpos = float4(floor(in.position.xy), 0.0, 1.0); r.face = face;\n"
"#define CP(n) r.n = in.n;\n"
"  " D3D9_STR(D3D9_VARYINGS(CP)) "\n"
"#undef CP\n"
"}\n"
"static inline void vs_regs_init(thread VSRegs &o) {\n"
"  o = {}; o.COLOR0 = float4(1.0); o.COLOR1 = float4(0.0); o.FOG0 = float4(1.0);\n"
"  o.TEXCOORD0 = o.TEXCOORD1 = o.TEXCOORD2 = o.TEXCOORD3 = o.TEXCOORD4 = o.TEXCOORD5 = o.TEXCOORD6 = o.TEXCOORD7 = float4(0.0, 0.0, 0.0, 1.0);\n"
"}\n"
"static inline VSOut vs_out_from(thread VSRegs &o, float4 fix) {\n"
"  VSOut r; r.position = o.position;\n"
// D3D9 puts pixel centers at integer coordinates, Metal at +0.5: shift right and down by (almost) half a pixel
// (fix.xy = (63/64) * (1/width, -1/height) in clip space); fix.z: D3DRS_DEPTHBIAS, a constant offset of the depth
"  r.position.xy += fix.xy * o.position.w;\n"
"  r.position.z += fix.z * o.position.w;\n"
"#define CP(n) r.n = o.n;\n"
"  " D3D9_STR(D3D9_VARYINGS(CP)) "\n"
"#undef CP\n"
"  return r; }\n"
"static inline VSOutP vs_outp_from(VSOut o, float psize) {\n"
"  VSOutP r; r.position = o.position; r.psize = psize;\n"
"#define CP(n) r.n = o.n;\n"
"  " D3D9_STR(D3D9_VARYINGS(CP)) "\n"
"#undef CP\n"
"  return r; }\n"
// fog (D3DRS_FOGENABLE) on the final color of fixed-function and shader model 1-2 pixel shaders
"template <typename T> static inline float4 d3d_fog(float4 col, T in, constant PSConsts &k) {\n"
"  int mode = int(k.fog_mode.x); if (mode == 0) return col;\n"
"  int src = int(k.fog_mode.y); float f;\n"
"  if (src == 0) f = in.FOG0.x;\n"
"  else { float d = src == 1 ? in.position.z : 1.0 / in.position.w;\n"
"    if (mode == 3) { float r = k.fog.y - k.fog.x; f = r == 0.0 ? (d < k.fog.y ? 1.0 : 0.0) : (k.fog.y - d) / r; }\n"
"    else if (mode == 1) f = exp(-d * k.fog.z);\n"
"    else { float e = d * k.fog.z; f = exp(-e * e); } }\n"
"  f = saturate(f);\n"
"  return float4(mix(k.fog_color.rgb, col.rgb, f), col.a); }\n"
// alpha test (D3DRS_ALPHATESTENABLE) on the 8-bit alpha against D3DRS_ALPHAREF
"static inline bool d3d_alpha_pass(float alpha, constant PSConsts &k) {\n"
"  if (k.alpha_ref.y == 0.0) return true;\n"
"  float a = rint(saturate(alpha) * 255.0), ref = k.alpha_ref.x; int f = int(k.alpha_ref.z);\n"
"  return f == 1 ? false : f == 2 ? a < ref : f == 3 ? a == ref : f == 4 ? a <= ref : f == 5 ? a > ref :\n"
"         f == 6 ? a != ref : f == 7 ? a >= ref : true; }\n";
