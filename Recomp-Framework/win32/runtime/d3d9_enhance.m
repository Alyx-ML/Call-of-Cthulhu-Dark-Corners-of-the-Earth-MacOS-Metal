// Direct3D 9 on Metal: scene enhancements of rt_enh.h (global illumination, ambient occlusion, contact shadows, temporal
// anti-aliasing and bloom).
//
// d3d9_metal.m calls d3d9_enh_scene, d3d9_enh_taa and then d3d9_enh_bloom once per frame, after the game's 3D scene
// and before its post-processing (see enh_apply there). The ambient occlusion and contact shadow passes read the
// scene depth buffer and multiply the back buffer in place; the anti-aliasing pass replaces the back buffer with the
// output of MetalFX; the bloom pass adds to the back buffer in place. The global illumination pass reads the scene depth
// buffer and the back buffer and adds to the back buffer in place (before the ambient occlusion).
//
// Ambient occlusion: a Metal port of XeGTAO (ground-truth ambient occlusion, Jimenez et al. 2016, in the XeGTAO
// form of Intel):
//
//   MIT License
//
//   Copyright (C) 2016-2021, Intel Corporation
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
// Contact shadows: the screen-space shadows of Bend Studio ("Inside Bend: Screen Space Shadows", Days Gone),
// derived from bend_sss_gpu.h and bend_sss_cpu.h:
//
//   Copyright 2023 Sony Interactive Entertainment.
//
//   Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in
//   compliance with the License. You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software distributed under the License is
//   distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See
//   the License for the specific language governing permissions and limitations under the License.
//
//   Changes from the original: the shader is written in the Metal shading language and the dispatch list in C;
//   depth reads outside the texture give the far depth; pixels at the far depth get no shadow; the shadow fades
//   out between 4000 and 8000 units of view distance; the debug outputs are not included.
//
// The two ports come from the project's "Graphic Enhancements" folder (rt_d3d9_ao.m, rt_d3d9_sss.m) and are
// adapted to this renderer: the camera is the game's view and projection transforms (SetTransform), and the light
// is the projector of the game's own character shadows (d3d9_enh_light_pass) or a fixed direction (d3d9_enh_light_dir).
//
// Camera. For a perspective projection P (row-vector form, w = z_view * P23, P33 = 0) the positive view distance
// of a depth value z is t = P32 / (z - P22 / P23). The passes use the left-handed view space (x right, y up,
// z = t forward): x = ndc_x * t / P00, y = ndc_y * t / P11. A projection with P20 or P21 not zero (off center) or
// P33 not zero (orthographic) gives no pass for the frame.
//
// Units. The game's near plane is 8 units and its scenes measure thousands of units, so one unit is taken as one
// centimeter (UNCONFIRMED). The XeGTAO effect radius is in meters (with the XeGTAO radius multiplier 1.457).
//
// Bloom: the dual filter of Marius Bjorge ("Bandwidth-Efficient Rendering", SIGGRAPH 2015), from the project's
// "Graphic Enhancements" folder (rt_d3d9_bloom.m), adapted to this renderer, with the bloom of Jorge Jimenez ("Next
// Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014). The game's own glow (a quarter-size
// bright pass alpha * color - 1, blurred and added to the scene) gives no light from 8-bit scene colors, so the
// bloom here starts from the scene. The first pass is the 13-tap filter of Jimenez with the Karis average, in linear
// light, without a threshold. Six levels down (1/2 to 1/64 of the scene size), six passes up, each adding the
// up-sampled levels below it. The composite mixes the mean of the levels into the scene in linear light:
// scene x (1 - k) + bloom x k (Jimenez: k 0.04). Thus the bloom moves light from bright areas into their
// surroundings and does not add light to the image. k is 0.04 (low), 0.08 (medium) and 0.15 (high); medium and
// high are UNCONFIRMED tuning. A threshold does not suit this game: the game's scenes are dark. With the earlier
// soft threshold (0.75 of linear white), the bright pass of a level frame kept a mean of 0.00004 of white per pixel
// (measured on the back buffer), and the bloom was almost not visible. The levels are RGBA16Float. The passes do not
// clamp to white: a half-float back buffer (rt_enh.h reduced banding) gives its colors above white to the bloom and
// keeps the composite above white, for HDR output and tone mapping; a unorm back buffer clamps the composite when it
// stores it.
//
// Temporal anti-aliasing: the MetalFX temporal scaler at the size of the back buffer (input size = output size), as
// in Apple's "Applying temporal antialiasing and upscaling using MetalFX". d3d9_metal.m moves the 3D scene draws of each
// frame by a sub-pixel offset (the Halton (2, 3) sequence, 8 phases) and gives that offset here. The motion vectors come
// from the depth buffer and the camera of this frame and the last frame (taa_motion): the movement of the camera only.
// The game's vertex shaders give no position of the last frame, so an object that moves by itself (a character, a
// door) gets the motion of the static world behind it at that pixel; MetalFX then finds that the history color does not
// agree and uses less history for those pixels. The kernel also copies the depth to R32Float for MetalFX.
//
// Global illumination: screen-space ray tracing of one bounce of the scene's light, written for this renderer (the
// method of screen-space ray-traced global illumination, as in the RTGI shader of ReShade; no code of that shader is
// used). The pass runs at half the size of the back buffer. For each pixel, gi_main makes the view position and the
// normal from the depth buffer and sends GI_RAYS rays in a cosine-weighted hemisphere around the normal. Each ray
// takes GI_STEPS steps out to GI_RADIUS, with more steps near the pixel. A step whose position is behind the depth
// buffer by less than the thickness (GI_THICK + 2% of the view distance) is a hit: the ray gets the linear color of
// the back buffer at that pixel. A ray that leaves the screen, or that only passes behind thicker surfaces, gets no
// light. The result is the mean of the rays (the irradiance / pi of a cosine-weighted estimate). The noise is fixed
// for each pixel (the Hilbert index, as in XeGTAO), so the image does not flicker. gi_denoise then filters three
// times (an a-trous 3 x 3 filter at the distances 1, 2 and 4) with a depth weight. The composite (enh_fs) up-samples
// the result with depth weights and adds strength x albedo x result to the linear color. The game has no albedo
// buffer, so the albedo comes from the pixel's color: its chroma (the color / its luminance, at most 3) x 0.4, at most
// 0.9. Thus the bounce light keeps the color of the surface that it lights. The light can come only from surfaces on
// the screen. The levels low, medium and high use 2, 3 and 4 rays, 12, 16 and 20 steps and the strength 0.5, 1.0 and
// 1.5 (UNCONFIRMED tuning). The radius is 3 m and the thickness 0.3 m (UNCONFIRMED tuning).
//
// Settings: XeGTAO falloff range 0.615, sample distribution power 2, depth mip offset 3.30, denoise beta 1.2. The
// levels low, medium and high use 1 x 2, 2 x 2 and 3 x 3 slices x steps, an effect radius of 0.8, 1.2 and 1.6 m
// and a final value power of 2.4, 3.0 and 3.6. With the XeGTAO defaults (0.5 m, power 2.2) the mean visibility of
// a level frame was 0.94 to 0.985; with high it is 0.86 to 0.89 in most level frames (measured on the AO texture).
// Bend: 60 samples, 4 hard samples, 8 fade samples, surface thickness 0.005, bilinear threshold 0.02, contrast 4. The game has no separate light term, so
// the contact shadow removes CONTACT_STRENGTH x N.L of the color (UNCONFIRMED tuning). N.L is the cosine between the
// surface normal (from the depth buffer) and the direction to the light: a surface that the light does not reach (it
// faces away from the light) or that the light only grazes keeps its color, because its color has no part of that
// light that a shadow could remove.
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include "rt32.h"
#include "rt_enh.h"
#include <math.h>
#include <string.h>

#define AO_MIPS 5u
#define SSS_WAVE 64
#define SSS_MAX_DISPATCH 8
#define UNIT_SCALE 100.0f          // world units per meter (UNCONFIRMED: one unit = 1 cm)
#define CONTACT_STRENGTH 0.6f
#define GI_RADIUS (3.0f * UNIT_SCALE)   // the length of a global illumination ray (UNCONFIRMED tuning)
#define GI_THICK (0.3f * UNIT_SCALE)    // the thickness of a surface of the depth buffer, plus 2% of its distance

static const char enh_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct AOC {\n"
    "  float sx, sy, px, py;\n"
    "  float mulx, muly, addx, addy;\n"
    "  float mpx, mpy, da, db;\n"
    "  float radius, falloff, power, mipoff;\n"
    "  float beta, pad;\n"
    "};\n"
    "constant uint SLICES [[function_constant(0)]];\n"
    "constant uint STEPS [[function_constant(1)]];\n"
    "#define SKY 1.0e6\n"
    "static float view_z(float z, constant AOC &c) {\n"
    "  if (z >= 1.0) return SKY;\n"
    "  float den = z - c.da;\n"
    "  float w = abs(den) < 1e-12 ? SKY : c.db / den;\n"
    "  return clamp(w, 1e-4, SKY);\n"
    "}\n"
    // XeGTAO depth mip filter: the mean of four depths, less weight for depths much nearer than the farthest one
    "static float depth_filter(float d0, float d1, float d2, float d3, constant AOC &c) {\n"
    "  float mx = max(max(d0, d1), max(d2, d3));\n"
    "  float er = 0.75 * c.radius, fr = c.falloff * er, ff = er * (1.0 - c.falloff);\n"
    "  float fm = -1.0 / fr, fa = ff / fr + 1.0;\n"
    "  float w0 = saturate((mx - d0) * fm + fa), w1 = saturate((mx - d1) * fm + fa);\n"
    "  float w2 = saturate((mx - d2) * fm + fa), w3 = saturate((mx - d3) * fm + fa);\n"
    "  return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / (w0 + w1 + w2 + w3);\n"
    "}\n"
    // all five depth mips in one dispatch: a group of 8x8 threads covers 16x16 pixels of mip 0; every thread
    // reaches every barrier, only the writes check the bounds
    "kernel void ao_prefilter(depth2d<float, access::read> dt [[texture(0)]],\n"
    "  texture2d<float, access::write> m0 [[texture(1)]], texture2d<float, access::write> m1 [[texture(2)]],\n"
    "  texture2d<float, access::write> m2 [[texture(3)]], texture2d<float, access::write> m3 [[texture(4)]],\n"
    "  texture2d<float, access::write> m4 [[texture(5)]], constant AOC &c [[buffer(0)]],\n"
    "  uint2 gid [[thread_position_in_grid]], uint2 lid [[thread_position_in_threadgroup]]) {\n"
    "  threadgroup float sm[8][8];\n"
    "  uint2 mx = uint2(dt.get_width() - 1, dt.get_height() - 1), b = gid * 2;\n"
    "  float d0 = view_z(dt.read(min(b, mx)), c), d1 = view_z(dt.read(min(b + uint2(1, 0), mx)), c);\n"
    "  float d2 = view_z(dt.read(min(b + uint2(0, 1), mx)), c), d3 = view_z(dt.read(min(b + uint2(1, 1), mx)), c);\n"
    "  uint2 s0 = uint2(m0.get_width(), m0.get_height());\n"
    "  if (b.x < s0.x && b.y < s0.y) m0.write(float4(d0), b);\n"
    "  if (b.x + 1 < s0.x && b.y < s0.y) m0.write(float4(d1), b + uint2(1, 0));\n"
    "  if (b.x < s0.x && b.y + 1 < s0.y) m0.write(float4(d2), b + uint2(0, 1));\n"
    "  if (b.x + 1 < s0.x && b.y + 1 < s0.y) m0.write(float4(d3), b + uint2(1, 1));\n"
    "  float v = depth_filter(d0, d1, d2, d3, c);\n"
    "  if (gid.x < m1.get_width() && gid.y < m1.get_height()) m1.write(float4(v), gid);\n"
    "  sm[lid.y][lid.x] = v;\n"
    "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "  if ((lid.x & 1) == 0 && (lid.y & 1) == 0) {\n"
    "    v = depth_filter(sm[lid.y][lid.x], sm[lid.y][lid.x + 1], sm[lid.y + 1][lid.x], sm[lid.y + 1][lid.x + 1], c);\n"
    "    uint2 q = gid / 2;\n"
    "    if (q.x < m2.get_width() && q.y < m2.get_height()) m2.write(float4(v), q);\n"
    "  }\n"
    "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "  if ((lid.x & 1) == 0 && (lid.y & 1) == 0) sm[lid.y][lid.x] = v;\n"
    "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "  if ((lid.x & 3) == 0 && (lid.y & 3) == 0) {\n"
    "    v = depth_filter(sm[lid.y][lid.x], sm[lid.y][lid.x + 2], sm[lid.y + 2][lid.x], sm[lid.y + 2][lid.x + 2], c);\n"
    "    uint2 q = gid / 4;\n"
    "    if (q.x < m3.get_width() && q.y < m3.get_height()) m3.write(float4(v), q);\n"
    "  }\n"
    "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "  if ((lid.x & 3) == 0 && (lid.y & 3) == 0) sm[lid.y][lid.x] = v;\n"
    "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "  if (lid.x == 0 && lid.y == 0) {\n"
    "    v = depth_filter(sm[0][0], sm[0][4], sm[4][0], sm[4][4], c);\n"
    "    uint2 q = gid / 8;\n"
    "    if (q.x < m4.get_width() && q.y < m4.get_height()) m4.write(float4(v), q);\n"
    "  }\n"
    "}\n"
    "static float3 vpos(float2 sp, float z, constant AOC &c) {\n"
    "  return float3((float2(c.mulx, c.muly) * sp + float2(c.addx, c.addy)) * z, z);\n"
    "}\n"
    // edges (1 = no edge) on the left, right, top and bottom sides
    "static float4 edges_of(float z, float l, float r, float t, float b) {\n"
    "  float4 e = float4(l, r, t, b) - z;\n"
    "  float slr = (e.y - e.x) * 0.5, stb = (e.w - e.z) * 0.5;\n"
    "  float4 es = e + float4(slr, -slr, stb, -stb);\n"
    "  e = min(abs(e), abs(es));\n"
    "  return saturate(1.25 - e / (z * 0.011));\n"
    "}\n"
    // hl: the Hilbert curve index of each pixel of a 64 x 64 tile
    "kernel void ao_main(texture2d<float> dz [[texture(0)]], texture2d<float, access::write> ao [[texture(1)]],\n"
    "  texture2d<float, access::write> eo [[texture(2)]], texture2d<ushort, access::read> hl [[texture(3)]],\n"
    "  constant AOC &c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
    "  constexpr sampler pc(filter::nearest, mip_filter::nearest, address::clamp_to_edge);\n"
    "  if (p.x >= ao.get_width() || p.y >= ao.get_height()) return;\n"
    "  int2 ip = int2(p), mx = int2(dz.get_width() - 1, dz.get_height() - 1);\n"
    "  float zc = dz.read(p).r;\n"
    "  float zl = dz.read(uint2(clamp(ip + int2(-1, 0), int2(0), mx))).r;\n"
    "  float zr = dz.read(uint2(clamp(ip + int2(1, 0), int2(0), mx))).r;\n"
    "  float zt = dz.read(uint2(clamp(ip + int2(0, -1), int2(0), mx))).r;\n"
    "  float zb = dz.read(uint2(clamp(ip + int2(0, 1), int2(0), mx))).r;\n"
    "  float4 e = edges_of(zc, zl, zr, zt, zb);\n"
    "  eo.write(e, p);\n"
    "  if (zc >= SKY * 0.5) { ao.write(float4(1.0), p); return; }\n"
    "  float2 px = float2(c.px, c.py);\n"
    "  float2 sp = (float2(p) + 0.5) * px;\n"
    "  float3 C = vpos(sp, zc, c);\n"
    "  float3 nl = normalize(vpos(sp + float2(-px.x, 0.0), zl, c) - C);\n"
    "  float3 nr = normalize(vpos(sp + float2(px.x, 0.0), zr, c) - C);\n"
    "  float3 nt = normalize(vpos(sp + float2(0.0, -px.y), zt, c) - C);\n"
    "  float3 nb = normalize(vpos(sp + float2(0.0, px.y), zb, c) - C);\n"
    "  float4 acc = saturate(float4(e.x * e.z, e.z * e.y, e.y * e.w, e.w * e.x) + 0.01);\n"
    "  float3 N = normalize(acc.x * cross(nl, nt) + acc.y * cross(nt, nr) + acc.z * cross(nr, nb) + acc.w * cross(nb, nl));\n"
    "  C *= 0.9999;\n"
    "  float3 V = normalize(-C);\n"
    "  float sr = c.radius / (c.mpx * C.z);\n"
    "  float vis = saturate((10.0 - sr) / 100.0) * 0.5;\n"
    "  float mins = 1.3 / sr;\n"
    "  float fr = c.falloff * c.radius, ff = c.radius * (1.0 - c.falloff);\n"
    "  float fm = -1.0 / fr, fa = ff / fr + 1.0;\n"
    "  uint hi = hl.read(p % 64).r;\n"
    "  float nslice = fract(0.5 + float(hi) * 0.75487766624669276);\n"
    "  float nsample = fract(0.5 + float(hi) * 0.56984029099805326);\n"
    "  half3 Vh = half3(V), Nh = half3(N);\n"
    "  for (uint s = 0; s < SLICES; s++) {\n"
    "    half phi = half((float(s) + nslice) / float(SLICES)) * M_PI_H;\n"
    "    half cp = cos(phi), spi = sin(phi);\n"
    "    float2 om = float2(float(cp), -float(spi)) * sr;\n"
    "    half3 dv = half3(cp, spi, 0.0h);\n"
    "    half3 odv = dv - dot(dv, Vh) * Vh;\n"
    "    half3 ax = normalize(cross(odv, Vh));\n"
    "    half3 pn = Nh - ax * dot(Nh, ax);\n"
    "    half sgn = sign(dot(odv, pn));\n"
    "    half pl = length(pn);\n"
    "    half cn = saturate(dot(pn, Vh) / max(pl, 1e-4h));\n"
    "    half n = sgn * acos(cn);\n"
    "    half lo0 = cos(n + M_PI_2_H), lo1 = cos(n - M_PI_2_H);\n"
    "    half h0c = lo0, h1c = lo1;\n"
    "    for (uint j = 0; j < STEPS; j++) {\n"
    "      float sn = fract(nsample + float(s + j * STEPS) * 0.6180339887498948);\n"
    "      float st = (float(j) + sn) / float(STEPS);\n"
    "      st = st * st + mins;\n"
    "      float2 so = st * om;\n"
    "      float mip = clamp(log2(length(so)) - c.mipoff, 0.0, 4.0);\n"
    "      so = round(so) * px;\n"
    "      float2 s0 = sp + so, s1 = sp - so;\n"
    "      float3 d0 = vpos(s0, dz.sample(pc, s0, level(mip)).r, c) - C;\n"
    "      float3 d1 = vpos(s1, dz.sample(pc, s1, level(mip)).r, c) - C;\n"
    "      float l0 = length(d0), l1 = length(d1);\n"
    "      half w0 = half(saturate(l0 * fm + fa)), w1 = half(saturate(l1 * fm + fa));\n"
    "      half c0 = l0 > 0.0 ? half(dot(d0 / l0, V)) : lo0, c1 = l1 > 0.0 ? half(dot(d1 / l1, V)) : lo1;\n"
    "      h0c = max(h0c, mix(lo0, c0, w0));\n"
    "      h1c = max(h1c, mix(lo1, c1, w1));\n"
    "    }\n"
    "    pl = mix(pl, 1.0h, 0.05h);\n"
    "    half h0 = -acos(clamp(h1c, -1.0h, 1.0h)), h1 = acos(clamp(h0c, -1.0h, 1.0h));\n"
    "    h0 = n + clamp(h0 - n, -M_PI_2_H, M_PI_2_H);\n"
    "    h1 = n + clamp(h1 - n, -M_PI_2_H, M_PI_2_H);\n"
    "    half i0 = (cn + 2.0h * h0 * sin(n) - cos(2.0h * h0 - n)) * 0.25h;\n"
    "    half i1 = (cn + 2.0h * h1 * sin(n) - cos(2.0h * h1 - n)) * 0.25h;\n"
    "    vis += float(pl * (i0 + i1));\n"
    "  }\n"
    "  vis /= float(SLICES);\n"
    "  vis = pow(saturate(vis), c.power);\n"
    "  ao.write(float4(max(0.03, vis)), p);\n"
    "}\n"
    // XeGTAO denoise: an edge-aware 3x3 filter; beta is the weight of the center pixel
    "static float4 rd(texture2d<float, access::read> t, int2 q, int2 mx) { return t.read(uint2(clamp(q, int2(0), mx))); }\n"
    "static float denoise(texture2d<float, access::read> a, texture2d<float, access::read> e, int2 ip, float beta) {\n"
    "  int2 mx = int2(a.get_width() - 1, a.get_height() - 1);\n"
    "  float4 ec = rd(e, ip, mx);\n"
    "  float4 el = rd(e, ip + int2(-1, 0), mx), er = rd(e, ip + int2(1, 0), mx);\n"
    "  float4 et = rd(e, ip + int2(0, -1), mx), eb = rd(e, ip + int2(0, 1), mx);\n"
    "  float edginess = saturate(4.0 - 2.5 - (ec.x + ec.y + ec.z + ec.w)) / (4.0 - 2.5) * 0.5;\n"
    "  ec = saturate(ec + edginess);\n"
    "  ec *= float4(el.y, er.x, et.w, eb.z);\n"
    "  const float dw = 0.425;\n"
    "  float wtl = dw * (ec.x * el.z + ec.z * et.x), wtr = dw * (ec.z * et.y + ec.y * er.z);\n"
    "  float wbl = dw * (ec.w * eb.x + ec.x * el.w), wbr = dw * (ec.y * er.w + ec.w * eb.y);\n"
    "  float sum = beta * rd(a, ip, mx).r;\n"
    "  sum += ec.x * rd(a, ip + int2(-1, 0), mx).r + ec.y * rd(a, ip + int2(1, 0), mx).r;\n"
    "  sum += ec.z * rd(a, ip + int2(0, -1), mx).r + ec.w * rd(a, ip + int2(0, 1), mx).r;\n"
    "  sum += wtl * rd(a, ip + int2(-1, -1), mx).r + wtr * rd(a, ip + int2(1, -1), mx).r;\n"
    "  sum += wbl * rd(a, ip + int2(-1, 1), mx).r + wbr * rd(a, ip + int2(1, 1), mx).r;\n"
    "  float ws = beta + ec.x + ec.y + ec.z + ec.w + wtl + wtr + wbl + wbr;\n"
    "  return sum / ws;\n"
    "}\n"
    "kernel void ao_denoise(texture2d<float, access::read> a [[texture(0)]],\n"
    "  texture2d<float, access::read> e [[texture(1)]], texture2d<float, access::write> o [[texture(2)]],\n"
    "  constant float &beta [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= o.get_width() || p.y >= o.get_height()) return;\n"
    "  o.write(float4(denoise(a, e, int2(p), beta)), p);\n"
    "}\n"
    // Bend screen-space shadows
    "#define WAVE 64\n"
    "#define SAMPLES 60\n"
    "#define HARD 4\n"
    "#define FADE 8\n"
    "#define READS (SAMPLES / WAVE + 2)\n"
    "struct SSSP {\n"
    "  float4 light;\n"                  // screen x, y (pixels), depth, sign of w
    "  int2 offset;\n"                   // wave offset of this dispatch (pixels)
    "  float far_z, near_z;\n"
    "  float thickness, bilinear, contrast, pad;\n"
    "  float da, db, fade_start, fade_end;\n"
    "};\n"
    "static float dget(depth2d<float, access::read> t, int2 q, float far_z) {\n"
    "  if (q.x < 0 || q.y < 0 || q.x >= int(t.get_width()) || q.y >= int(t.get_height())) return far_z;\n"
    "  return t.read(uint2(q));\n"
    "}\n"
    "static int2 isign(int2 v) { return int2(v > 0) - int2(v < 0); }\n"
    "kernel void sss_main(depth2d<float, access::read> dt [[texture(0)]],\n"
    "  texture2d<float, access::write> out [[texture(1)]], constant SSSP &p [[buffer(0)]],\n"
    "  uint3 gu [[threadgroup_position_in_grid]], uint tidu [[thread_index_in_threadgroup]]) {\n"
    "  threadgroup float lds[READS * WAVE];\n"
    "  int3 g = int3(gu); int tid = int(tidu);\n"
    "  int2 xy = g.yz * WAVE + p.offset;\n"
    "  float2 lc = floor(p.light.xy) + 0.5;\n"
    "  float2 lf = p.light.xy - lc;\n"
    "  bool rev = p.light.w > 0.0;\n"
    "  int2 sg = isign(xy);\n"
    "  bool horiz = abs(xy.x + sg.y) < abs(xy.y - sg.x);\n"
    "  int2 ax = horiz ? int2(sg.y, 0) : int2(0, -sg.x);\n"
    "  xy = ax * g.x + xy;\n"
    "  float2 xf = float2(xy);\n"
    "  bool xmaj = abs(xf.x) > abs(xf.y);\n"
    "  float maj = xmaj ? xf.x : xf.y;\n"
    "  float ms = abs(maj), me = abs(maj) - float(WAVE);\n"
    "  float mf = xmaj ? lf.x : lf.y;\n"
    "  mf = maj > 0.0 ? -mf : mf;\n"
    "  float2 sxy = xf + lc;\n"
    "  float2 exy = mix(p.light.xy, sxy, (me + mf) / (ms + mf));\n"
    "  float2 dxy = sxy - exy;\n"
    "  float st = float(uint(tid) ^ (rev ? 0u : uint(WAVE - 1)));\n"
    "  float2 pix = mix(sxy, exy, st / float(WAVE));\n"
    "  float pdist = ms - st + mf;\n"
    "  float dir = -p.light.w;\n"
    "  float zs = p.near_z > p.far_z ? -1.0 : 1.0;\n"
    "  float samp0 = 0.0, raw0 = 0.0, thick0 = 1.0, dist0 = 1.0;\n"
    "  float2 wxy = floor(pix);\n"
    "  for (int i = 0; i < READS; i++) {\n"
    "    int2 rq = int2(floor(pix));\n"
    "    float minor = xmaj ? pix.y : pix.x;\n"
    "    float bl = fract(minor) - 0.5;\n"
    "    int b = bl > 0.0 ? 1 : -1;\n"
    "    int2 oq = int2(xmaj ? 0 : b, xmaj ? b : 0);\n"
    "    float d0 = dget(dt, rq, p.far_z), d1 = dget(dt, rq + oq, p.far_z);\n"
    "    float thick = abs(p.far_z - d0);\n"
    "    bool point_f = abs(d0 - d1) > thick * p.bilinear;\n"
    "    float shadow_d = mix(d0, d1, point_f ? 0.0 : abs(bl));\n"
    "    float sdist = pdist + float(WAVE * i) * dir;\n"
    "    if (i == 0) { samp0 = shadow_d; raw0 = d0; thick0 = thick; dist0 = sdist; }\n"
    "    float sd = (shadow_d - p.light.z) / sdist;\n"
    "    if (i != 0) sd = sdist > 0.0 ? sd : 1e10;\n"
    "    lds[i * WAVE + tid] = sd;\n"
    "    pix += dxy * dir;\n"
    "  }\n"
    "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "  int2 w = int2(wxy);\n"
    "  bool inside = w.x >= 0 && w.y >= 0 && w.x < int(out.get_width()) && w.y < int(out.get_height());\n"
    "  if (raw0 >= p.far_z) { if (inside) out.write(float4(1.0), uint2(w)); return; }\n"
    "  float start = (samp0 - p.light.z) / dist0;\n"
    "  float scale = min(dist0 + dir, 1.0 / p.thickness) * dist0 / thick0;\n"
    "  start = start * scale - zs;\n"
    "  int si = tid + 1;\n"
    "  float4 sv = float4(1.0);\n"
    "  float hard = 1.0;\n"
    "  for (int i = 0; i < HARD; i++) hard = min(hard, abs(start - lds[si + i] * scale));\n"
    "  for (int i = HARD; i < SAMPLES - FADE; i++) sv[i & 3] = min(sv[i & 3], abs(start - lds[si + i] * scale));\n"
    "  for (int i = SAMPLES - FADE; i < SAMPLES; i++) {\n"
    "    float fo = float(i + 1 - (SAMPLES - FADE)) / float(FADE + 1) * 0.75;\n"
    "    sv[i & 3] = min(sv[i & 3], abs(start - lds[si + i] * scale) + fo);\n"
    "  }\n"
    "  sv = saturate(sv * p.contrast + (1.0 - p.contrast));\n"
    "  hard = saturate(hard * p.contrast + (1.0 - p.contrast));\n"
    "  float r = min(hard, dot(sv, float4(0.25)));\n"
    "  float vz = p.db / (raw0 - p.da);\n"
    "  r = mix(r, 1.0, saturate((vz - p.fade_start) / (p.fade_end - p.fade_start)));\n"
    "  if (inside) out.write(float4(r), uint2(w));\n"
    "}\n"
    "static float3 to_lin(float3 c) { c = max(c, 0.0); return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }\n"
    "static float3 to_srgb(float3 l) { l = max(l, 0.0); return select(1.055 * pow(l, 1.0 / 2.4) - 0.055, l * 12.92, l <= 0.0031308); }\n"
    // screen-space global illumination (see the comment at the top of the file)
    "struct GIC { float mulx, muly, addx, addy, da, db, px, py, radius, thick, sw, sh; uint rays, steps, pad0, pad1; };\n"
    "static float gi_z(depth2d<float, access::read> dt, int2 q, constant GIC &c) {\n"
    "  float d = dt.read(uint2(q));\n"
    "  if (d >= 1.0) return SKY;\n"
    "  float den = d - c.da;\n"
    "  return abs(den) < 1e-12 ? SKY : clamp(c.db / den, 1e-4, SKY);\n"
    "}\n"
    "static float3 gi_pos(float2 sp, float z, constant GIC &c) { return float3((float2(c.mulx, c.muly) * sp + float2(c.addx, c.addy)) * z, z); }\n"
    // the screen position (0 .. 1) of a view position: the inverse of gi_pos
    "static float2 gi_proj(float3 v, constant GIC &c) { return (v.xy / v.z - float2(c.addx, c.addy)) / float2(c.mulx, c.muly); }\n"
    // out: rgb the mean light of the rays (linear), a the view distance of the pixel (SKY: no surface)
    "kernel void gi_main(depth2d<float, access::read> dt [[texture(0)]], texture2d<float, access::read> col [[texture(1)]],\n"
    "  texture2d<float, access::write> out [[texture(2)]], texture2d<ushort, access::read> hl [[texture(3)]],\n"
    "  constant GIC &c [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= out.get_width() || p.y >= out.get_height()) return;\n"
    "  int2 mx = int2(int(c.sw) - 1, int(c.sh) - 1);\n"
    "  int2 q = min(int2(p) * 2, mx);\n"
    "  float z = gi_z(dt, q, c);\n"
    "  if (z >= SKY * 0.5) { out.write(float4(0.0, 0.0, 0.0, SKY), p); return; }\n"
    "  float2 px = float2(c.px, c.py), sp = (float2(q) + 0.5) * px;\n"
    "  float3 P = gi_pos(sp, z, c);\n"
    // the normal from the neighbor with the smaller depth step on each axis, toward the camera
    "  float3 pl = gi_pos(sp - float2(px.x, 0.0), gi_z(dt, clamp(q - int2(1, 0), int2(0), mx), c), c);\n"
    "  float3 pr = gi_pos(sp + float2(px.x, 0.0), gi_z(dt, clamp(q + int2(1, 0), int2(0), mx), c), c);\n"
    "  float3 pt = gi_pos(sp - float2(0.0, px.y), gi_z(dt, clamp(q - int2(0, 1), int2(0), mx), c), c);\n"
    "  float3 pb = gi_pos(sp + float2(0.0, px.y), gi_z(dt, clamp(q + int2(0, 1), int2(0), mx), c), c);\n"
    "  float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;\n"
    "  float3 dy = abs(pb.z - P.z) < abs(P.z - pt.z) ? pb - P : P - pt;\n"
    "  float3 N = cross(dx, dy);\n"
    "  float nn = dot(N, N);\n"
    "  if (!(nn > 1e-24)) { out.write(float4(0.0, 0.0, 0.0, z), p); return; }\n"
    "  N *= rsqrt(nn);\n"
    "  if (dot(N, P) > 0.0) N = -N;\n"
    "  float3 T = normalize(abs(N.y) < 0.99 ? cross(N, float3(0.0, 1.0, 0.0)) : cross(N, float3(1.0, 0.0, 0.0)));\n"
    "  float3 B = cross(N, T);\n"
    "  uint hi = hl.read(uint2(q) % 64u).r;\n"
    "  float n0 = fract(0.5 + float(hi) * 0.75487766624669276), n1 = fract(0.5 + float(hi) * 0.56984029099805326);\n"
    "  float3 O = P + N * (z * 0.004);\n"
    "  float2 ss = float2(c.sw, c.sh);\n"
    "  float3 acc = float3(0.0);\n"
    "  for (uint r = 0; r < c.rays; r++) {\n"
    "    float u1 = fract(n0 + float(r) * 0.6180339887498948), u2 = fract(n1 + float(r) * 0.7548776662466927);\n"
    "    float rr = sqrt(u1), ph = 2.0 * M_PI_F * u2;\n"
    "    float3 d = T * (rr * cos(ph)) + B * (rr * sin(ph)) + N * sqrt(max(0.0, 1.0 - u1));\n"
    "    float jit = fract(n0 + n1 + float(r) * 0.5698402909980533);\n"
    "    for (uint s = 0; s < c.steps; s++) {\n"
    "      float f = (float(s) + jit) / float(c.steps);\n"
    "      float3 Q = O + d * (c.radius * f * f + z * 0.01);\n"
    "      if (Q.z <= 1e-3) break;\n"
    "      float2 qs = gi_proj(Q, c);\n"
    "      if (any(qs < 0.0) || any(qs >= 1.0)) break;\n"
    "      int2 qi = min(int2(qs * ss), mx);\n"
    "      float zs = gi_z(dt, qi, c), dz = Q.z - zs;\n"
    "      if (dz > 0.0 && dz < c.thick + zs * 0.02) { acc += to_lin(col.read(uint2(qi)).rgb); break; }\n"
    "    }\n"
    "  }\n"
    "  out.write(float4(acc / float(c.rays), z), p);\n"
    "}\n"
    // a-trous 3 x 3 filter at the distance st (half-size pixels) with a depth weight
    "kernel void gi_denoise(texture2d<float, access::read> a [[texture(0)]], texture2d<float, access::write> o [[texture(1)]],\n"
    "  constant int &st [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= o.get_width() || p.y >= o.get_height()) return;\n"
    "  int2 mx = int2(int(a.get_width()) - 1, int(a.get_height()) - 1);\n"
    "  float4 cc = a.read(p);\n"
    "  if (cc.a >= SKY * 0.5) { o.write(cc, p); return; }\n"
    "  float3 s = float3(0.0); float ws = 0.0;\n"
    "  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
    "    float4 v = a.read(uint2(clamp(int2(p) + int2(x, y) * st, int2(0), mx)));\n"
    "    float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0) * saturate(1.0 - abs(v.a - cc.a) / (cc.a * 0.03));\n"
    "    s += v.rgb * w; ws += w;\n"
    "  }\n"
    "  o.write(float4(s / ws, cc.a), p);\n"
    "}\n"
    // the half-size result at the full-size pixel ip (view distance z): a 3 x 3 filter with spatial and depth weights
    "static float3 gi_up(texture2d<float, access::read> g, int2 ip, float z) {\n"
    "  int2 h = ip / 2, mx = int2(int(g.get_width()) - 1, int(g.get_height()) - 1);\n"
    "  float3 s = float3(0.0); float ws = 0.0;\n"
    "  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
    "    int2 qh = clamp(h + int2(x, y), int2(0), mx);\n"
    "    float4 v = g.read(uint2(qh));\n"
    "    float2 dd = float2(qh * 2) - float2(ip);\n"
    "    float w = exp(-dot(dd, dd) / 4.5) * max(saturate(1.0 - abs(v.a - z) / (z * 0.03)), 1e-3);\n"
    "    s += v.rgb * w; ws += w;\n"
    "  }\n"
    "  return s / ws;\n"
    "}\n"
    // the composite: the global illumination, the last denoise pass of XeGTAO with the GTAO multi-bounce
    // approximation (the scene color is the albedo), and the contact shadow; the scene pixel is read in place
    // (framebuffer fetch)
    "struct PV { float4 pos [[position]]; };\n"
    "vertex PV enh_vs(uint vid [[vertex_id]]) {\n"
    "  float2 q = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
    "  PV o; o.pos = float4(q * 2.0 - 1.0, 0.0, 1.0); return o;\n"
    "}\n"
    // CC.light: the light in view space (a position, w = 1, or a direction, w = 0); mulx .. db: the view position of
    // a depth value (as vpos); px, py: the size of one pixel in screen coordinates (0 .. 1)
    "struct CC { float beta; uint ao; uint contact; float strength; float4 light; float mulx, muly, addx, addy, da, db, px, py;\n"
    "  uint gi; float gik, pad0, pad1; };\n"
    "static float3 cpos(depth2d<float, access::read> dt, int2 q, constant CC &k) {\n"
    "  q = clamp(q, int2(0), int2(int(dt.get_width()) - 1, int(dt.get_height()) - 1));\n"
    "  float z = k.db / (dt.read(uint2(q)) - k.da);\n"
    "  float2 sp = (float2(q) + 0.5) * float2(k.px, k.py);\n"
    "  return float3((float2(k.mulx, k.muly) * sp + float2(k.addx, k.addy)) * z, z);\n"
    "}\n"
    // The weight of the contact shadow at a pixel, from N.L of the light: 0 on a surface that faces away from the
    // light, 1 from N.L 0.25 (76 degrees from the normal), smoothstep between. The normal comes from the neighbor with
    // the smaller depth step on each axis, toward the camera. The weight is 1 where the normal or the direction to
    // the light has no length.
    "static float contact_nl(depth2d<float, access::read> dt, int2 ip, constant CC &k) {\n"
    "  float3 p0 = cpos(dt, ip, k), pl = cpos(dt, ip - int2(1, 0), k), pr = cpos(dt, ip + int2(1, 0), k);\n"
    "  float3 pt = cpos(dt, ip - int2(0, 1), k), pb = cpos(dt, ip + int2(0, 1), k);\n"
    "  float3 dx = abs(pr.z - p0.z) < abs(p0.z - pl.z) ? pr - p0 : p0 - pl;\n"
    "  float3 dy = abs(pb.z - p0.z) < abs(p0.z - pt.z) ? pb - p0 : p0 - pt;\n"
    "  float3 n = cross(dx, dy);\n"
    "  if (dot(n, p0) > 0.0) n = -n;\n"
    "  float3 l = k.light.w != 0.0 ? k.light.xyz - p0 : k.light.xyz;\n"
    "  float nn = dot(n, n), ll = dot(l, l);\n"
    "  if (!(nn > 1e-24) || !(ll > 1e-24)) return 1.0;\n"
    "  return smoothstep(0.0, 0.25, dot(n, l) * rsqrt(nn) * rsqrt(ll));\n"
    "}\n"
    "fragment float4 enh_fs(PV in [[stage_in]], texture2d<float, access::read> a [[texture(0)]],\n"
    "  texture2d<float, access::read> e [[texture(1)]], texture2d<float, access::read> sh [[texture(2)]],\n"
    "  depth2d<float, access::read> dt [[texture(3)]], texture2d<float, access::read> gt [[texture(4)]],\n"
    "  constant CC &k [[buffer(0)]], float4 dst [[color(0)]]) {\n"
    "  int2 ip = int2(in.pos.xy);\n"
    "  float3 c0 = dst.rgb;\n"
    "  if (k.gi != 0u) {\n"
    "    float3 g = gi_up(gt, ip, cpos(dt, ip, k).z);\n"
    "    float3 cl = to_lin(c0);\n"
    "    float lum = dot(cl, float3(0.2126, 0.7152, 0.0722));\n"
    "    float3 alb = min(0.4 * (lum > 1e-4 ? min(cl / lum, float3(3.0)) : float3(1.0)), float3(0.9));\n"
    "    c0 = to_srgb(cl + k.gik * alb * g);\n"
    "  }\n"
    "  float3 m = float3(1.0);\n"
    "  if (k.ao != 0u) {\n"
    "    float v = denoise(a, e, ip, k.beta);\n"
    "    float3 al = saturate(c0);\n"
    "    float3 ka = 2.0404 * al - 0.3324, kb = -4.7951 * al + 0.6417, kc = 2.7552 * al + 0.6903;\n"
    "    m = max(float3(v), ((v * ka + kb) * v + kc) * v);\n"
    "  }\n"
    "  if (k.contact != 0u) {\n"
    "    float sv = sh.read(uint2(ip)).r;\n"
    "    if (sv < 1.0) m *= mix(1.0, sv, k.strength * contact_nl(dt, ip, k));\n"
    "  }\n"
    "  return float4(c0 * m, dst.a);\n"
    "}\n"
    // dual Kawase bloom. BP.px: one texel of the source (bloom_pre), half a texel of the output (bloom_down,
    // bloom_up, bloom_comp)
    "struct BV { float4 pos [[position]]; float2 uv; };\n"
    "vertex BV bloom_vs(uint vid [[vertex_id]]) {\n"
    "  float2 q = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
    "  BV o; o.pos = float4(q * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(q.x, 1.0 - q.y); return o;\n"
    "}\n"
    "constexpr sampler bls(filter::linear, address::clamp_to_edge);\n"
    // BP.k: the bloom part of the composite; inv: 1 / the number of levels
    "struct BP { float2 px; float k, inv; };\n"
    "static float karis(float3 c) { return 1.0 / (1.0 + max(max(c.r, c.g), c.b)); }\n"
    "static float3 lt(texture2d<float> t, float2 uv) { return to_lin(t.sample(bls, uv).rgb); }\n"
    "fragment float4 bloom_pre(BV in [[stage_in]], texture2d<float> t [[texture(0)]], constant BP &p [[buffer(0)]]) {\n"
    "  float2 uv = in.uv, o = p.px;\n"
    "  float3 a = lt(t, uv + o * float2(-2.0, -2.0)), b = lt(t, uv + o * float2(0.0, -2.0)), c = lt(t, uv + o * float2(2.0, -2.0));\n"
    "  float3 d = lt(t, uv + o * float2(-1.0, -1.0)), e = lt(t, uv + o * float2(1.0, -1.0));\n"
    "  float3 f = lt(t, uv + o * float2(-2.0, 0.0)), g = lt(t, uv), h = lt(t, uv + o * float2(2.0, 0.0));\n"
    "  float3 i = lt(t, uv + o * float2(-1.0, 1.0)), j = lt(t, uv + o * float2(1.0, 1.0));\n"
    "  float3 k = lt(t, uv + o * float2(-2.0, 2.0)), l = lt(t, uv + o * float2(0.0, 2.0)), m = lt(t, uv + o * float2(2.0, 2.0));\n"
    "  float3 b0 = (d + e + i + j) * 0.25, b1 = (a + b + f + g) * 0.25, b2 = (b + c + g + h) * 0.25;\n"
    "  float3 b3 = (f + g + k + l) * 0.25, b4 = (g + h + l + m) * 0.25;\n"
    "  float w0 = 0.5 * karis(b0), w1 = 0.125 * karis(b1), w2 = 0.125 * karis(b2), w3 = 0.125 * karis(b3), w4 = 0.125 * karis(b4);\n"
    "  return float4((b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / (w0 + w1 + w2 + w3 + w4), 1.0);\n"
    "}\n"
    "fragment float4 bloom_down(BV in [[stage_in]], texture2d<float> t [[texture(0)]], constant BP &p [[buffer(0)]]) {\n"
    "  float2 uv = in.uv, hp = p.px;\n"
    "  float4 s = t.sample(bls, uv) * 4.0 + t.sample(bls, uv - hp) + t.sample(bls, uv + hp);\n"
    "  s += t.sample(bls, uv + float2(hp.x, -hp.y)) + t.sample(bls, uv - float2(hp.x, -hp.y));\n"
    "  return s / 8.0;\n"
    "}\n"
    "static float3 up8(texture2d<float> t, float2 uv, float2 hp) {\n"
    "  float3 s = t.sample(bls, uv + float2(-hp.x * 2.0, 0.0)).rgb + t.sample(bls, uv + float2(hp.x * 2.0, 0.0)).rgb;\n"
    "  s += t.sample(bls, uv + float2(0.0, hp.y * 2.0)).rgb + t.sample(bls, uv + float2(0.0, -hp.y * 2.0)).rgb;\n"
    "  s += (t.sample(bls, uv + float2(-hp.x, hp.y)).rgb + t.sample(bls, uv + float2(hp.x, hp.y)).rgb) * 2.0;\n"
    "  s += (t.sample(bls, uv + float2(hp.x, -hp.y)).rgb + t.sample(bls, uv + float2(-hp.x, -hp.y)).rgb) * 2.0;\n"
    "  return s / 12.0;\n"
    "}\n"
    "fragment float4 bloom_up(BV in [[stage_in]], texture2d<float> t [[texture(0)]], constant BP &p [[buffer(0)]]) {\n"
    "  return float4(up8(t, in.uv, p.px), 0.0);\n"
    "}\n"
    // the composite: the scene pixel is read in place (framebuffer fetch)
    // temporal anti-aliasing: TC.r = the rows of inverse(this frame's view-projection) x the last frame's view-projection
    // (row vectors); TC.jclip = the sub-pixel offset of this frame in clip space; valid = 0: no camera motion is known
    "struct TC { float4 r[4]; float2 size, jclip; uint valid, pad0, pad1, pad2; };\n"
    "kernel void taa_motion(depth2d<float, access::read> dt [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],\n"
    "  texture2d<float, access::write> dz [[texture(2)]], constant TC &k [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {\n"
    "  if (gid.x >= uint(k.size.x) || gid.y >= uint(k.size.y)) return;\n"
    "  float z = dt.read(gid);\n"
    "  dz.write(float4(z, 0.0, 0.0, 0.0), gid);\n"
    "  float2 m = float2(0.0);\n"
    "  if (k.valid != 0u) {\n"
    // the pixel center in clip space without the offset of this frame
    "    float2 ndc = float2((float(gid.x) + 0.5) / k.size.x * 2.0 - 1.0, 1.0 - (float(gid.y) + 0.5) / k.size.y * 2.0) - k.jclip;\n"
    "    float4 p = ndc.x * k.r[0] + ndc.y * k.r[1] + z * k.r[2] + k.r[3];\n"
    // the motion in pixels (y down) to the position of the last frame
    "    if (p.w > 1e-6) { float2 q = p.xy / p.w; m = float2((q.x - ndc.x) * 0.5 * k.size.x, (ndc.y - q.y) * 0.5 * k.size.y); }\n"
    "  }\n"
    "  mv.write(float4(m, 0.0, 0.0), gid);\n"
    "}\n"
    "fragment float4 bloom_comp(BV in [[stage_in]], texture2d<float> t [[texture(0)]], constant BP &p [[buffer(0)]],\n"
    "  float4 dst [[color(0)]]) {\n"
    "  return float4(to_srgb(mix(to_lin(dst.rgb), up8(t, in.uv, p.px) * p.inv, p.k)), dst.a);\n"
    "}\n";

typedef struct { float sx, sy, px, py, mulx, muly, addx, addy, mpx, mpy, da, db, radius, falloff, power, mipoff, beta, pad; } AOC;
typedef struct { float light[4]; int32_t offset[2]; float far_z, near_z, thickness, bilinear, contrast, pad, da, db, fade_start, fade_end; } SSSP;
typedef struct { float beta; uint32_t ao, contact; float strength; float light[4]; float mulx, muly, addx, addy, da, db, px, py;
                 uint32_t gi; float gik, pad0, pad1; } CC;
typedef struct { float mulx, muly, addx, addy, da, db, px, py, radius, thick, sw, sh; uint32_t rays, steps, pad0, pad1; } GIC;
typedef struct { int32_t count[3]; int32_t offset[2]; } SssDispatch;

typedef struct { float px[2]; float k, inv; } BP;
typedef struct { float r[4][4]; float size[2], jclip[2]; uint32_t valid, pad[3]; } TC;

static const uint32_t ao_slices[4] = {0, 1, 2, 3}, ao_steps[4] = {0, 2, 2, 3};
// the strength of each level: the effect radius in meters and the XeGTAO final value power
static const float ao_radius[4] = {0.0f, 0.8f, 1.2f, 1.6f}, ao_power[4] = {0.0f, 2.4f, 3.0f, 3.6f};
#define BLOOM_LEVELS 6
static const float bloom_intensity[4] = {0.0f, 0.04f, 0.08f, 0.15f};   // k of the composite
// the global illumination levels: rays per pixel, steps per ray, strength (UNCONFIRMED tuning)
static const uint32_t gi_rays[4] = {0, 2, 3, 4}, gi_steps[4] = {0, 12, 16, 20};
static const float gi_strength[4] = {0.0f, 0.5f, 1.0f, 1.5f};

static struct {
    id<MTLDevice> dev;
    id<MTLLibrary> lib;
    id<MTLComputePipelineState> prefilter, main_pso[4], denoise, sss, gi_main, gi_denoise;
    id<MTLRenderPipelineState> apply; MTLPixelFormat apply_fmt;
    id<MTLTexture> hilbert, depths, mip[AO_MIPS], ao0, ao1, edges, shadow, gi0, gi1;
    id<MTLRenderPipelineState> bloom_pre, bloom_down, bloom_up, bloom_comp; MTLPixelFormat bloom_comp_fmt;
    id<MTLTexture> bloom_lv[BLOOM_LEVELS];
    id<MTLComputePipelineState> taa_motion;
    id<MTLFXTemporalScaler> taa; NSUInteger taa_w, taa_h; MTLPixelFormat taa_fmt;
    id<MTLTexture> taa_mv, taa_dz, taa_out;
    int logged_taa, logged_taa_ms;
    int logged_gi, logged_ao, logged_sss, logged_cam, logged_ms, logged_bloom, logged_bloom_ms;
} E;

static id<MTLComputePipelineState> kernel_of(NSString *name, MTLFunctionConstantValues *cv) {
    NSError *err = nil;
    id<MTLFunction> f = cv ? [E.lib newFunctionWithName:name constantValues:cv error:&err] : [E.lib newFunctionWithName:name];
    if (!f) rt_fatal("[d3d9] enhancement function %s is missing: %s", name.UTF8String, err ? err.localizedDescription.UTF8String : "");
    id<MTLComputePipelineState> p = [E.dev newComputePipelineStateWithFunction:f error:&err];
    if (!p) rt_fatal("[d3d9] enhancement pipeline %s not created: %s", name.UTF8String, err.localizedDescription.UTF8String);
    return p;
}
// the Hilbert curve index of each pixel of a 64 x 64 tile (the XeGTAO noise)
static id<MTLTexture> hilbert_texture(void) {
    static uint16_t data[64 * 64];
    for (uint32_t py = 0; py < 64; py++)
        for (uint32_t px = 0; px < 64; px++) {
            uint32_t x = px, y = py, index = 0;
            for (uint32_t lvl = 32; lvl > 0; lvl /= 2) {
                uint32_t rx = (x & lvl) ? 1u : 0u, ry = (y & lvl) ? 1u : 0u;
                index += lvl * lvl * ((3u * rx) ^ ry);
                if (ry == 0) {
                    if (rx == 1) { x = 63 - x; y = 63 - y; }
                    uint32_t t = x; x = y; y = t;
                }
            }
            data[py * 64 + px] = (uint16_t)index;
        }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Uint width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [E.dev newTextureWithDescriptor:td];
    if (!t) rt_fatal("[d3d9] out of GPU memory for the ambient occlusion noise texture");
    [t replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:data bytesPerRow:64 * sizeof(uint16_t)];
    return t;
}
static void setup(id<MTLDevice> dev) {
    if (E.lib) return;
    E.dev = dev;
    NSError *err = nil;
    E.lib = [dev newLibraryWithSource:@(enh_src) options:[MTLCompileOptions new] error:&err];
    if (!E.lib) rt_fatal("[d3d9] the enhancement shaders did not compile: %s", err.localizedDescription.UTF8String);
    E.prefilter = kernel_of(@"ao_prefilter", nil);
    for (int q = 1; q < 4; q++) {
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        [cv setConstantValue:&ao_slices[q] type:MTLDataTypeUInt atIndex:0];
        [cv setConstantValue:&ao_steps[q] type:MTLDataTypeUInt atIndex:1];
        E.main_pso[q] = kernel_of(@"ao_main", cv);
    }
    E.denoise = kernel_of(@"ao_denoise", nil);
    E.sss = kernel_of(@"sss_main", nil);
    if (E.sss.maxTotalThreadsPerThreadgroup < SSS_WAVE) rt_fatal("[d3d9] the contact shadow kernel cannot run %d threads in a group", SSS_WAVE);
    E.hilbert = hilbert_texture();
    E.taa_motion = kernel_of(@"taa_motion", nil);
    E.gi_main = kernel_of(@"gi_main", nil);
    E.gi_denoise = kernel_of(@"gi_denoise", nil);
}
static id<MTLTexture> tex_of(id<MTLTexture> __strong *slot, NSUInteger w, NSUInteger h, MTLPixelFormat fmt, NSUInteger mips) {
    id<MTLTexture> t = *slot;
    if (t && t.width == w && t.height == h && t.pixelFormat == fmt && t.mipmapLevelCount == mips) return t;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.mipmapLevelCount = mips; td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [E.dev newTextureWithDescriptor:td];
    if (!t) rt_fatal("[d3d9] out of GPU memory for an enhancement texture (%lux%lu)", (unsigned long)w, (unsigned long)h);
    *slot = t;
    return t;
}
static id<MTLRenderPipelineState> apply_pso(MTLPixelFormat fmt) {
    if (E.apply && E.apply_fmt == fmt) return E.apply;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [E.lib newFunctionWithName:@"enh_vs"];
    pd.fragmentFunction = [E.lib newFunctionWithName:@"enh_fs"];
    pd.colorAttachments[0].pixelFormat = fmt;
    pd.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    NSError *err = nil;
    E.apply = [E.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!E.apply) rt_fatal("[d3d9] the enhancement composite pipeline was not created: %s", err.localizedDescription.UTF8String);
    E.apply_fmt = fmt;
    return E.apply;
}

// ---- Bend dispatch list: the screen in four quadrants around the light, each split in two on its long axis ----
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int dispatch_list(const float lp[4], int w, int h, float light_out[4], SssDispatch *list) {
    float lw = lp[3], lim = 0.000002f * (float)SSS_WAVE;
    if (lw >= 0.0f && lw < lim) lw = lim;
    else if (lw < 0.0f && lw > -lim) lw = -lim;
    light_out[0] = ((lp[0] / lw) * 0.5f + 0.5f) * (float)w;
    light_out[1] = ((lp[1] / lw) * -0.5f + 0.5f) * (float)h;
    light_out[2] = lp[3] == 0.0f ? 0.0f : lp[2] / lp[3];
    light_out[3] = lp[3] > 0.0f ? 1.0f : -1.0f;
    int lx = (int)(light_out[0] + 0.5f), ly = (int)(light_out[1] + 0.5f);
    int bb[4] = {0 - lx, -(h - ly), w - lx, -(0 - ly)};   // inclusive bounds relative to the light (y up)
    int n = 0;
    for (int q = 0; q < 4; q++) {
        int vert = q == 0 || q == 3;
        int r0 = imax(0, (q & 1) ? bb[0] : -bb[2]) / SSS_WAVE;
        int r1 = imax(0, (q & 2) ? bb[1] : -bb[3]) / SSS_WAVE;
        int r2 = imax(0, ((q & 1) ? bb[2] : -bb[0]) + SSS_WAVE * (vert ? 1 : 2) - 1) / SSS_WAVE;
        int r3 = imax(0, ((q & 2) ? bb[3] : -bb[1]) + SSS_WAVE * (vert ? 2 : 1) - 1) / SSS_WAVE;
        if (r2 - r0 <= 0 || r3 - r1 <= 0) continue;
        SssDispatch *a = &list[n++];
        a->count[0] = SSS_WAVE; a->count[1] = r2 - r0; a->count[2] = r3 - r1;
        a->offset[0] = ((q & 1) ? r0 : -r2) + ((q == 2 || q == 3) ? 1 : 0);
        a->offset[1] = ((q & 2) ? -r3 : r1) + ((q == 1 || q == 3) ? 1 : 0);
        int diag = bb[0] - bb[1];
        if (q == 1) diag = bb[2] + bb[1];
        if (q == 2) diag = -bb[0] - bb[3];
        if (q == 3) diag = -bb[2] + bb[3];
        diag = (diag + SSS_WAVE - 1) / SSS_WAVE;
        if (diag <= 0) continue;
        SssDispatch *b = &list[n++];
        *b = *a;
        if (q == 0) { b->count[2] = imin(a->count[2], diag); a->count[2] -= b->count[2]; b->offset[1] = a->offset[1] + a->count[2]; b->offset[0]--; b->count[1]++; }
        else if (q == 1) { b->count[1] = imin(a->count[1], diag); a->count[1] -= b->count[1]; b->offset[0] = a->offset[0] + a->count[1]; b->count[2]++; }
        else if (q == 2) { b->count[1] = imin(a->count[1], diag); a->count[1] -= b->count[1]; a->offset[0] += b->count[1]; b->count[2]++; b->offset[1]--; }
        else { b->count[2] = imin(a->count[2], diag); a->count[2] -= b->count[2]; a->offset[1] += b->count[2]; b->count[1]++; }
        if (b->count[1] <= 0 || b->count[2] <= 0) *b = list[--n];
        if (a->count[1] <= 0 || a->count[2] <= 0) *a = list[--n];
    }
    for (int i = 0; i < n; i++) { list[i].offset[0] *= SSS_WAVE; list[i].offset[1] *= SSS_WAVE; }
    return n;
}

int d3d9_enh_scene(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> color, id<MTLTexture> depth,
                   const EnhScene *s, int ao, int contact, int gi) {
    if (ao < 0) ao = 0;
    if (ao > 3) ao = 3;
    if (gi < 0) gi = 0;
    if (gi > 3) gi = 3;
    if (!s->have_light) contact = 0;
    if (!ao && !contact && !gi) return 0;
    NSUInteger w = depth.width, h = depth.height;
    if (color.sampleCount > 1 || depth.sampleCount > 1 || color.width != w || color.height != h || w < 16 || h < 16) {
        if (!E.logged_ms) rt_log("[d3d9] ambient occlusion / contact shadows: the scene is multisampled or its depth buffer (%lux%lu) "
                                 "differs from the back buffer (%lux%lu); the frame has no ambient occlusion or contact shadows",
                                 (unsigned long)w, (unsigned long)h, (unsigned long)color.width, (unsigned long)color.height);
        E.logged_ms = 1;
        return 0;
    }
    const float *P = s->proj;
    // perspective, centered: w = z * P23 with P33 = 0, P20 = P21 = 0
    if (fabsf(P[15]) > 1e-6f || fabsf(P[11]) < 1e-6f || fabsf(P[8]) > 1e-4f || fabsf(P[9]) > 1e-4f || fabsf(P[0]) < 1e-6f || fabsf(P[5]) < 1e-6f) {
        if (!E.logged_cam) rt_log("[d3d9] ambient occlusion / contact shadows: the projection is not a centered perspective "
                                  "projection (P20 %g, P21 %g, P23 %g, P33 %g); such frames have no ambient occlusion or contact shadows",
                                  P[8], P[9], P[11], P[15]);
        E.logged_cam = 1;
        return 0;
    }
    float da = P[10] / P[11], db = P[14];
    @autoreleasepool {
        setup(dev);
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        if (!ce) rt_fatal("[d3d9] Metal did not create the enhancement compute encoder");
        CC k = {1.2f, (uint32_t)(ao != 0), (uint32_t)(contact != 0), CONTACT_STRENGTH, {0, 0, 0, 0},
                2.0f / P[0], -2.0f / P[5], -1.0f / P[0], 1.0f / P[5], da, db, 1.0f / (float)w, 1.0f / (float)h,
                (uint32_t)(gi != 0), gi_strength[gi], 0.0f, 0.0f};
        // the light in view space: light * view (row vectors)
        for (int cc = 0; cc < 3; cc++) {
            float a = 0; for (int j = 0; j < 4; j++) a += s->light[j] * s->view[j * 4 + cc];
            k.light[cc] = a;
        }
        k.light[3] = s->light[3];
        if (gi) {
            if (!(color.usage & MTLTextureUsageShaderRead))
                rt_fatal("[d3d9] global illumination: the back buffer texture cannot be read by a shader (usage 0x%lx)", (unsigned long)color.usage);
            NSUInteger hw = (w + 1) / 2, hh = (h + 1) / 2;
            tex_of(&E.gi0, hw, hh, MTLPixelFormatRGBA16Float, 1);
            tex_of(&E.gi1, hw, hh, MTLPixelFormatRGBA16Float, 1);
            GIC g = {k.mulx, k.muly, k.addx, k.addy, da, db, k.px, k.py, GI_RADIUS, GI_THICK, (float)w, (float)h,
                     gi_rays[gi], gi_steps[gi], 0, 0};
            if (!E.logged_gi) {
                rt_log("[d3d9] screen-space global illumination %lux%lu (computed at %lux%lu): %u rays x %u steps, strength %.2f",
                       (unsigned long)w, (unsigned long)h, (unsigned long)hw, (unsigned long)hh, gi_rays[gi], gi_steps[gi], gi_strength[gi]);
                E.logged_gi = 1;
            }
            [ce setComputePipelineState:E.gi_main];
            [ce setBytes:&g length:sizeof g atIndex:0];
            [ce setTexture:depth atIndex:0];
            [ce setTexture:color atIndex:1];
            [ce setTexture:E.gi0 atIndex:2];
            [ce setTexture:E.hilbert atIndex:3];
            [ce dispatchThreads:MTLSizeMake(hw, hh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            // three a-trous passes (distances 1, 2, 4): gi0 -> gi1 -> gi0 -> gi1
            [ce setComputePipelineState:E.gi_denoise];
            for (int i = 0; i < 3; i++) {
                int32_t st = 1 << i;
                [ce setBytes:&st length:sizeof st atIndex:0];
                [ce setTexture:(i & 1) ? E.gi1 : E.gi0 atIndex:0];
                [ce setTexture:(i & 1) ? E.gi0 : E.gi1 atIndex:1];
                [ce dispatchThreads:MTLSizeMake(hw, hh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            }
        }
        if (ao) {
            tex_of(&E.ao0, w, h, MTLPixelFormatR16Float, 1);
            tex_of(&E.ao1, w, h, MTLPixelFormatR16Float, 1);
            tex_of(&E.edges, w, h, MTLPixelFormatRGBA8Unorm, 1);
            if (!E.depths || E.depths.width != w || E.depths.height != h) {
                tex_of(&E.depths, w, h, MTLPixelFormatR32Float, AO_MIPS);
                for (NSUInteger i = 0; i < AO_MIPS; i++) {
                    E.mip[i] = [E.depths newTextureViewWithPixelFormat:MTLPixelFormatR32Float textureType:MTLTextureType2D
                                                                levels:NSMakeRange(i, 1) slices:NSMakeRange(0, 1)];
                    if (!E.mip[i]) rt_fatal("[d3d9] Metal did not create an ambient occlusion depth mip view");
                }
            }
            AOC c; memset(&c, 0, sizeof c);
            c.sx = (float)w; c.sy = (float)h; c.px = 1.0f / (float)w; c.py = 1.0f / (float)h;
            c.mulx = 2.0f / P[0]; c.muly = -2.0f / P[5]; c.addx = -1.0f / P[0]; c.addy = 1.0f / P[5];
            c.mpx = c.mulx * c.px; c.mpy = c.muly * c.py;
            c.da = da; c.db = db;
            c.radius = ao_radius[ao] * UNIT_SCALE * 1.457f; c.falloff = 0.615f; c.power = ao_power[ao]; c.mipoff = 3.30f; c.beta = 1.2f;
            if (!E.logged_ao) { rt_log("[d3d9] XeGTAO ambient occlusion %lux%lu: %u slices x %u steps", (unsigned long)w, (unsigned long)h, ao_slices[ao], ao_steps[ao]); E.logged_ao = 1; }
            [ce setBytes:&c length:sizeof c atIndex:0];
            [ce setTexture:depth atIndex:0];
            for (NSUInteger i = 0; i < AO_MIPS; i++) [ce setTexture:E.mip[i] atIndex:1 + i];
            [ce setComputePipelineState:E.prefilter];
            [ce dispatchThreadgroups:MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [ce setTexture:E.depths atIndex:0];
            [ce setTexture:E.ao0 atIndex:1];
            [ce setTexture:E.edges atIndex:2];
            [ce setTexture:E.hilbert atIndex:3];
            [ce setComputePipelineState:E.main_pso[ao]];
            [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            // the first denoise pass (less weight on the center); the second is the composite
            float beta = c.beta / 5.0f;
            [ce setBytes:&beta length:sizeof beta atIndex:0];
            [ce setTexture:E.ao0 atIndex:0];
            [ce setTexture:E.edges atIndex:1];
            [ce setTexture:E.ao1 atIndex:2];
            [ce setComputePipelineState:E.denoise];
            [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        }
        if (contact) {
            tex_of(&E.shadow, w, h, MTLPixelFormatR8Unorm, 1);
            // the light through the camera: clip = light * view * proj (row vectors)
            float vp[16], lp[4];
            for (int r = 0; r < 4; r++) for (int cc = 0; cc < 4; cc++) {
                float a = 0; for (int j = 0; j < 4; j++) a += s->view[r * 4 + j] * P[j * 4 + cc];
                vp[r * 4 + cc] = a;
            }
            for (int cc = 0; cc < 4; cc++) { float a = 0; for (int j = 0; j < 4; j++) a += s->light[j] * vp[j * 4 + cc]; lp[cc] = a; }
            SSSP p; memset(&p, 0, sizeof p);
            SssDispatch list[SSS_MAX_DISPATCH];
            int n = dispatch_list(lp, (int)w, (int)h, p.light, list);
            p.far_z = 1.0f; p.near_z = 0.0f; p.thickness = 0.005f; p.bilinear = 0.02f; p.contrast = 4.0f;
            p.da = da; p.db = db; p.fade_start = 40.0f * UNIT_SCALE; p.fade_end = 80.0f * UNIT_SCALE;
            if (!E.logged_sss) { rt_log("[d3d9] Bend contact shadows %lux%lu: light (%.1f %.1f %.1f %.1f), %d dispatches in the first frame",
                                        (unsigned long)w, (unsigned long)h, s->light[0], s->light[1], s->light[2], s->light[3], n); E.logged_sss = 1; }
            [ce setComputePipelineState:E.sss];
            [ce setTexture:depth atIndex:0];
            [ce setTexture:E.shadow atIndex:1];
            for (int j = 0; j < n; j++) {
                p.offset[0] = list[j].offset[0]; p.offset[1] = list[j].offset[1];
                [ce setBytes:&p length:sizeof p atIndex:0];
                [ce dispatchThreadgroups:MTLSizeMake((NSUInteger)list[j].count[0], (NSUInteger)list[j].count[1], (NSUInteger)list[j].count[2])
                   threadsPerThreadgroup:MTLSizeMake(SSS_WAVE, 1, 1)];
            }
        }
        [ce endEncoding];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = color;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        if (!re) rt_fatal("[d3d9] Metal did not create the enhancement composite encoder");
        [re setRenderPipelineState:apply_pso(color.pixelFormat)];
        // every texture argument is bound; the feature flags in k select the ones the shader reads. `any` is a texture
        // of an effect of this frame (the arguments of the effects that are off get it)
        id<MTLTexture> any = ao ? E.ao1 : contact ? E.shadow : E.gi1;
        [re setFragmentTexture:ao ? E.ao1 : any atIndex:0];
        [re setFragmentTexture:ao ? E.edges : any atIndex:1];
        [re setFragmentTexture:contact ? E.shadow : any atIndex:2];
        [re setFragmentTexture:depth atIndex:3];
        [re setFragmentTexture:gi ? E.gi1 : any atIndex:4];
        [re setFragmentBytes:&k length:sizeof k atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
    }
    return 1;
}

// ---- dual Kawase bloom ----
static id<MTLRenderPipelineState> bloom_pso(NSString *fs, MTLPixelFormat fmt, int add, int rgb_only) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [E.lib newFunctionWithName:@"bloom_vs"];
    pd.fragmentFunction = [E.lib newFunctionWithName:fs];
    if (!pd.vertexFunction || !pd.fragmentFunction) rt_fatal("[d3d9] bloom function %s is missing", fs.UTF8String);
    pd.colorAttachments[0].pixelFormat = fmt;
    if (rgb_only) pd.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    if (add) {
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [E.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) rt_fatal("[d3d9] bloom pipeline %s not created: %s", fs.UTF8String, err.localizedDescription.UTF8String);
    return p;
}
// one full-screen pass from src into dst; load: dst keeps its content (additive passes and the composite)
static void bloom_pass(id<MTLCommandBuffer> cb, id<MTLRenderPipelineState> pso, id<MTLTexture> src, id<MTLTexture> dst,
                       const BP *p, int load) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    if (!re) rt_fatal("[d3d9] Metal did not create a bloom encoder");
    re.label = @"Dual Kawase bloom";
    [re setRenderPipelineState:pso];
    [re setFragmentTexture:src atIndex:0];
    [re setFragmentBytes:p length:sizeof *p atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
}
int d3d9_enh_bloom(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> color, int level) {
    if (level <= 0) return 0;
    if (level > 3) level = 3;
    NSUInteger w = color.width, h = color.height;
    if (color.sampleCount > 1 || w < 128 || h < 128) {
        if (!E.logged_bloom_ms) rt_log("[d3d9] bloom: the back buffer (%lux%lu, %lu samples) is multisampled or smaller than 128x128; "
                                       "such frames have no bloom", (unsigned long)w, (unsigned long)h, (unsigned long)color.sampleCount);
        E.logged_bloom_ms = 1;
        return 0;
    }
    @autoreleasepool {
        setup(dev);
        if (!E.bloom_pre) {
            E.bloom_pre = bloom_pso(@"bloom_pre", MTLPixelFormatRGBA16Float, 0, 0);
            E.bloom_down = bloom_pso(@"bloom_down", MTLPixelFormatRGBA16Float, 0, 0);
            E.bloom_up = bloom_pso(@"bloom_up", MTLPixelFormatRGBA16Float, 1, 0);
        }
        if (!E.bloom_comp || E.bloom_comp_fmt != color.pixelFormat) {
            E.bloom_comp = bloom_pso(@"bloom_comp", color.pixelFormat, 0, 1);
            E.bloom_comp_fmt = color.pixelFormat;
        }
        for (int i = 0; i < BLOOM_LEVELS; i++) {
            NSUInteger lw = w >> (i + 1), lh = h >> (i + 1);
            if (lw < 1) lw = 1;
            if (lh < 1) lh = 1;
            id<MTLTexture> t = E.bloom_lv[i];
            if (t && t.width == lw && t.height == lh) continue;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:lw height:lh mipmapped:NO];
            td.storageMode = MTLStorageModePrivate;
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            E.bloom_lv[i] = [E.dev newTextureWithDescriptor:td];
            if (!E.bloom_lv[i]) rt_fatal("[d3d9] out of GPU memory for a bloom texture (%lux%lu)", (unsigned long)lw, (unsigned long)lh);
        }
        if (!E.logged_bloom) {
            rt_log("[d3d9] dual Kawase bloom %lux%lu: %d levels from %lux%lu, intensity %.2f", (unsigned long)w, (unsigned long)h,
                   BLOOM_LEVELS, (unsigned long)E.bloom_lv[0].width, (unsigned long)E.bloom_lv[0].height, bloom_intensity[level]);
            E.logged_bloom = 1;
        }
        BP p; memset(&p, 0, sizeof p);
        p.px[0] = 1.0f / (float)w; p.px[1] = 1.0f / (float)h;
        bloom_pass(cb, E.bloom_pre, color, E.bloom_lv[0], &p, 0);
        for (int i = 1; i < BLOOM_LEVELS; i++) {
            id<MTLTexture> out = E.bloom_lv[i];
            p.px[0] = 0.5f / (float)out.width; p.px[1] = 0.5f / (float)out.height;
            bloom_pass(cb, E.bloom_down, E.bloom_lv[i - 1], out, &p, 0);
        }
        // up: each level adds the up-sampled sum of the levels below it
        for (int i = BLOOM_LEVELS - 2; i >= 0; i--) {
            id<MTLTexture> out = E.bloom_lv[i];
            p.px[0] = 0.5f / (float)out.width; p.px[1] = 0.5f / (float)out.height;
            bloom_pass(cb, E.bloom_up, E.bloom_lv[i + 1], out, &p, 1);
        }
        p.px[0] = 0.5f / (float)w; p.px[1] = 0.5f / (float)h;
        p.k = bloom_intensity[level]; p.inv = 1.0f / (float)BLOOM_LEVELS;
        bloom_pass(cb, E.bloom_comp, E.bloom_lv[0], color, &p, 1);
    }
    return 1;
}

// ---- temporal anti-aliasing (MetalFX) ----
static id<MTLTexture> taa_tex(NSUInteger w, NSUInteger h, MTLPixelFormat fmt, MTLTextureUsage need, const char *what) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = need | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> t = [E.dev newTextureWithDescriptor:td];
    if (!t) rt_fatal("[d3d9] out of GPU memory for the anti-aliasing %s texture (%lux%lu)", what, (unsigned long)w, (unsigned long)h);
    return t;
}
static void taa_setup(NSUInteger w, NSUInteger h, MTLPixelFormat fmt) {
    if (E.taa && E.taa_w == w && E.taa_h == h && E.taa_fmt == fmt) return;
    if (![MTLFXTemporalScalerDescriptor supportsDevice:E.dev]) rt_fatal("[d3d9] this GPU does not support the MetalFX temporal scaler (anti-aliasing)");
    float mn = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:E.dev];
    if (!(mn <= 1.0f)) rt_fatal("[d3d9] the MetalFX temporal scaler of this GPU needs a scale of at least %g; anti-aliasing needs 1", mn);
    MTLFXTemporalScalerDescriptor *dd = [MTLFXTemporalScalerDescriptor new];
    dd.inputWidth = w; dd.inputHeight = h; dd.outputWidth = w; dd.outputHeight = h;
    dd.colorTextureFormat = fmt; dd.outputTextureFormat = fmt;
    dd.depthTextureFormat = MTLPixelFormatR32Float; dd.motionTextureFormat = MTLPixelFormatRG16Float;
    dd.autoExposureEnabled = NO;
    dd.requiresSynchronousInitialization = YES;
    id<MTLFXTemporalScaler> sc = [dd newTemporalScalerWithDevice:E.dev];
    if (!sc) rt_fatal("[d3d9] the MetalFX temporal scaler (anti-aliasing) was not created for %lux%lu, format %lu",
                      (unsigned long)w, (unsigned long)h, (unsigned long)fmt);
    E.taa = sc; E.taa_w = w; E.taa_h = h; E.taa_fmt = fmt;
    E.taa_mv = taa_tex(w, h, MTLPixelFormatRG16Float, sc.motionTextureUsage, "motion");
    E.taa_dz = taa_tex(w, h, MTLPixelFormatR32Float, sc.depthTextureUsage, "depth");
    E.taa_out = taa_tex(w, h, fmt, sc.outputTextureUsage, "output");
    sc.motionVectorScaleX = 1.0f; sc.motionVectorScaleY = 1.0f;
    sc.inputContentWidth = w; sc.inputContentHeight = h;
    sc.depthReversed = NO;
    rt_log("[d3d9] MetalFX temporal anti-aliasing %lux%lu, format %lu", (unsigned long)w, (unsigned long)h, (unsigned long)fmt);
}
int d3d9_enh_taa(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> color, id<MTLTexture> depth, const float *reproj,
                 float jx, float jy, int reset) {
    NSUInteger w = color.width, h = color.height;
    if (color.sampleCount > 1 || depth.sampleCount > 1 || depth.width != w || depth.height != h) {
        if (!E.logged_taa_ms) rt_log("[d3d9] anti-aliasing: the scene is multisampled or its depth buffer (%lux%lu) differs from the "
                                     "back buffer (%lux%lu); such frames have no anti-aliasing", (unsigned long)depth.width,
                                     (unsigned long)depth.height, (unsigned long)w, (unsigned long)h);
        E.logged_taa_ms = 1;
        return 0;
    }
    @autoreleasepool {
        setup(dev);
        taa_setup(w, h, color.pixelFormat);
        id<MTLFXTemporalScaler> sc = E.taa;
        if ((color.usage & sc.colorTextureUsage) != sc.colorTextureUsage)
            rt_fatal("[d3d9] anti-aliasing: the back buffer texture lacks the usage 0x%lx that MetalFX needs", (unsigned long)sc.colorTextureUsage);
        TC k; memset(&k, 0, sizeof k);
        if (reproj) { memcpy(k.r, reproj, sizeof k.r); k.valid = 1; }
        k.size[0] = (float)w; k.size[1] = (float)h;
        k.jclip[0] = 2.0f * jx / (float)w; k.jclip[1] = -2.0f * jy / (float)h;
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        ce.label = @"Anti-aliasing motion";
        [ce setComputePipelineState:E.taa_motion];
        [ce setTexture:depth atIndex:0]; [ce setTexture:E.taa_mv atIndex:1]; [ce setTexture:E.taa_dz atIndex:2];
        [ce setBytes:&k length:sizeof k atIndex:0];
        [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [ce endEncoding];
        sc.colorTexture = color; sc.depthTexture = E.taa_dz; sc.motionTexture = E.taa_mv; sc.outputTexture = E.taa_out;
        sc.jitterOffsetX = jx; sc.jitterOffsetY = jy;
        sc.reset = reset ? YES : NO;
        [sc encodeToCommandBuffer:cb];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        be.label = @"Anti-aliasing output";
        [be copyFromTexture:E.taa_out toTexture:color];
        [be endEncoding];
        if (!E.logged_taa) { rt_log("[d3d9] anti-aliasing: first frame (jitter %.3f %.3f, %s)", jx, jy, reproj ? "camera motion" : "no camera motion"); E.logged_taa = 1; }
    }
    return 1;
}
