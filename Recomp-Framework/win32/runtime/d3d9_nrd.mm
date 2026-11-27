// The NVIDIA NRD REBLUR_DIFFUSE denoiser of the screen-space global illumination (d3d9_nrd.h). NRD (NVIDIA-RTX/NRD,
// third_party/nrd, not changed) is a static library (nrd.mk). tools/nrd_msl.py compiles its compute shaders (Slang,
// SPIR-V) and changes them to Metal Shading Language (nrd_msl.h). This file gives the shaders and
// the resources of each NRD dispatch to Metal:
//
// - The NRD instance has one denoiser (REBLUR_DIFFUSE, identifier 0). Its pipelines must have the shader identifiers
//   of nrd_msl.h (nrd_pipe, in the order of the instance; tools/nrd_dump.cpp made them from the same library); else the
//   program stops. Pipelines with the same identifier use one Metal pipeline.
// - The SPIR-V binding offsets of the NRD build (s 0, b 2, u 3, t 20) are the Metal argument indices: the samplers of
//   InstanceDesc at sampler 0 and up, the constants at buffer 2, the storage textures of a dispatch at texture 3 and
//   up, and its textures at texture 20 and up, in the order of DispatchDesc::resources.
// - The textures of the permanent and the transient pools have the NRD formats at the size of the global
//   illumination divided by their downsample factor (rounded up). They are made again when the size changes; the
//   history then starts again (CLEAR_AND_RESTART).
// - The compute encoder is serial and the textures are tracked by Metal, so each dispatch sees the writes of the
//   dispatches before it.
//
// The shaders are compiled at the first call (MTLCompileOptions with fast math off, because NRD tests for NaN and
// infinity), in parallel. Settings: the REBLUR defaults, with these changes: the maximum accumulated frames come from
// REBLUR_DEFAULT_ACCUMULATION_TIME (0.5 s) and the low-pass filtered frame rate (the formula of NRDSettings.h); the
// hit distance parameter A is 3 m in world units; the denoising range is half the sky distance of d3d9_enhance.m.
#import <Metal/Metal.h>
#include <NRD.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <string.h>
#include <time.h>
// rt32.h (rt_log, rt_fatal): not included, because its CPU register macros (cpu32.h) have the names of NRD fields
extern "C" void rt_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" void rt_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#include "d3d9_nrd.h"
#include "nrd_msl.h"

#define NRD_SKY 1.0e6f              // the view distance of a pixel with no surface (d3d9_enhance.m SKY)
#define NRD_TEX_MAX 128u            // the textures of a compute function (Apple GPU family 7 and later)

namespace {

// the constant buffer of nrd_glue.hlsl nrd_pack (cbuffer NrdPack)
struct PackC {
    float reproj[4][4];
    float view_to_world[3][4];
    float hit_dist[4];
    float size[4];
    uint32_t flags[4];
};
static_assert(sizeof(PackC) == 160, "NrdPack layout");

struct State {
    id<MTLDevice> dev;
    nrd::Instance* inst;
    const nrd::InstanceDesc* desc;
    id<MTLComputePipelineState> pso[NRD_MSL_COUNT];    // the pipeline of each nrd_msl shader
    MTLSize tg[NRD_MSL_COUNT];
    id<MTLComputePipelineState> pack, unpack;
    MTLSize pack_tg, unpack_tg;
    id<MTLSamplerState> samplers[(size_t)nrd::Sampler::MAX_NUM];
    id<MTLTexture> pool[64];        // the permanent pool, then the transient pool
    uint32_t pool_n;
    id<MTLTexture> viewz, normal, diff, mv, conf[2], out_diff;
    uint32_t w, h;                  // the size of the textures (0: none)
    uint32_t frame;                 // CommonSettings::frameIndex
    uint32_t conf_cur;              // the confidence texture that nrd_pack writes in this frame
    double last_t, smooth_ms;       // the time of the last frame and the low-pass filtered frame time
    int logged;
};
static State S;

static MTLPixelFormat mtl_format(nrd::Format f) {
    switch (f) {
        case nrd::Format::R8_UNORM: return MTLPixelFormatR8Unorm;
        case nrd::Format::R8_SNORM: return MTLPixelFormatR8Snorm;
        case nrd::Format::R8_UINT: return MTLPixelFormatR8Uint;
        case nrd::Format::R8_SINT: return MTLPixelFormatR8Sint;
        case nrd::Format::RG8_UNORM: return MTLPixelFormatRG8Unorm;
        case nrd::Format::RG8_SNORM: return MTLPixelFormatRG8Snorm;
        case nrd::Format::RG8_UINT: return MTLPixelFormatRG8Uint;
        case nrd::Format::RG8_SINT: return MTLPixelFormatRG8Sint;
        case nrd::Format::RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
        case nrd::Format::RGBA8_SNORM: return MTLPixelFormatRGBA8Snorm;
        case nrd::Format::RGBA8_UINT: return MTLPixelFormatRGBA8Uint;
        case nrd::Format::RGBA8_SINT: return MTLPixelFormatRGBA8Sint;
        case nrd::Format::RGBA8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
        case nrd::Format::R16_UNORM: return MTLPixelFormatR16Unorm;
        case nrd::Format::R16_SNORM: return MTLPixelFormatR16Snorm;
        case nrd::Format::R16_UINT: return MTLPixelFormatR16Uint;
        case nrd::Format::R16_SINT: return MTLPixelFormatR16Sint;
        case nrd::Format::R16_SFLOAT: return MTLPixelFormatR16Float;
        case nrd::Format::RG16_UNORM: return MTLPixelFormatRG16Unorm;
        case nrd::Format::RG16_SNORM: return MTLPixelFormatRG16Snorm;
        case nrd::Format::RG16_UINT: return MTLPixelFormatRG16Uint;
        case nrd::Format::RG16_SINT: return MTLPixelFormatRG16Sint;
        case nrd::Format::RG16_SFLOAT: return MTLPixelFormatRG16Float;
        case nrd::Format::RGBA16_UNORM: return MTLPixelFormatRGBA16Unorm;
        case nrd::Format::RGBA16_SNORM: return MTLPixelFormatRGBA16Snorm;
        case nrd::Format::RGBA16_UINT: return MTLPixelFormatRGBA16Uint;
        case nrd::Format::RGBA16_SINT: return MTLPixelFormatRGBA16Sint;
        case nrd::Format::RGBA16_SFLOAT: return MTLPixelFormatRGBA16Float;
        case nrd::Format::R32_UINT: return MTLPixelFormatR32Uint;
        case nrd::Format::R32_SINT: return MTLPixelFormatR32Sint;
        case nrd::Format::R32_SFLOAT: return MTLPixelFormatR32Float;
        case nrd::Format::RG32_UINT: return MTLPixelFormatRG32Uint;
        case nrd::Format::RG32_SINT: return MTLPixelFormatRG32Sint;
        case nrd::Format::RG32_SFLOAT: return MTLPixelFormatRG32Float;
        case nrd::Format::RGBA32_UINT: return MTLPixelFormatRGBA32Uint;
        case nrd::Format::RGBA32_SINT: return MTLPixelFormatRGBA32Sint;
        case nrd::Format::RGBA32_SFLOAT: return MTLPixelFormatRGBA32Float;
        case nrd::Format::R10_G10_B10_A2_UNORM: return MTLPixelFormatRGB10A2Unorm;
        case nrd::Format::R10_G10_B10_A2_UINT: return MTLPixelFormatRGB10A2Uint;
        case nrd::Format::R11_G11_B10_UFLOAT: return MTLPixelFormatRG11B10Float;
        case nrd::Format::R9_G9_B9_E5_UFLOAT: return MTLPixelFormatRGB9E5Float;
        default: return MTLPixelFormatInvalid;   // RGB32 formats: Metal has no texture format with 3 x 32 bits
    }
}

static id<MTLTexture> new_tex(MTLPixelFormat fmt, uint32_t w, uint32_t h, const char* what) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> t = [S.dev newTextureWithDescriptor:td];
    if (!t) rt_fatal("[d3d9] NRD: out of GPU memory for the texture %s (%ux%u)", what, w, h);
    return t;
}

static id<MTLComputePipelineState> compile(const NrdMsl& m, const char* fn, MTLSize* tg) {
    MTLCompileOptions* o = [MTLCompileOptions new];
    o.languageVersion = MTLLanguageVersion3_0;
    o.fastMathEnabled = NO;
    NSError* err = nil;
    id<MTLLibrary> lib = [S.dev newLibraryWithSource:@(m.msl) options:o error:&err];
    if (!lib) rt_fatal("[d3d9] NRD: the Metal shader of %s did not compile: %s", m.id, err.localizedDescription.UTF8String);
    id<MTLFunction> f = [lib newFunctionWithName:@(fn)];
    if (!f) rt_fatal("[d3d9] NRD: the Metal shader of %s has no kernel %s", m.id, fn);
    id<MTLComputePipelineState> p = [S.dev newComputePipelineStateWithFunction:f error:&err];
    if (!p) rt_fatal("[d3d9] NRD: the pipeline of %s was not created: %s", m.id, err.localizedDescription.UTF8String);
    NSUInteger n = (NSUInteger)m.tx * m.ty * m.tz;
    if (n == 0 || p.maxTotalThreadsPerThreadgroup < n)
        rt_fatal("[d3d9] NRD: the pipeline of %s cannot run %ux%ux%u threads in a group (at most %lu)", m.id, m.tx, m.ty, m.tz,
                 (unsigned long)p.maxTotalThreadsPerThreadgroup);
    *tg = MTLSizeMake(m.tx, m.ty, m.tz);
    return p;
}

static void setup(id<MTLDevice> dev) {
    if (S.inst) return;
    S.dev = dev;
    const nrd::LibraryDesc* lib = nrd::GetLibraryDesc();
    if (lib->versionMajor != NRD_VERSION_MAJOR || lib->versionMinor != NRD_VERSION_MINOR)
        rt_fatal("[d3d9] NRD: the library is version %u.%u, the headers %u.%u", lib->versionMajor, lib->versionMinor,
                 NRD_VERSION_MAJOR, NRD_VERSION_MINOR);
    const nrd::SPIRVBindingOffsets& b = lib->spirvBindingOffsets;
    if (b.samplerOffset != 0 || b.constantBufferOffset != 2 || b.storageTextureAndBufferOffset != 3 || b.textureOffset != 20)
        rt_fatal("[d3d9] NRD: the SPIR-V binding offsets of the library (s %u, b %u, u %u, t %u) are not the offsets of "
                 "nrd_msl.h (s 0, b 2, u 3, t 20)", b.samplerOffset, b.constantBufferOffset, b.storageTextureAndBufferOffset,
                 b.textureOffset);
    if (lib->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM || lib->roughnessEncoding != nrd::RoughnessEncoding::LINEAR)
        rt_fatal("[d3d9] NRD: the library was built with other normal or roughness encodings than nrd_glue.hlsl writes");
    const nrd::DenoiserDesc dd = {0, nrd::Denoiser::REBLUR_DIFFUSE};
    nrd::InstanceCreationDesc cd = {};
    cd.denoisers = &dd;
    cd.denoisersNum = 1;
    nrd::Result r = nrd::CreateInstance(cd, S.inst);
    if (r != nrd::Result::SUCCESS || !S.inst) rt_fatal("[d3d9] NRD: nrd::CreateInstance (REBLUR_DIFFUSE) failed (result %u)", (unsigned)r);
    S.desc = nrd::GetInstanceDesc(*S.inst);
    const nrd::InstanceDesc& d = *S.desc;
    if (d.pipelinesNum != NRD_PIPE_COUNT)
        rt_fatal("[d3d9] NRD: the instance has %u pipelines, nrd_msl.h has %u", d.pipelinesNum, (unsigned)NRD_PIPE_COUNT);
    for (uint32_t i = 0; i < d.pipelinesNum; i++)
        if (nrd_pipe[i] >= NRD_MSL_COUNT || strcmp(d.pipelines[i].shaderIdentifier, nrd_msl[nrd_pipe[i]].id) != 0)
            rt_fatal("[d3d9] NRD: pipeline %u is %s, nrd_msl.h does not have this shader there", i,
                     d.pipelines[i].shaderIdentifier);
    if (d.samplersNum > (uint32_t)nrd::Sampler::MAX_NUM || d.samplersBaseRegisterIndex != 0 || d.resourcesBaseRegisterIndex != 0 ||
        d.constantBufferRegisterIndex != 0)
        rt_fatal("[d3d9] NRD: the instance registers (samplers %u from %u, resources from %u, constants %u) are not the "
                 "registers of nrd_msl.h", d.samplersNum, d.samplersBaseRegisterIndex, d.resourcesBaseRegisterIndex,
                 d.constantBufferRegisterIndex);
    if (d.permanentPoolSize + d.transientPoolSize > sizeof S.pool / sizeof S.pool[0])
        rt_fatal("[d3d9] NRD: the instance has %u pool textures (at most %zu)", d.permanentPoolSize + d.transientPoolSize,
                 sizeof S.pool / sizeof S.pool[0]);
    for (uint32_t i = 0; i < d.samplersNum; i++) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.minFilter = sd.magFilter = d.samplers[i] == nrd::Sampler::LINEAR_CLAMP ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
        S.samplers[i] = [dev newSamplerStateWithDescriptor:sd];
        if (!S.samplers[i]) rt_fatal("[d3d9] NRD: Metal did not create sampler %u", i);
    }
    double t0 = CFAbsoluteTimeGetCurrent();
    dispatch_apply(NRD_MSL_COUNT + 2, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
        @autoreleasepool {
            if (i < NRD_MSL_COUNT) S.pso[i] = compile(nrd_msl[i], "main0", &S.tg[i]);
            else if (i == NRD_MSL_COUNT) S.pack = compile(nrd_pack_msl, "nrd_pack", &S.pack_tg);
            else S.unpack = compile(nrd_unpack_msl, "nrd_unpack", &S.unpack_tg);
        }
    });
    rt_log("[d3d9] NRD %u.%u.%u REBLUR_DIFFUSE: %u pipelines (%u shaders) and 2 glue kernels compiled in %.0f ms, %u permanent "
           "and %u transient textures", lib->versionMajor, lib->versionMinor, lib->versionBuild, d.pipelinesNum,
           (unsigned)NRD_MSL_COUNT, (CFAbsoluteTimeGetCurrent() - t0) * 1000.0, d.permanentPoolSize, d.transientPoolSize);
}

// the textures at the size w x h; returns 1 when they were made again
static int textures(uint32_t w, uint32_t h) {
    if (S.w == w && S.h == h) return 0;
    const nrd::InstanceDesc& d = *S.desc;
    S.pool_n = 0;
    for (uint32_t k = 0; k < 2; k++) {
        const nrd::TextureDesc* pool = k == 0 ? d.permanentPool : d.transientPool;
        uint32_t n = k == 0 ? d.permanentPoolSize : d.transientPoolSize;
        for (uint32_t i = 0; i < n; i++) {
            MTLPixelFormat f = mtl_format(pool[i].format);
            if (f == MTLPixelFormatInvalid) rt_fatal("[d3d9] NRD: pool texture %u has the format %u, which Metal does not have", i, (unsigned)pool[i].format);
            uint32_t ds = pool[i].downsampleFactor ? pool[i].downsampleFactor : 1;
            S.pool[S.pool_n++] = new_tex(f, (w + ds - 1) / ds, (h + ds - 1) / ds, k == 0 ? "permanent pool" : "transient pool");
        }
    }
    S.viewz = new_tex(MTLPixelFormatR32Float, w, h, "IN_VIEWZ");
    S.normal = new_tex(MTLPixelFormatRGB10A2Unorm, w, h, "IN_NORMAL_ROUGHNESS");
    S.diff = new_tex(MTLPixelFormatRGBA16Float, w, h, "IN_DIFF_RADIANCE_HITDIST");
    S.mv = new_tex(MTLPixelFormatRG16Float, w, h, "IN_MV");
    S.conf[0] = new_tex(MTLPixelFormatR8Unorm, w, h, "IN_DIFF_CONFIDENCE");
    S.conf[1] = new_tex(MTLPixelFormatR8Unorm, w, h, "IN_DIFF_CONFIDENCE");
    S.out_diff = new_tex(MTLPixelFormatRGBA16Float, w, h, "OUT_DIFF_RADIANCE_HITDIST");
    S.w = w;
    S.h = h;
    return 1;
}

static id<MTLTexture> resource(const nrd::ResourceDesc& r, id<MTLTexture> conf_prev) {
    const nrd::InstanceDesc& d = *S.desc;
    switch (r.type) {
        case nrd::ResourceType::IN_MV: return S.mv;
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return S.normal;
        case nrd::ResourceType::IN_VIEWZ: return S.viewz;
        case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: return S.diff;
        case nrd::ResourceType::IN_DIFF_CONFIDENCE: return conf_prev;
        case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: return S.out_diff;
        case nrd::ResourceType::PERMANENT_POOL:
            if (r.indexInPool < d.permanentPoolSize) return S.pool[r.indexInPool];
            break;
        case nrd::ResourceType::TRANSIENT_POOL:
            if (r.indexInPool < d.transientPoolSize) return S.pool[d.permanentPoolSize + r.indexInPool];
            break;
        default: break;
    }
    const char* name = nrd::GetResourceTypeString(r.type);
    rt_fatal("[d3d9] NRD: a dispatch uses the resource %s (pool index %u), which this integration does not have",
             name ? name : "(unknown)", r.indexInPool);
}

static void copy_rows(float* dst, const float* src) { memcpy(dst, src, 16 * sizeof(float)); }

}  // namespace

extern "C" void d3d9_nrd_gi(id<MTLDevice> dev, id<MTLComputeCommandEncoder> ce, const NrdCamera* cam, id<MTLTexture> gi,
                            id<MTLTexture> nz, id<MTLTexture> nz_prev, id<MTLTexture> mask, id<MTLTexture> out) {
    setup(dev);
    uint32_t w = (uint32_t)gi.width, h = (uint32_t)gi.height;
    if (w > 65535 || h > 65535) rt_fatal("[d3d9] NRD: the global illumination size %ux%u is larger than NRD supports", w, h);
    int fresh = textures(w, h);
    const nrd::InstanceDesc& d = *S.desc;

    // the frame rate, low-pass filtered with the 0.2 s time constant of NRDSettings.h
    double now = CFAbsoluteTimeGetCurrent();
    double ms = S.last_t > 0.0 ? (now - S.last_t) * 1000.0 : 1000.0 / 60.0;
    if (ms <= 0.0 || ms > 1000.0) ms = S.smooth_ms > 0.0 ? S.smooth_ms : 1000.0 / 60.0;   // a pause: keep the filtered value
    if (S.smooth_ms <= 0.0) S.smooth_ms = ms;
    double fps_prev = 1000.0 / S.smooth_ms;
    S.smooth_ms += (ms - S.smooth_ms) / (1.0 + fps_prev * 0.2);
    S.last_t = now;
    float fps = (float)(1000.0 / S.smooth_ms);

    nrd::CommonSettings cs = {};
    copy_rows(cs.viewToClipMatrix, cam->proj);
    copy_rows(cs.viewToClipMatrixPrev, cam->pproj);
    copy_rows(cs.worldToViewMatrix, cam->view);
    copy_rows(cs.worldToViewMatrixPrev, cam->pview);
    cs.motionVectorScale[0] = 1.0f;
    cs.motionVectorScale[1] = 1.0f;
    cs.motionVectorScale[2] = 0.0f;
    cs.resourceSize[0] = cs.resourceSizePrev[0] = cs.rectSize[0] = cs.rectSizePrev[0] = (uint16_t)w;
    cs.resourceSize[1] = cs.resourceSizePrev[1] = cs.rectSize[1] = cs.rectSizePrev[1] = (uint16_t)h;
    cs.denoisingRange = NRD_SKY * 0.5f;
    cs.frameIndex = S.frame++;
    int restart = fresh || cam->restart || !cam->have_reproj;
    cs.accumulationMode = fresh ? nrd::AccumulationMode::CLEAR_AND_RESTART
                                : restart ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
    cs.isHistoryConfidenceAvailable = true;
    nrd::Result r = nrd::SetCommonSettings(*S.inst, cs);
    if (r != nrd::Result::SUCCESS) rt_fatal("[d3d9] NRD: nrd::SetCommonSettings failed (result %u)", (unsigned)r);

    nrd::ReblurSettings rs = {};
    rs.hitDistanceParameters.A = 3.0f * cam->unit_scale;
    uint32_t maxn = nrd::GetMaxAccumulatedFrameNum(nrd::REBLUR_DEFAULT_ACCUMULATION_TIME, fps);
    if (maxn < 1) maxn = 1;
    if (maxn > nrd::REBLUR_MAX_HISTORY_FRAME_NUM) maxn = nrd::REBLUR_MAX_HISTORY_FRAME_NUM;
    rs.maxAccumulatedFrameNum = maxn;
    if (rs.maxFastAccumulatedFrameNum > maxn) rs.maxFastAccumulatedFrameNum = maxn;
    if (rs.historyFixFrameNum >= rs.maxFastAccumulatedFrameNum) rs.historyFixFrameNum = rs.maxFastAccumulatedFrameNum - 1;
    r = nrd::SetDenoiserSettings(*S.inst, 0, &rs);
    if (r != nrd::Result::SUCCESS) rt_fatal("[d3d9] NRD: nrd::SetDenoiserSettings (REBLUR) failed (result %u)", (unsigned)r);

    // the NRD inputs; this frame's confidence goes to the next frame (NRD reads the confidence of the last frame)
    uint32_t cc = S.conf_cur, cp = cc ^ 1u;
    id<MTLTexture> conf_prev = restart ? S.conf[cc] : S.conf[cp];
    PackC pc = {};
    memcpy(pc.reproj, cam->reproj, sizeof pc.reproj);
    // the rows of the rotation from view space to world space: the columns of the 3 x 3 part of the view matrix
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++) pc.view_to_world[k][j] = cam->view[j * 4 + k];
    pc.hit_dist[0] = rs.hitDistanceParameters.A;
    pc.hit_dist[1] = rs.hitDistanceParameters.B;
    pc.hit_dist[2] = rs.hitDistanceParameters.C;
    pc.size[0] = (float)cam->sw;
    pc.size[1] = (float)cam->sh;
    pc.size[2] = cam->da;
    pc.size[3] = cam->db;
    pc.flags[0] = (uint32_t)(cam->have_reproj != 0 && !restart);
    pc.flags[1] = (uint32_t)(cam->have_mask != 0);
    [ce setComputePipelineState:S.pack];
    [ce setBytes:&pc length:sizeof pc atIndex:15];
    [ce setTexture:gi atIndex:0];
    [ce setTexture:nz atIndex:1];
    [ce setTexture:nz_prev atIndex:2];
    [ce setTexture:mask atIndex:3];
    [ce setTexture:S.viewz atIndex:8];
    [ce setTexture:S.normal atIndex:9];
    [ce setTexture:S.diff atIndex:10];
    [ce setTexture:S.mv atIndex:11];
    [ce setTexture:S.conf[cc] atIndex:12];
    [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:S.pack_tg];
    S.conf_cur = cp;

    // the REBLUR dispatches
    const nrd::DispatchDesc* dds = nullptr;
    uint32_t ddn = 0;
    const nrd::Identifier id0 = 0;
    r = nrd::GetComputeDispatches(*S.inst, &id0, 1, dds, ddn);
    if (r != nrd::Result::SUCCESS) rt_fatal("[d3d9] NRD: nrd::GetComputeDispatches failed (result %u)", (unsigned)r);
    for (uint32_t i = 0; i < d.samplersNum; i++) [ce setSamplerState:S.samplers[i] atIndex:i];
    for (uint32_t i = 0; i < ddn; i++) {
        const nrd::DispatchDesc& dd = dds[i];
        if (dd.pipelineIndex >= NRD_PIPE_COUNT) rt_fatal("[d3d9] NRD: dispatch %s uses pipeline %u", dd.name ? dd.name : "?", dd.pipelineIndex);
        const uint16_t sh = nrd_pipe[dd.pipelineIndex];
        [ce setComputePipelineState:S.pso[sh]];
        if (dd.constantBufferDataSize) {
            if (dd.constantBufferDataSize > 4096) rt_fatal("[d3d9] NRD: dispatch %s has %u bytes of constants (at most 4096)",
                                                           dd.name ? dd.name : "?", dd.constantBufferDataSize);
            [ce setBytes:dd.constantBufferData length:dd.constantBufferDataSize atIndex:2];
        }
        uint32_t nt = 0, ns = 0;
        for (uint32_t j = 0; j < dd.resourcesNum; j++) {
            const nrd::ResourceDesc& rd = dd.resources[j];
            id<MTLTexture> t = resource(rd, conf_prev);
            NSUInteger at;
            if (rd.descriptorType == nrd::DescriptorType::TEXTURE) at = 20 + nt++;
            else at = 3 + ns++;
            if ((rd.descriptorType == nrd::DescriptorType::TEXTURE && at >= NRD_TEX_MAX) ||
                (rd.descriptorType != nrd::DescriptorType::TEXTURE && at >= 20))
                rt_fatal("[d3d9] NRD: dispatch %s has too many textures (%u) or storage textures (%u)", dd.name ? dd.name : "?", nt, ns);
            [ce setTexture:t atIndex:at];
        }
        [ce dispatchThreadgroups:MTLSizeMake(dd.gridWidth, dd.gridHeight, 1) threadsPerThreadgroup:S.tg[sh]];
    }

    // the output for the composite
    [ce setComputePipelineState:S.unpack];
    [ce setTexture:S.out_diff atIndex:0];
    [ce setTexture:nz atIndex:1];
    [ce setTexture:out atIndex:8];
    [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:S.unpack_tg];
    if (!S.logged) {
        rt_log("[d3d9] NRD REBLUR_DIFFUSE %ux%u: %u dispatches in the first frame, at most %u accumulated frames (%.0f fps)", w, h, ddn, maxn, fps);
        S.logged = 1;
    }
}
