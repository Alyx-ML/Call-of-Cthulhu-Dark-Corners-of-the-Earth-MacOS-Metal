// Enhancement settings that the runtime applies on top of the game (anisotropic filtering, ambient occlusion,
// tone mapping, HDR output and its calibration, contact shadows, bloom, reduced banding, sharp shadows, temporal
// anti-aliasing, headphone audio, raw mouse input, the interface in 4:3; and
// settings that the game project applies: gamepad input, intro videos, subtitle size, field of view, sanity effects).
//
// The game project owns the settings: it allocates RtEnh in guest memory, so that controls of the game's own
// menus can write the fields directly, and sets rt_enh before the first frame. While rt_enh is NULL the runtime
// applies no enhancement.
#pragma once
#include <stdint.h>

typedef struct RtEnh {
    int32_t aniso;     // 0: the game's own filtering; 1..4: anisotropic filtering 2x, 4x, 8x, 16x on all textures
    int32_t ao;        // 0: off; 1..3: XeGTAO ambient occlusion, quality low, medium, high
    int32_t tonemap;   // 0: off; 1: GT7 tone mapping of the presented image
    int32_t hdr;       // 0: SDR output; 1: extended dynamic range (EDR) output
    int32_t contact;   // 0: off; 1: Bend screen-space contact shadows
    int32_t pad;       // 0: off; 1: SDL3 gamepad input
    int32_t skip_intro; // 0: the game's intro videos; 1: the game starts at the main menu (read by the game project)
    int32_t bloom;     // 0: off; 1..3: dual Kawase bloom of the scene, intensity low, medium, high
    int32_t banding;   // 0: off; 1: 32-bit color render targets created from now on keep 16 bits per channel, and the
                       //    presented image is dithered to 8 bits
    int32_t shadows;   // 0: off; 1: the shadow targets of d3d9_enh_shadow_target created from now on are larger and keep
                       //    8 bits per channel
    int32_t sub_scale; // game project: the size of the subtitles (0 = the game's size; the game project gives the steps)
    int32_t hdr_peak;  // HDR output: Max Brightness (HDR), the peak brightness, an index into RT_ENH_HDR_PEAK_NITS
    int32_t hdr_paper; // HDR output: Brightness (HDR), paper white (the white of the game's image and UI), an index into
                       //    RT_ENH_HDR_PAPER_NITS
    int32_t hdr_test;  // game project: 1 while the HDR calibration menu is on the screen; the presented image then has
                       //    the calibration test image (only with HDR output)
    int32_t fov;       // game project: the field of view of the player camera (0 = the game's; the game project gives
                       //    the steps)
    int32_t aa;        // 0: off; 1: MetalFX temporal anti-aliasing of the 3D scene
    int32_t sanity;    // game project: the intensity of the game's visual sanity effects (0 = the game's; the game
                       //    project gives the steps)
    int32_t hrtf;      // 0: 3D sounds with balance panning; 1: 3D sounds through the head-related transfer function (HRTF)
                       //    of macOS for headphones (fmodex.c)
    int32_t raw_mouse; // 0: the mouse movement of the pointer (with the pointer acceleration of macOS); 1: the
                       //    unaccelerated movement of the mouse (kCGEventUnacceleratedPointerMovementX and Y, cocoa32.m)
} RtEnh;

// The HDR calibration has the two settings of the Capcom RE Engine (Resident Evil Village, Resident Evil 4), with the
// same 21 steps from -10 to +10 and the same values in nits (cd/m2). Index i is the step i - 10; index 10 (step 0) is
// the default of both settings.
// The values of hdr_peak (Max Brightness (HDR)), 300 to 4000 nits.
#define RT_ENH_HDR_PEAK_NITS {300, 400, 500, 600, 700, 750, 800, 850, 900, 950, 1000, \
                              1100, 1200, 1400, 1600, 1800, 2000, 2500, 3000, 3500, 4000}
#define RT_ENH_HDR_PEAKS 21
// The values of hdr_paper (Brightness (HDR)), 50 to 640 nits. macOS always shows SDR content at its SDR white (EDR
// 1.0), so step 0 (300 nits) is paper white at the SDR white of macOS, and another step is paper white at that SDR
// white x its nits / 300 (for example +10: 2.13 x SDR white, -10: 0.17 x SDR white).
#define RT_ENH_HDR_PAPER_NITS {50, 80, 120, 160, 180, 200, 220, 240, 260, 280, 300, \
                               320, 340, 380, 420, 460, 500, 540, 580, 610, 640}
#define RT_ENH_HDR_PAPERS 21
#define RT_ENH_HDR_DEFAULT 10
// macOS gives the brightness of the screen only as EDR headroom, a factor of the current SDR white. The runtime takes
// the peak of the screen as 100 nits x the potential EDR headroom (16 on a Liquid Retina XDR display: 1600 nits, its
// HDR peak), so SDR white is approximately 100 nits x potential headroom / current headroom. The runtime converts
// the peak from nits to EDR units with this SDR white. macOS clips a peak above the peak of the screen.

extern RtEnh *rt_enh;

// The light of the contact shadows comes from the game's own projected-shadow pass: the first draw of a frame into
// a render target of w x h pixels and D3DFORMAT fmt, with no depth buffer, whose vertex shader constants
// creg .. creg + 3 hold the light's view-projection matrix (one row per register, clip = position * M).
// Without this call (or in a frame without such a draw) the frame has no contact shadows.
void d3d9_enh_light_pass(uint32_t w, uint32_t h, uint32_t fmt, uint32_t creg);
// A fixed light for the contact shadows instead: the direction toward the light (x, y, z) in world space, a light
// at infinity. With this call every frame with a scene camera has contact shadows. d3d9_enh_light_pass then does not
// give the light; its render targets still do not end the scene when the game draws into them during the scene.
void d3d9_enh_light_dir(float x, float y, float z);
// The light of the game's lit draws instead: the draws into the back buffer whose vertex shader has, in its constant
// table (CTAB), the float constants dir (a unit vector toward the light, in world space, at the world position pos)
// and grad (the derivatives of that vector along world x, y and z in the registers grad, grad + stride and
// grad + 2 x stride). For a point light at the distance r the derivative matrix is -(I - dir dir^T) / r, so
// r = -2 / its trace, and the light is at pos + r x dir; a trace of 0 or more gives a light at infinity in the
// direction dir. The light of a frame is the light of the draw nearest the camera. The light moves 15% of the way to
// a new light in each frame, and a frame with no such draw keeps the last light. Until the first such draw, no frame
// has contact shadows. Call it before the device is created, and not together with d3d9_enh_light_dir. With this call
// d3d9_enh_light_pass does not give the light; its render targets still do not end the scene.
void d3d9_enh_light_vs(const char *dir, const char *pos, const char *grad, uint32_t stride);

// The interface in 4:3: the 2D draws of the game's interface are the draws with a vertex shader whose debug
// information (DBUG) names the source file `source` (the file name without its path, for example "Font.vsh"), and the
// draws with pretransformed vertices. For a shader of `source`, the screen x of a vertex (0 = left, 1 = right) is
// position.x * c[creg].x + c[creg].z, with the position element (FLOAT2 to FLOAT4, SHORT2 or SHORT4) of stream 0; for
// pretransformed vertices it is x / the width of the back buffer. On a back buffer wider than 4:3, such a draw into
// the back buffer is drawn in the centered 4:3 area of the back buffer (its viewport and its scissor rectangle are
// scaled horizontally by 4:3 / the aspect ratio of the back buffer, about the center). A draw whose vertices cover the
// full width (from 0 to 1) keeps the full width (backgrounds, fades, the bands behind the subtitles). Without this
// call every draw keeps the full width. Call it before the game creates its vertex shaders.
void d3d9_enh_ui_vs(const char *source, uint32_t creg);

// The size in pixels of the back buffer of the device (its last Reset or creation); 0 x 0 before the device exists.
void d3d9_enh_back_buffer(uint32_t *w, uint32_t *h);

// The camera of ambient occlusion and contact shadows: without this call, the fixed-function view and projection
// transforms of the scene draws. With it, the vertex shader constants creg .. creg + 3 of the opaque draws into the
// back buffer: a world-view-projection matrix (one row per register, clip = position * M) whose world transform is
// rigid. The matrix of the most draws in a frame is the camera (the level geometry, whose world transform is the
// identity). A frame with no such matrix gets no ambient occlusion and no contact shadows.
void d3d9_enh_camera_vs(uint32_t creg);

// The shadow targets of the game: render target textures of w x h pixels and D3DFORMAT fmt (only 23, R5G6B5). With
// the shadows setting, such a texture is stored scale times larger in x and y with 8 bits per channel; Direct3D still
// sees w x h. Such a target must not be drawn with a depth buffer (the program stops with an error).
void d3d9_enh_shadow_target(uint32_t w, uint32_t h, uint32_t fmt, uint32_t scale);

#ifdef __OBJC__
#import <Metal/Metal.h>
// d3d9_enhance.m, called by d3d9_metal.m.
// The scene of one frame: the camera (Direct3D 9 row-vector matrices, clip = position * view * proj) and the light
// of the contact shadows (homogeneous world position, w = 1, or the world direction to the light, w = 0).
typedef struct EnhScene {
    float view[16], proj[16];
    int have_light;
    float light[4];
} EnhScene;
// Ambient occlusion (quality ao 1..3, 0 = off) and contact shadows (contact != 0) of the scene: compute passes on
// cb that read depth, then a render pass that multiplies color in place. Returns 1 when color was changed.
int d3d9_enh_scene(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> color, id<MTLTexture> depth,
                   const EnhScene *s, int ao, int contact);
// Dual Kawase bloom (level 1..3, 0 = off) of the scene in color: render passes on cb that add the bloom to color in
// place. Returns 1 when color was changed.
int d3d9_enh_bloom(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> color, int level);
// MetalFX temporal anti-aliasing of the scene in color (with its depth buffer): color is replaced by the output of
// MetalFX. reproj: the rows of inverse(this frame's view-projection) x the last frame's view-projection (row vectors),
// or NULL when the motion of the camera is not known (zero motion). jx, jy: the sub-pixel offset of this frame's scene
// draws in pixels (y down). reset: the history of MetalFX starts again. Returns 1 when color was changed.
int d3d9_enh_taa(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> color, id<MTLTexture> depth, const float *reproj,
                 float jx, float jy, int reset);
#endif
