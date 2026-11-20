// Enhancement settings that the runtime applies on top of the game (anisotropic filtering, ambient occlusion,
// tone mapping, HDR output, contact shadows, bloom, reduced banding, sharp shadows; and settings that the game
// project applies: gamepad input, intro videos, subtitle size).
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
} RtEnh;

extern RtEnh *rt_enh;

// The light of the contact shadows comes from the game's own projected-shadow pass: the first draw of a frame into
// a render target of w x h pixels and D3DFORMAT fmt, with no depth buffer, whose vertex shader constants
// creg .. creg + 3 hold the light's view-projection matrix (one row per register, clip = position * M).
// Without this call (or in a frame without such a draw) the frame has no contact shadows.
void d3d9_enh_light_pass(uint32_t w, uint32_t h, uint32_t fmt, uint32_t creg);

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
#endif
