// The inputs and the output of the NRD REBLUR_DIFFUSE denoiser of the global illumination (d3d9_nrd.mm). This file
// is part of the Recomp-Framework runtime. It uses the front-end and back-end functions of NRD.hlsli (NVIDIA NRD, not
// changed) for the NRD encodings. tools/nrd_msl.py compiles the two kernels with Slang and SPIRV-Cross (nrd.mk).
//
// All textures are at the size of the global illumination (half the size of the back buffer). The registers are
// the Metal argument indices (SPIR-V binding = register, --msl-decoration-binding).
#include "NRD.hlsli"

#define SKY 1.0e6                       // the view distance of a pixel with no surface (d3d9_enhance.m)
#define CHAR_CONFIDENCE 0.25            // the history confidence of a character pixel (its motion is not known)

cbuffer NrdPack : register(b15)
{
    float4 gReproj[4];      // the rows of inverse(this frame's view-projection) x the last frame's (row vectors)
    float4 gViewToWorld[3]; // the rows of the rotation from view space to world space (row vectors)
    float4 gHitDist;        // xyz: the REBLUR hit distance parameters (ReblurHitDistanceParameters A, B, C)
    float4 gSize;           // x, y: the size of the back buffer; z, w: da, db (depth = da + db / view distance)
    uint4 gFlags;           // x: the reprojection is valid; y: the character mask is valid
};

// nrd_pack: gGi rgb the light of the rays of this frame (gi_main), a their mean hit distance; gNz xyz the view normal
// (0: none), w the view distance; gNzPrev the same for the last frame; gMask the stencil of the back buffer (bit 0x80:
// a character pixel)
Texture2D<float4> gGi : register(t0);
Texture2D<float4> gNz : register(t1);
Texture2D<float4> gNzPrev : register(t2);
Texture2D<uint> gMask : register(t3);
RWTexture2D<float> gOutViewZ : register(u8);            // IN_VIEWZ
RWTexture2D<float4> gOutNormalRoughness : register(u9); // IN_NORMAL_ROUGHNESS
RWTexture2D<float4> gOutDiff : register(u10);           // IN_DIFF_RADIANCE_HITDIST
RWTexture2D<float2> gOutMv : register(u11);             // IN_MV: the screen-space motion (last UV - this UV)
RWTexture2D<float> gOutConfidence : register(u12);      // IN_DIFF_CONFIDENCE of the next frame

[numthreads(8, 8, 1)]
void nrd_pack(uint2 p : SV_DispatchThreadID)
{
    uint w, h;
    gOutViewZ.GetDimensions(w, h);
    if (p.x >= w || p.y >= h)
        return;
    float2 ss = gSize.xy;
    int2 q = min(int2(p) * 2, int2(ss) - 1); // the full-size pixel of gi_main
    bool ch = gFlags.y != 0 && (gMask.Load(int3(q, 0)) & 0x80u) != 0;
    gOutConfidence[p] = ch ? CHAR_CONFIDENCE : 1.0;

    float4 nz = gNz.Load(int3(p, 0));
    float z = nz.w;
    gOutViewZ[p] = z;
    if (z >= SKY * 0.5)
    {
        gOutNormalRoughness[p] = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0, 0.0, 1.0), 1.0, 0.0);
        gOutDiff[p] = float4(0.0, 0.0, 0.0, 0.0);
        gOutMv[p] = float2(0.0, 0.0);
        return;
    }
    // a pixel with no normal faces the camera (view space -z)
    float3 nv = dot(nz.xyz, nz.xyz) < 0.5 ? float3(0.0, 0.0, -1.0) : nz.xyz;
    float3 nw = normalize(nv.x * gViewToWorld[0].xyz + nv.y * gViewToWorld[1].xyz + nv.z * gViewToWorld[2].xyz);
    gOutNormalRoughness[p] = NRD_FrontEnd_PackNormalAndRoughness(nw, 1.0, 0.0);
    float4 gi = gGi.Load(int3(p, 0));
    float nhd = REBLUR_FrontEnd_GetNormHitDist(gi.a, z, gHitDist.xyz, 1.0);
    gOutDiff[p] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(gi.rgb, nhd, true);

    // the motion of the camera (as ta_reproj of d3d9_enhance.m); 0 for a surface that moves with the camera: the last
    // frame has the same view distance at this pixel (within 0.3%) and a normal cosine of 0.9 or more, and the
    // reprojected position of the last frame does not match (more than 4 times that error and more than 0.5%)
    float2 mv = float2(0.0, 0.0);
    if (gFlags.x != 0)
    {
        float2 sp = (float2(q) + 0.5) / ss;
        float d = gSize.z + gSize.w / z;
        float4 r = (sp.x * 2.0 - 1.0) * gReproj[0] + (1.0 - sp.y * 2.0) * gReproj[1] + d * gReproj[2] + gReproj[3];
        if (r.w > 1e-6)
        {
            float2 n = r.xy / r.w;
            float2 ps = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
            float pz = r.w * z;
            mv = ps - sp;
            float4 n0 = gNzPrev.Load(int3(p, 0));
            float es = abs(n0.w - z) / z;
            float er = 1.0;
            int2 hp = int2(floor(ps * float2(w, h)));
            if (all(hp >= 0) && hp.x < int(w) && hp.y < int(h))
                er = abs(gNzPrev.Load(int3(hp, 0)).w - pz) / pz;
            if (es < 0.003 && er > max(4.0 * es, 0.005) && dot(n0.xyz, nz.xyz) >= 0.9)
                mv = float2(0.0, 0.0);
        }
    }
    gOutMv[p] = mv;
}

// nrd_unpack: gDenoised the REBLUR output (OUT_DIFF_RADIANCE_HITDIST), gNz as for nrd_pack; gOut rgb the light (linear),
// a the view distance (SKY: no surface), as the composite of d3d9_enhance.m reads it
Texture2D<float4> gDenoised : register(t0);
RWTexture2D<float4> gOut : register(u8);

[numthreads(8, 8, 1)]
void nrd_unpack(uint2 p : SV_DispatchThreadID)
{
    uint w, h;
    gOut.GetDimensions(w, h);
    if (p.x >= w || p.y >= h)
        return;
    float z = gNz.Load(int3(p, 0)).w;
    if (z >= SKY * 0.5)
    {
        gOut[p] = float4(0.0, 0.0, 0.0, SKY);
        return;
    }
    float4 c = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gDenoised.Load(int3(p, 0)));
    gOut[p] = float4(max(c.rgb, 0.0), z);
}
