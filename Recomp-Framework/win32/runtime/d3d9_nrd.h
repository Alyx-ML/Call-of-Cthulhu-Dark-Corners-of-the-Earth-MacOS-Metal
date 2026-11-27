// The NVIDIA NRD REBLUR_DIFFUSE denoiser of the screen-space global illumination (d3d9_nrd.mm, nrd.mk). d3d9_enhance.m
// calls it after gi_main.
#ifndef D3D9_NRD_H
#define D3D9_NRD_H
#import <Metal/Metal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The camera of one frame. The matrices are Direct3D row-vector matrices (rows in memory), as EnhScene: view (world to
// view) and proj (view to clip) of this frame, pview and pproj of the last frame (equal to view and proj when the last
// frame is not known). reproj and da, db are as in d3d9_enhance.m (TA.r, the depth of a view distance z is da + db / z).
typedef struct NrdCamera {
    float view[16], proj[16], pview[16], pproj[16];
    float reproj[16];
    float da, db;
    uint32_t sw, sh;        // the size of the back buffer (gi_main reads the full-size pixel 2 x p)
    int have_reproj;        // reproj is valid
    int restart;            // the history starts again (no last frame, a new texture size)
    int have_mask;          // mask is the character stencil mask (bit 0x80)
    float unit_scale;       // world units per meter
} NrdCamera;

// Encodes on ce: the NRD inputs (nrd_glue.hlsl nrd_pack), the REBLUR_DIFFUSE dispatches and the output (nrd_unpack).
// gi: rgb the light of this frame, a the mean hit distance of its rays (gi_main); nz, nz_prev: xyz the view normal,
// w the view distance of this frame and of the last frame (all at the global illumination size); mask: the stencil
// view of the back buffer depth (or a 1 x 1 texture when have_mask is 0); out: rgb the denoised light, a the view
// distance, at the size of gi. The first call compiles the NRD shaders; an error stops the program (rt_fatal).
void d3d9_nrd_gi(id<MTLDevice> dev, id<MTLComputeCommandEncoder> ce, const NrdCamera *cam, id<MTLTexture> gi,
                 id<MTLTexture> nz, id<MTLTexture> nz_prev, id<MTLTexture> mask, id<MTLTexture> out);

#ifdef __cplusplus
}
#endif
#endif
