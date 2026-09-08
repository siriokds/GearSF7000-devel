/* SDL3/SDL_GPU renderer.  The desktop UI and emulator images share the same
 * GPU command buffer; no OpenGL objects or compatibility path remain. */

#include <algorithm>
#include <cstring>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdlgpu3.h"
#include "emu.h"
#include "config.h"
#include <climits>

#include "scheduler.h"
#include "crt_signal_capture_loader.h"
#include "../../src/gearsf7000.h"
#include "shaders/crt_pass_shaders.h"
#include "shaders/composite_decode_shaders.h"
#include "shaders/crt_consumer_shaders.h"
#include "shaders/crt_lottes_shaders.h"
#include "shaders/advanced_scaling_shaders.h"

#define RENDERER_IMPORT
#include "renderer.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"


namespace
{
const config_VideoOutput& active_video_output()
{
    return config_video_output_for_mode(config_debug.debug);
}

SDL_GPUDevice* gpu_device = nullptr;
SDL_Window* gpu_window = nullptr;
SDL_GPUTexture* emu_texture = nullptr;
SDL_GPUTexture* debug_background_texture = nullptr;
SDL_GPUTexture* debug_tiles_texture = nullptr;
SDL_GPUTexture* debug_sprite_textures[64] = {};
SDL_GPUTexture* debug_rom_inspector_texture = nullptr;
SDL_GPUTransferBuffer* emu_transfer = nullptr;
SDL_GPUTransferBuffer* debug_background_transfer = nullptr;
SDL_GPUTransferBuffer* debug_tiles_transfer = nullptr;
SDL_GPUTransferBuffer* debug_sprite_transfers[64] = {};
SDL_GPUTransferBuffer* debug_rom_inspector_transfer = nullptr;
SDL_GPUSampler* nearest_sampler = nullptr;
GC_RuntimeInfo current_runtime = {};
int emu_texture_width = 0;
int emu_texture_height = 0;
std::vector<SDL_GPUTexture*> auxiliary_textures;

// Second, GPU-side video output: a render target fed by a custom pipeline
// sampling emu_texture, instead of CPU upload like every texture above.
// Proof-of-infrastructure only - the shader is a plain horizontal blur, not
// a real composite/S-Video decode. See platforms/desktop-shared/shaders/
// crt_pass/README.md. Off by default (bool below), so it costs nothing
// unless a debug view asks for it.
SDL_GPUTexture* crt_test_texture = nullptr;
SDL_GPUGraphicsPipeline* crt_blur_pipeline = nullptr;
SDL_GPUSampler* linear_sampler = nullptr;
bool crt_test_enabled = false;
bool crt_test_blur_enabled = true;
// In dots (see blur.frag) - kept independent of crt_test_blur_enabled so
// toggling the checkbox off and back on restores the chosen value. Must
// stay inside the GUI slider's 0.00-1.20 range (debug_window_crt_test_pass
// in gui_debug.cpp) - this used to default to 2.0, outside that range,
// which the slider displayed as an unclamped "2.00" pinned at its own max.
float crt_test_blur_radius = 0.30f;

// Real two-pass composite/S-Video decode, running on a captured GCRT file
// rather than live gameplay - see crt_signal_capture_loader.h and
// shaders/crt_pass/composite_chroma.frag / composite_luma.frag. Geometry
// is fixed at CrtSignalCaptureLoader::kPictureDots/kPictureLines: this is
// not tied to emu_texture's own (possibly overscan-cropped, possibly
// full-raster) size the way the blur test pass is.
SDL_GPUTexture* composite_index_texture = nullptr;
SDL_GPUTransferBuffer* composite_index_transfer = nullptr;
SDL_GPUTexture* composite_chroma_texture = nullptr;
SDL_GPUTexture* composite_output_texture = nullptr;
SDL_GPUGraphicsPipeline* composite_chroma_pipeline = nullptr;
SDL_GPUGraphicsPipeline* composite_luma_pipeline = nullptr;
bool composite_decode_ready = false;
bool composite_decode_enabled = false;
bool composite_decode_mode_is_composite = true;

// Display-stage test - samples emu_texture at its native geometry, but
// renders into a target kCrtConsumerDebugUpscale times larger. The
// scanline term (see crt_consumer.frag) only varies row-to-row when the
// output has more rows than the source - at a 1:1 target every fragment
// lands on the same texel-center phase (fract(n+0.5) == 0.5 for every
// integer n), so the scanline weight collapses to one constant for the
// whole image and no banding is possible. Measured, not assumed: this was
// caught by comparing rendered output against the shader math after the
// debug window showed no horizontal banding at all. A real final pass
// would get this "for free" from the window's own higher resolution; this
// debug texture has no such window to borrow from, so it manufactures the
// same effect on purpose. Runtime-adjustable (not a compile-time constant)
// specifically so it can be tuned live from the debug window while looking
// for a value where the banding is actually visible through ImGui's own
// display-time downsampling - the real final pass will never need this
// control at all, since there OutputSize simply follows whatever the
// window's actual resolution already is. See shaders/crt_pass/
// crt_consumer.frag.
constexpr int kCrtConsumerDebugUpscaleDefault = 4;
constexpr int kCrtConsumerDebugUpscaleMin = 1;
constexpr int kCrtConsumerDebugUpscaleMax = 16;
int crt_consumer_debug_upscale = kCrtConsumerDebugUpscaleDefault;
SDL_GPUTexture* crt_consumer_texture = nullptr;
SDL_GPUGraphicsPipeline* crt_consumer_pipeline = nullptr;
bool crt_consumer_enabled = false;
RendererCrtConsumerParams crt_consumer_params;

// Second display-stage candidate - trimmed crt-lottes-fast.glsl port, see
// shaders/crt_pass/crt_lottes.frag. Same debug-upscale reasoning as
// crt_consumer_debug_upscale above, kept as an independent variable so the
// two passes can be tuned/compared without one affecting the other.
int crt_lottes_debug_upscale = kCrtConsumerDebugUpscaleDefault;
SDL_GPUTexture* crt_lottes_texture = nullptr;
SDL_GPUGraphicsPipeline* crt_lottes_pipeline = nullptr;
bool crt_lottes_enabled = false;
RendererCrtLottesParams crt_lottes_params;

// Live Output path (selected independently by Video or Debug > Video, not a
// diagnostic test window) - separate texture from crt_lottes_texture above because the two
// have genuinely different sizing needs: the debug texture is native
// resolution times a manually tuned upscale factor with no real window to
// match, this one must be exactly the real on-screen pixel size the main
// game view currently occupies (see renderer_set_crt_lottes_output_size),
// so it needs no further resampling when ImGui draws it - avoiding a
// repeat of the hidden-downsample bug that hid the scanline banding twice
// already this session. Reads the active presentation profile's persisted
// settings rather than crt_lottes_params (the Debug
// window's own transient scratch copy) - see config.h's own comment on
// why those are deliberately two separate parameter sets.
SDL_GPUTexture* crt_lottes_live_texture = nullptr;
int crt_lottes_live_width = 0;
int crt_lottes_live_height = 0;

// Live Output Advanced Scaling - same live-texture-at-real-
// size pattern as crt_lottes_live_texture above, and for the same reason:
// the resampling weights come from where each output fragment actually
// lands relative to the source texel grid, so this only works rendered at
// the real on-screen resolution. See shaders/crt_pass/advanced_scaling.frag.
SDL_GPUTexture* advanced_scaling_live_texture = nullptr;
SDL_GPUGraphicsPipeline* advanced_scaling_pipeline = nullptr;
int advanced_scaling_live_width = 0;
int advanced_scaling_live_height = 0;

constexpr int kBytesPerPixel = 4;

SDL_GPUTexture* create_texture(int width, int height)
{
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(gpu_device, &info);
}

SDL_GPUTransferBuffer* create_transfer(int width, int height)
{
    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<Uint32>(width * height * kBytesPerPixel);
    return SDL_CreateGPUTransferBuffer(gpu_device, &info);
}

void release_texture(SDL_GPUTexture*& texture, SDL_GPUTransferBuffer*& transfer)
{
    if (transfer) SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);
    if (texture) SDL_ReleaseGPUTexture(gpu_device, texture);
    transfer = nullptr;
    texture = nullptr;
}

// Unlike create_texture (CPU-upload only), this can be bound as a render
// pass colour target as well as sampled afterwards - it never gets a
// transfer buffer, only a GPU pipeline ever writes into it.
SDL_GPUTexture* create_render_target(int width, int height)
{
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(gpu_device, &info);
}

SDL_GPUShader* create_crt_pass_shader(SDL_GPUShaderStage stage,
    const uint8_t* spirv, size_t spirvSize,
    const uint8_t* metallib, size_t metallibSize,
    const uint8_t* dxil, size_t dxilSize,
    int fragmentSamplers = 1, int fragmentUniformBuffers = 1)
{
    (void)metallib;
    (void)metallibSize;

    SDL_GPUShaderCreateInfo info = {};
    info.stage = stage;
    info.num_samplers = (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) ? fragmentSamplers : 0;
    info.num_uniform_buffers = (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) ? fragmentUniformBuffers : 0;

    const char* driver = SDL_GetGPUDeviceDriver(gpu_device);
    if (driver && strcmp(driver, "vulkan") == 0)
    {
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = spirv;
        info.code_size = spirvSize;
    }
    else if (driver && strcmp(driver, "direct3d12") == 0)
    {
        // SPIRV-Cross keeps the descriptor sets the GLSL declares, and those
        // already match the register spaces SDL_CreateGPUShader documents
        // (set 2 -> t/s space2, set 3 -> b space3), so the translation needs
        // no rebinding - see shaders/crt_pass/README.md.
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = dxil;
        info.code_size = dxilSize;
    }
#ifdef __APPLE__
    else
    {
        info.entrypoint = "main0";
        info.format = SDL_GPU_SHADERFORMAT_METALLIB;
        info.code = metallib;
        info.code_size = metallibSize;
    }
#else
    else
    {
        // Some other driver: no bytecode to offer, so the pass turns itself
        // off rather than shipping an unverified format.
        return nullptr;
    }
#endif
    return SDL_CreateGPUShader(gpu_device, &info);
}

// Every fullscreen pass in this file shares the same vertex shader (see
// shaders/crt_pass/blur.vert - a generic full-screen triangle, nothing
// pass-specific), so pipeline creation only really varies by fragment
// shader, sampler count and output format.
SDL_GPUGraphicsPipeline* create_fullscreen_pipeline(
    const uint8_t* fragSpirv, size_t fragSpirvSize,
    const uint8_t* fragMetallib, size_t fragMetallibSize,
    const uint8_t* fragDxil, size_t fragDxilSize,
    int fragmentSamplers, SDL_GPUTextureFormat targetFormat,
    int fragmentUniformBuffers = 1)
{
    SDL_GPUShader* vertexShader = create_crt_pass_shader(SDL_GPU_SHADERSTAGE_VERTEX,
        spirv_crt_blur_vertex, sizeof(spirv_crt_blur_vertex),
        metallib_crt_blur_vertex, sizeof(metallib_crt_blur_vertex),
        dxil_crt_blur_vertex, sizeof(dxil_crt_blur_vertex));
    SDL_GPUShader* fragmentShader = create_crt_pass_shader(SDL_GPU_SHADERSTAGE_FRAGMENT,
        fragSpirv, fragSpirvSize, fragMetallib, fragMetallibSize,
        fragDxil, fragDxilSize, fragmentSamplers,
        fragmentUniformBuffers);
    if (!vertexShader || !fragmentShader)
    {
        if (vertexShader) SDL_ReleaseGPUShader(gpu_device, vertexShader);
        if (fragmentShader) SDL_ReleaseGPUShader(gpu_device, fragmentShader);
        Log("Fullscreen pass shader creation failed: %s\n", SDL_GetError());
        return nullptr;
    }

    SDL_GPUColorTargetDescription colorTarget = {};
    colorTarget.format = targetFormat;

    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vertexShader;
    info.fragment_shader = fragmentShader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    // No vertex buffer: the vertex shader derives its full-screen triangle
    // from gl_VertexIndex alone.
    info.vertex_input_state.num_vertex_buffers = 0;
    info.vertex_input_state.num_vertex_attributes = 0;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &colorTarget;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &info);
    SDL_ReleaseGPUShader(gpu_device, vertexShader);
    SDL_ReleaseGPUShader(gpu_device, fragmentShader);
    if (!pipeline)
        Log("Fullscreen pass pipeline creation failed: %s\n", SDL_GetError());
    return pipeline;
}

bool ensure_crt_blur_pipeline()
{
    if (crt_blur_pipeline)
        return true;
    crt_blur_pipeline = create_fullscreen_pipeline(
        spirv_crt_blur_fragment, sizeof(spirv_crt_blur_fragment),
        metallib_crt_blur_fragment, sizeof(metallib_crt_blur_fragment),
        dxil_crt_blur_fragment, sizeof(dxil_crt_blur_fragment),
        1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    return crt_blur_pipeline != nullptr;
}

bool ensure_composite_decode_pipelines()
{
    if (composite_chroma_pipeline && composite_luma_pipeline)
        return true;
    if (!composite_chroma_pipeline)
        composite_chroma_pipeline = create_fullscreen_pipeline(
            spirv_composite_chroma_fragment, sizeof(spirv_composite_chroma_fragment),
            metallib_composite_chroma_fragment, sizeof(metallib_composite_chroma_fragment),
            dxil_composite_chroma_fragment, sizeof(dxil_composite_chroma_fragment),
            1, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT);
    if (!composite_luma_pipeline)
        composite_luma_pipeline = create_fullscreen_pipeline(
            spirv_composite_luma_fragment, sizeof(spirv_composite_luma_fragment),
            metallib_composite_luma_fragment, sizeof(metallib_composite_luma_fragment),
            dxil_composite_luma_fragment, sizeof(dxil_composite_luma_fragment),
            2, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    return composite_chroma_pipeline != nullptr && composite_luma_pipeline != nullptr;
}

bool ensure_crt_consumer_pipeline()
{
    if (crt_consumer_pipeline)
        return true;
    crt_consumer_pipeline = create_fullscreen_pipeline(
        spirv_crt_consumer_fragment, sizeof(spirv_crt_consumer_fragment),
        metallib_crt_consumer_fragment, sizeof(metallib_crt_consumer_fragment),
        dxil_crt_consumer_fragment, sizeof(dxil_crt_consumer_fragment),
        1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    return crt_consumer_pipeline != nullptr;
}

// Runs the display-stage test pass into crt_consumer_texture, reading
// emu_texture directly - this operates on whatever RGB the signal stage
// (today: the default static palette, tomorrow: Composite/S-Video/RGB)
// already produced, matching the real gameplay resolution and geometry.
void render_crt_consumer_pass(SDL_GPUCommandBuffer* command)
{
    if (!crt_consumer_texture || !ensure_crt_consumer_pipeline())
        return;

    SDL_GPUColorTargetInfo target = {};
    target.texture = crt_consumer_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, crt_consumer_pipeline);

    SDL_GPUTextureSamplerBinding sourceBinding = {emu_texture, linear_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sourceBinding, 1);

    const float w = static_cast<float>(emu_texture_width);
    const float h = static_cast<float>(emu_texture_height);
    // The render target is crt_consumer_debug_upscale times larger than the
    // source - see that variable's comment for why OutputSize must be the
    // real target resolution, not SourceSize, for the scanline term to do
    // anything at all.
    const float outW = w * static_cast<float>(crt_consumer_debug_upscale);
    const float outH = h * static_cast<float>(crt_consumer_debug_upscale);
    const RendererCrtConsumerParams& p = crt_consumer_params;
    // 96 bytes, laid out identically to the GLSL std140 and MSL struct -
    // see crt_consumer.frag's own comment on why exactly two pads.
    const float uniformData[24] = {
        w, h, 1.0f / w, 1.0f / h,             // SourceSize
        outW, outH, 0.0f, 0.0f,               // OutputSize
        0.0f, 0.0f, 0.0f, 1.0f,               // BackgroundColor
        p.blurx, p.blury, p.scanlow, p.scanhigh,
        p.beamlow, p.beamhigh, p.preserve, p.brightboost1,
        p.brightboost2, p.gammaOut, 0.0f, 0.0f,
    };
    SDL_PushGPUFragmentUniformData(command, 0, uniformData, sizeof(uniformData));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

bool ensure_crt_lottes_pipeline()
{
    if (crt_lottes_pipeline)
        return true;
    crt_lottes_pipeline = create_fullscreen_pipeline(
        spirv_crt_lottes_fragment, sizeof(spirv_crt_lottes_fragment),
        metallib_crt_lottes_fragment, sizeof(metallib_crt_lottes_fragment),
        dxil_crt_lottes_fragment, sizeof(dxil_crt_lottes_fragment),
        1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    return crt_lottes_pipeline != nullptr;
}

// Runs the second display-stage test pass into crt_lottes_texture, reading
// emu_texture directly - same role as render_crt_consumer_pass, a second
// candidate for the same stage rather than a replacement.
void render_crt_lottes_pass(SDL_GPUCommandBuffer* command)
{
    if (!crt_lottes_texture || !ensure_crt_lottes_pipeline())
        return;

    SDL_GPUColorTargetInfo target = {};
    target.texture = crt_lottes_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, crt_lottes_pipeline);

    // Nearest, not linear: the shader computes exact texel-center UVs by
    // hand (see crt_lottes.frag's Fetch calls) - the original shader forces
    // LOD 0 for the same reason (COMPAT_TEXTURE(..., -16.0)), we have no
    // mips to force away from, but nearest keeps every fetch an exact texel
    // read regardless, matching the reference's intent precisely.
    SDL_GPUTextureSamplerBinding sourceBinding = {emu_texture, nearest_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sourceBinding, 1);

    const float w = static_cast<float>(emu_texture_width);
    const float h = static_cast<float>(emu_texture_height);

    // Same reasoning as render_crt_consumer_pass's outW/outH - the vertical
    // scanline term needs OutputSize.y > SourceSize.y to have anything to
    // compute at all.
    const float outW = w * static_cast<float>(crt_lottes_debug_upscale);
    const float outH = h * static_cast<float>(crt_lottes_debug_upscale);
    const RendererCrtLottesParams& p = crt_lottes_params;
    // 64 bytes, laid out identically to the GLSL std140 and MSL struct -
    // four vec4 rows, no separate padding needed.
    const float uniformData[16] = {
        w, h, 1.0f / w, 1.0f / h,                          // SourceSize
        outW, outH, 1.0f / outW, 1.0f / outH,              // OutputSize
        p.maskType, p.maskIntensity, p.scanlineThinness, p.scanBlur, // Mask
        p.crtGamma, p.blackThreshold, p.boldness, p.edgeRadius, // GammaPad
    };
    SDL_PushGPUFragmentUniformData(command, 0, uniformData, sizeof(uniformData));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

// Recreates crt_lottes_live_texture only when the requested size actually
// changes (window resize, Scale menu change) - called from
// renderer_set_crt_lottes_output_size, not every frame unconditionally.
void recreate_crt_lottes_live_texture(int width, int height)
{
    SDL_WaitForGPUIdle(gpu_device);
    if (crt_lottes_live_texture) SDL_ReleaseGPUTexture(gpu_device, crt_lottes_live_texture);
    crt_lottes_live_texture = (width > 0 && height > 0) ? create_render_target(width, height) : nullptr;
    crt_lottes_live_width = width;
    crt_lottes_live_height = height;
    renderer_crt_lottes_live_texture = crt_lottes_live_texture
        ? static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(crt_lottes_live_texture))
        : ImTextureID_Invalid;
}

// Runs the live Output CRT (Lottes) pass into crt_lottes_live_texture, at
// exactly the real on-screen size (see crt_lottes_live_texture's own
// comment) - only called when the active presentation profile selects
// POSTPROCESSING_CRT_LOTTES. Uses its persisted settings, not the diagnostic
// Debug test window's crt_lottes_params.
void render_crt_lottes_live_pass(SDL_GPUCommandBuffer* command)
{
    if (!crt_lottes_live_texture || !ensure_crt_lottes_pipeline())
        return;

    SDL_GPUColorTargetInfo target = {};
    target.texture = crt_lottes_live_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, crt_lottes_pipeline);

    SDL_GPUTextureSamplerBinding sourceBinding = {emu_texture, nearest_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sourceBinding, 1);

    const float w = static_cast<float>(emu_texture_width);
    const float h = static_cast<float>(emu_texture_height);
    // The real on-screen resolution, not a manufactured upscale factor -
    // this is the "for free" case output_pipeline_architecture.md always
    // described: a real window genuinely is higher resolution than the
    // source, so the scanline term has real row-to-row variation without
    // any debug trick.
    const float outW = static_cast<float>(crt_lottes_live_width);
    const float outH = static_cast<float>(crt_lottes_live_height);
    const float uniformData[16] = {
        w, h, 1.0f / w, 1.0f / h,
        outW, outH, 1.0f / outW, 1.0f / outH,
        active_video_output().crt_lottes_mask_type, active_video_output().crt_lottes_mask_intensity,
        active_video_output().crt_lottes_scanline_thinness, active_video_output().crt_lottes_scan_blur,
        active_video_output().crt_lottes_gamma, active_video_output().crt_lottes_black_threshold,
        active_video_output().crt_lottes_boldness, active_video_output().crt_lottes_edge_radius,
    };
    SDL_PushGPUFragmentUniformData(command, 0, uniformData, sizeof(uniformData));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

bool ensure_advanced_scaling_pipeline()
{
    if (advanced_scaling_pipeline)
        return true;
    // One uniform buffer (SourceSize + Params: sharpness/blackLevel/
    // contrast) - see advanced_scaling.frag's own comment.
    advanced_scaling_pipeline = create_fullscreen_pipeline(
        spirv_advanced_scaling_fragment, sizeof(spirv_advanced_scaling_fragment),
        metallib_advanced_scaling_fragment, sizeof(metallib_advanced_scaling_fragment),
        dxil_advanced_scaling_fragment, sizeof(dxil_advanced_scaling_fragment),
        1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    return advanced_scaling_pipeline != nullptr;
}

// Rebuilds advanced_scaling_live_texture at the given size, only when it actually
// changes - mirrors recreate_crt_lottes_live_texture.
void recreate_advanced_scaling_live_texture(int width, int height)
{
    SDL_WaitForGPUIdle(gpu_device);
    if (advanced_scaling_live_texture) SDL_ReleaseGPUTexture(gpu_device, advanced_scaling_live_texture);
    advanced_scaling_live_texture = (width > 0 && height > 0) ? create_render_target(width, height) : nullptr;
    advanced_scaling_live_width = width;
    advanced_scaling_live_height = height;
    renderer_advanced_scaling_live_texture = advanced_scaling_live_texture
        ? static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(advanced_scaling_live_texture))
        : ImTextureID_Invalid;
}

// Runs the Advanced Scaling pass into advanced_scaling_live_texture, at
// the real on-screen size - only called when the active presentation profile
// selects POSTPROCESSING_ADVANCED_SCALING.
void render_advanced_scaling_live_pass(SDL_GPUCommandBuffer* command)
{
    if (!advanced_scaling_live_texture || !ensure_advanced_scaling_pipeline())
        return;

    SDL_GPUColorTargetInfo target = {};
    target.texture = advanced_scaling_live_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, advanced_scaling_pipeline);

    // Nearest, not linear - the resampling kernel samples at exact
    // texel-center UVs by hand (see advanced_scaling.frag's own comment),
    // so a linear sampler would just silently blend in wrong directions on
    // top of that. The bold-dark-edges search samples arbitrary nearby UVs
    // instead, where nearest is still the right choice (cheap, and exact
    // enough for a same-or-adjacent-texel darkness check).
    SDL_GPUTextureSamplerBinding sourceBinding = {emu_texture, nearest_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sourceBinding, 1);

    const float w = static_cast<float>(emu_texture_width);
    const float h = static_cast<float>(emu_texture_height);
    const float outW = static_cast<float>(advanced_scaling_live_width);
    const float outH = static_cast<float>(advanced_scaling_live_height);
    const float uniformData[12] = {
        w, h, 1.0f / w, 1.0f / h,
        outW, outH, 1.0f / outW, 1.0f / outH,
        active_video_output().advanced_scaling_sharpness,
        active_video_output().advanced_scaling_black_threshold,
        active_video_output().advanced_scaling_boldness,
        active_video_output().advanced_scaling_edge_radius,
    };
    SDL_PushGPUFragmentUniformData(command, 0, uniformData, sizeof(uniformData));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

// Runs the blur test pass into crt_test_texture, reading emu_texture. Only
// called when crt_test_enabled, right after emu_texture has been uploaded
// for this frame and before the swapchain pass begins.
void render_crt_test_pass(SDL_GPUCommandBuffer* command)
{
    if (!crt_test_texture || !ensure_crt_blur_pipeline())
        return;

    SDL_GPUColorTargetInfo target = {};
    target.texture = crt_test_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, crt_blur_pipeline);

    SDL_GPUTextureSamplerBinding sourceBinding = {};
    sourceBinding.texture = emu_texture;
    sourceBinding.sampler = linear_sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &sourceBinding, 1);

    // 16 bytes, explicitly padded to match the GLSL std140 and MSL struct
    // layouts field for field - avoids relying on either side's automatic
    // padding rules agreeing with the other.
    const float uniformData[4] = {
        1.0f / emu_texture_width, 1.0f / emu_texture_height,
        crt_test_blur_enabled ? crt_test_blur_radius : 0.0f, 0.0f};
    SDL_PushGPUFragmentUniformData(command, 0, uniformData, sizeof(uniformData));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

bool ensure_composite_decode_textures()
{
    if (composite_index_texture && composite_chroma_texture && composite_output_texture)
        return true;

    const int w = CrtSignalCaptureLoader::kPictureDots;
    const int h = CrtSignalCaptureLoader::kPictureLines;

    SDL_GPUTextureCreateInfo indexInfo = {};
    indexInfo.type = SDL_GPU_TEXTURETYPE_2D;
    indexInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    indexInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    indexInfo.width = w;
    indexInfo.height = h;
    indexInfo.layer_count_or_depth = 1;
    indexInfo.num_levels = 1;
    indexInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    composite_index_texture = SDL_CreateGPUTexture(gpu_device, &indexInfo);

    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(w * h);  // 1 byte per texel, R8
    composite_index_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &transferInfo);

    composite_chroma_texture = [&]() {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = w; info.height = h;
        info.layer_count_or_depth = 1; info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        return SDL_CreateGPUTexture(gpu_device, &info);
    }();

    composite_output_texture = create_render_target(w, h);
    renderer_composite_decode_texture = composite_output_texture
        ? static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(composite_output_texture))
        : ImTextureID_Invalid;

    if (!composite_index_texture || !composite_index_transfer ||
        !composite_chroma_texture || !composite_output_texture)
    {
        Log("Composite decode texture allocation failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

// Runs both composite/S-Video decode passes: chroma (into
// composite_chroma_texture) then luma+colour (into composite_output_texture,
// which composite_decode_texture points at). Only called when
// composite_decode_enabled && composite_decode_ready, alongside the blur
// test pass.
void render_composite_decode_passes(SDL_GPUCommandBuffer* command)
{
    if (!ensure_composite_decode_textures() || !ensure_composite_decode_pipelines())
        return;

    const float uniformData[4] = {
        composite_decode_mode_is_composite ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};

    {
        SDL_GPUColorTargetInfo target = {};
        target.texture = composite_chroma_texture;
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, composite_chroma_pipeline);
        SDL_GPUTextureSamplerBinding indexBinding = {composite_index_texture, nearest_sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &indexBinding, 1);
        SDL_PushGPUFragmentUniformData(command, 0, uniformData, sizeof(uniformData));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    {
        SDL_GPUColorTargetInfo target = {};
        target.texture = composite_output_texture;
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, composite_luma_pipeline);
        SDL_GPUTextureSamplerBinding samplerBindings[2] = {
            {composite_index_texture, nearest_sampler},
            {composite_chroma_texture, linear_sampler},
        };
        SDL_BindGPUFragmentSamplers(pass, 0, samplerBindings, 2);
        SDL_PushGPUFragmentUniformData(command, 0, uniformData, sizeof(uniformData));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
}

// Rebuilds crt_consumer_texture at emu_texture's current native size times
// crt_consumer_debug_upscale - called both when emu_texture's own geometry
// changes and when the upscale factor itself is changed live from the
// debug window (see renderer_set_crt_consumer_debug_upscale).
void recreate_crt_consumer_texture()
{
    // Guards the live-tuning path (renderer_set_crt_consumer_debug_upscale)
    // as well as the geometry-change path (ensure_emulator_texture already
    // waited once above, so this is a harmless no-op there).
    SDL_WaitForGPUIdle(gpu_device);
    if (crt_consumer_texture) SDL_ReleaseGPUTexture(gpu_device, crt_consumer_texture);
    crt_consumer_texture = create_render_target(
        emu_texture_width * crt_consumer_debug_upscale,
        emu_texture_height * crt_consumer_debug_upscale);
    renderer_crt_consumer_texture = crt_consumer_texture
        ? static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(crt_consumer_texture))
        : ImTextureID_Invalid;
}

// Same role as recreate_crt_consumer_texture, for the second display-stage
// candidate.
void recreate_crt_lottes_texture()
{
    SDL_WaitForGPUIdle(gpu_device);
    if (crt_lottes_texture) SDL_ReleaseGPUTexture(gpu_device, crt_lottes_texture);
    crt_lottes_texture = create_render_target(
        emu_texture_width * crt_lottes_debug_upscale,
        emu_texture_height * crt_lottes_debug_upscale);
    renderer_crt_lottes_texture = crt_lottes_texture
        ? static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(crt_lottes_texture))
        : ImTextureID_Invalid;
}

bool ensure_emulator_texture()
{
    const GC_VideoFrameDescriptor descriptor = emu_get_video_frame_descriptor();
    if (!descriptor.IsValid())
        return false;
    if (emu_texture && emu_transfer &&
        emu_texture_width == descriptor.frame_width &&
        emu_texture_height == descriptor.frame_height)
        return true;

    SDL_GPUTexture* replacementTexture = create_texture(
        descriptor.frame_width, descriptor.frame_height);
    SDL_GPUTransferBuffer* replacementTransfer = create_transfer(
        descriptor.frame_width, descriptor.frame_height);
    if (!replacementTexture || !replacementTransfer)
    {
        release_texture(replacementTexture, replacementTransfer);
        Log("SDL_GPU emulator texture allocation failed: %s\n", SDL_GetError());
        return false;
    }

    // Geometry changes are rare (region/device/output-mode changes).  Waiting
    // here makes releasing the previous texture unambiguous on every backend.
    SDL_WaitForGPUIdle(gpu_device);
    release_texture(emu_texture, emu_transfer);
    emu_texture = replacementTexture;
    emu_transfer = replacementTransfer;
    emu_texture_width = descriptor.frame_width;
    emu_texture_height = descriptor.frame_height;
    renderer_emu_texture = static_cast<ImTextureID>(
        reinterpret_cast<uintptr_t>(emu_texture));

    if (crt_test_texture) SDL_ReleaseGPUTexture(gpu_device, crt_test_texture);
    crt_test_texture = create_render_target(descriptor.frame_width, descriptor.frame_height);
    renderer_crt_test_texture = crt_test_texture
        ? static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(crt_test_texture))
        : ImTextureID_Invalid;

    recreate_crt_consumer_texture();
    recreate_crt_lottes_texture();
    return true;
}

bool upload_rgb(SDL_GPUCommandBuffer* command, SDL_GPUTexture* texture, SDL_GPUTransferBuffer* transfer,
                const u8* source, int width, int height)
{
    u8* destination = static_cast<u8*>(SDL_MapGPUTransferBuffer(gpu_device, transfer, true));
    if (!destination)
        return false;
    const int pixels = width * height;
    for (int pixel = 0; pixel < pixels; ++pixel)
    {
        destination[pixel * 4 + 0] = source[pixel * 3 + 0];
        destination[pixel * 4 + 1] = source[pixel * 3 + 1];
        destination[pixel * 4 + 2] = source[pixel * 3 + 2];
        destination[pixel * 4 + 3] = 0xff;
    }
    SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

    SDL_GPUTextureTransferInfo from = {};
    from.transfer_buffer = transfer;
    from.pixels_per_row = width;
    from.rows_per_layer = height;
    SDL_GPUTextureRegion to = {};
    to.texture = texture;
    to.w = width;
    to.h = height;
    to.d = 1;
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(command);
    SDL_UploadToGPUTexture(pass, &from, &to, false);
    SDL_EndGPUCopyPass(pass);
    return true;
}

// Temporal resampling, for the refresh rates the scheduler cannot lock to.
//
// A 60 Hz display against a 50.159 Hz PAL machine has to repeat one frame in
// six, and 144 or 165 Hz divides into nothing. The repeat reads as a step
// where the picture waits and then carries on - very visible in Uridium,
// which scrolls one pixel per frame and so has no roughness of its own to
// hide it behind.
//
// The filter is a box over the presentation interval that just elapsed:
// mpv's "oversample" tscale, which it uses by default for the same job.
// Where that interval fell entirely inside one emulated frame, that frame is
// shown untouched; where a frame boundary fell inside it, the two are mixed
// by how much of the interval each covered.
//
// The distinction from a plain crossfade matters, and it is why this is not
// the simpler thing that was written first. Crossfading blends every single
// presentation, so it softens the picture permanently to fix an event that -
// at 59.9227 against 60 Hz - happens once every 775 frames. Oversample
// leaves those 774 frames exactly as the VDP drew them.
//
// It also needs no frame from the future, because it integrates over the
// interval already gone. The lag is one presentation, 16.67 ms, rather than
// a whole emulated frame.
//
// Why this is correct rather than an approximation, for this content: at one
// pixel per frame the mix of two frames is algebraically identical to a
// bilinear sub-pixel shift, since frame N *is* frame N-1 shifted by one.
// Nothing is being smeared - the picture is being placed between two pixels,
// which is what it would do on a display running at the machine's own rate.
// Motion estimation would find vectors of length one and arrive here anyway,
// with block artefacts of its own.
//
// Done on the CPU deliberately. The reason to prefer a shader is avoiding a
// per-pixel pass, and this frame is 284x243 at most - about 207 KB, a lerp
// costing microseconds. A shader would also have meant a new fragment
// program in three compiled forms, and the Metal toolchain is a separate
// several-hundred-megabyte Xcode component that is not installed here.
//
// emu_frame_buffer itself is never touched, which is the important part: the
// screenshot tools, the full raster view and the recorder read it, and a
// frame they capture has to be the one the VDP drew, not an average of two.
u8* ghost_previous = nullptr;
u8* ghost_blended = nullptr;
std::size_t ghost_capacity = 0;
unsigned long long ghost_serial = 0;
bool ghost_has_previous = false;

void ghost_release(void)
{
    delete[] ghost_previous;
    delete[] ghost_blended;
    ghost_previous = nullptr;
    ghost_blended = nullptr;
    ghost_capacity = 0;
    ghost_has_previous = false;
}

// How much of the conversion is actually happening, since the claim that it
// leaves almost everything alone is the reason to prefer it to a blur - and a
// claim on screen can be checked.
unsigned long long ghost_presentations = 0;
unsigned long long ghost_converted = 0;

// Returns the buffer to upload: the resampled frame when a boundary fell
// inside the last presentation, the untouched frame otherwise.
const u8* ghost_apply(const u8* current, int width, int height)
{
    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
    if (bytes == 0)
        return current;

    const SchedulerDiagnostics pacing = scheduler_get_diagnostics();

    // Nothing to resample when one frame is produced per presentation: the
    // machine and the screen are in step, so there is no boundary to fall
    // anywhere. Switched off rather than left running at weight 1, so the
    // history does not have to be maintained for nothing.
    // Accurate shows what the VDP drew, untouched - both its variants, with
    // and without the blank. Locked mode has one frame per presentation, so
    // there is no boundary inside an interval to blend across. Both leave the
    // picture alone.
    if (!config_video_pacing_aligned() || pacing.display_locked)
    {
        // Dropped rather than kept warm: coming back later would otherwise
        // blend across whatever happened in between.
        ghost_has_previous = false;
        ghost_presentations = 0;
        ghost_converted = 0;
        return current;
    }

    ++ghost_presentations;

    if (bytes != ghost_capacity)
    {
        ghost_release();
        ghost_previous = new u8[bytes]();
        ghost_blended = new u8[bytes]();
        ghost_capacity = bytes;
    }

    // Only when the beam has actually drawn a new frame. Presentations run
    // faster than the machine here by definition - that is the problem being
    // solved - and advancing the history on a repeat would throw the real
    // previous frame away and mix a frame with itself.
    const unsigned long long serial = emu_get_frame_serial();
    const bool fresh = (serial != ghost_serial);

    if (!ghost_has_previous)
    {
        std::memcpy(ghost_previous, current, bytes);
        ghost_serial = serial;
        ghost_has_previous = true;
        return current;
    }

    const float weight = static_cast<float>(pacing.blend_weight);

    // Weight 1 means the whole presentation fell inside this frame. Upload it
    // as it is - not through the lerp, which would only cost time to produce
    // the same bytes, and not exactly the same ones after rounding.
    if (weight >= 0.999f)
    {
        if (fresh)
        {
            std::memcpy(ghost_previous, current, bytes);
            ghost_serial = serial;
        }
        return current;
    }

    ++ghost_converted;

    const float previous_weight = 1.0f - weight;
    for (std::size_t i = 0; i < bytes; ++i)
    {
        const float mixed = static_cast<float>(current[i]) * weight +
                            static_cast<float>(ghost_previous[i]) * previous_weight;
        ghost_blended[i] = static_cast<u8>(mixed + 0.5f);
    }

    if (fresh)
    {
        std::memcpy(ghost_previous, current, bytes);
        ghost_serial = serial;
    }

    return ghost_blended;
}

void upload_images(SDL_GPUCommandBuffer* command)
{
    const GC_VideoFrameDescriptor descriptor = emu_get_video_frame_descriptor();
    current_runtime.screen_width = descriptor.frame_width;
    current_runtime.screen_height = descriptor.frame_height;
    current_runtime.region = descriptor.region;
    if (descriptor.frame_width != emu_texture_width ||
        descriptor.frame_height != emu_texture_height)
        return;
    upload_rgb(command, emu_texture, emu_transfer,
               ghost_apply(emu_frame_buffer, descriptor.frame_width,
                           descriptor.frame_height),
               descriptor.frame_width, descriptor.frame_height);
    if (!config_debug.debug)
        return;

    if (config_debug.show_video)
    {
        // The machine can be paused after VRAM changed but before
        // emu_run_frame() refreshed the debug cache. Decode at presentation
        // time so Live SAT really means live.
        emu_refresh_debug_sprite_buffers();
        upload_rgb(command, debug_background_texture, debug_background_transfer, emu_debug_background_buffer, 256, 256);
        upload_rgb(command, debug_tiles_texture, debug_tiles_transfer, emu_debug_tile_buffer, 256, 256);
        for (int sprite = 0; sprite < 64; ++sprite)
            upload_rgb(command, debug_sprite_textures[sprite], debug_sprite_transfers[sprite], emu_debug_sprite_buffers[sprite], 16, 16);
    }

    if (config_debug.show_rom_inspector)
    {
        upload_rgb(command, debug_rom_inspector_texture, debug_rom_inspector_transfer,
                   emu_debug_rom_inspector_buffer,
                   ROM_INSPECTOR_TEXTURE_SIZE, ROM_INSPECTOR_TEXTURE_SIZE);
    }
}

void set_emulator_sampler_callback(const ImDrawList*, const ImDrawCmd*)
{
    auto* state = static_cast<ImGui_ImplSDLGPU3_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (state)
        // Only meaningful for None/Bilinear - in CRT mode the main window
        // draws renderer_crt_lottes_live_texture instead of emu_texture
        // (see gui.cpp), already rendered at the real output resolution, so
        // this sampler choice has no visible effect on that path either way.
        state->SamplerCurrent = active_video_output().postprocessing == POSTPROCESSING_BILINEAR
            ? state->SamplerDefault : nearest_sampler;
}

void reset_sampler_callback(const ImDrawList*, const ImDrawCmd*)
{
    auto* state = static_cast<ImGui_ImplSDLGPU3_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (state)
        state->SamplerCurrent = state->SamplerDefault;
}
}

bool renderer_init(SDL_GPUDevice* device, SDL_Window* window)
{
    gpu_device = device;
    gpu_window = window;
    renderer_gpu_driver = SDL_GetGPUDeviceDriver(gpu_device);
    SDL_GPUSamplerCreateInfo sampler_info = {};
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_sampler = SDL_CreateGPUSampler(gpu_device, &sampler_info);

    SDL_GPUSamplerCreateInfo linear_sampler_info = sampler_info;
    linear_sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    linear_sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    linear_sampler = SDL_CreateGPUSampler(gpu_device, &linear_sampler_info);

    if (!ensure_emulator_texture())
        return false;
    debug_background_texture = create_texture(256, 256);
    debug_background_transfer = create_transfer(256, 256);
    debug_tiles_texture = create_texture(256, 256);
    debug_tiles_transfer = create_transfer(256, 256);
    for (int sprite = 0; sprite < 64; ++sprite)
    {
        debug_sprite_textures[sprite] = create_texture(16, 16);
        debug_sprite_transfers[sprite] = create_transfer(16, 16);
    }
    debug_rom_inspector_texture = create_texture(ROM_INSPECTOR_TEXTURE_SIZE, ROM_INSPECTOR_TEXTURE_SIZE);
    debug_rom_inspector_transfer = create_transfer(ROM_INSPECTOR_TEXTURE_SIZE, ROM_INSPECTOR_TEXTURE_SIZE);
    if (!nearest_sampler || !emu_texture || !emu_transfer || !debug_background_texture || !debug_background_transfer || !debug_tiles_texture || !debug_tiles_transfer || !debug_rom_inspector_texture || !debug_rom_inspector_transfer)
    {
        Log("SDL_GPU texture allocation failed: %s\n", SDL_GetError());
        return false;
    }
    renderer_emu_debug_vram_background = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(debug_background_texture));
    renderer_emu_debug_vram_tiles = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(debug_tiles_texture));
    for (int sprite = 0; sprite < 64; ++sprite)
        renderer_emu_debug_vram_sprites[sprite] = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(debug_sprite_textures[sprite]));
    renderer_emu_debug_rom_inspector = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(debug_rom_inspector_texture));

    ImGui_ImplSDLGPU3_InitInfo info = {};
    info.Device = gpu_device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_device, gpu_window);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    info.PresentMode = config_video_vsync() ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
    return ImGui_ImplSDLGPU3_Init(&info);
}

void renderer_destroy(void)
{
    // Plain heap buffers, not GPU resources, so this is safe before the
    // early return below - and has to be, or a headless run leaks them.
    ghost_release();

    if (!gpu_device) return;
    SDL_WaitForGPUIdle(gpu_device);
    ImGui_ImplSDLGPU3_Shutdown();
    release_texture(emu_texture, emu_transfer);
    release_texture(debug_background_texture, debug_background_transfer);
    release_texture(debug_tiles_texture, debug_tiles_transfer);
    for (int sprite = 0; sprite < 64; ++sprite)
        release_texture(debug_sprite_textures[sprite], debug_sprite_transfers[sprite]);
    release_texture(debug_rom_inspector_texture, debug_rom_inspector_transfer);
    if (nearest_sampler) SDL_ReleaseGPUSampler(gpu_device, nearest_sampler);
    nearest_sampler = nullptr;
    if (linear_sampler) SDL_ReleaseGPUSampler(gpu_device, linear_sampler);
    linear_sampler = nullptr;
    if (crt_blur_pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu_device, crt_blur_pipeline);
    crt_blur_pipeline = nullptr;
    if (crt_test_texture) SDL_ReleaseGPUTexture(gpu_device, crt_test_texture);
    crt_test_texture = nullptr;
    if (composite_chroma_pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu_device, composite_chroma_pipeline);
    composite_chroma_pipeline = nullptr;
    if (composite_luma_pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu_device, composite_luma_pipeline);
    composite_luma_pipeline = nullptr;
    release_texture(composite_index_texture, composite_index_transfer);
    if (composite_chroma_texture) SDL_ReleaseGPUTexture(gpu_device, composite_chroma_texture);
    composite_chroma_texture = nullptr;
    if (composite_output_texture) SDL_ReleaseGPUTexture(gpu_device, composite_output_texture);
    composite_output_texture = nullptr;
    composite_decode_ready = false;
    if (crt_consumer_pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu_device, crt_consumer_pipeline);
    crt_consumer_pipeline = nullptr;
    if (crt_consumer_texture) SDL_ReleaseGPUTexture(gpu_device, crt_consumer_texture);
    crt_consumer_texture = nullptr;
    if (crt_lottes_pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu_device, crt_lottes_pipeline);
    crt_lottes_pipeline = nullptr;
    if (crt_lottes_texture) SDL_ReleaseGPUTexture(gpu_device, crt_lottes_texture);
    crt_lottes_texture = nullptr;
    if (crt_lottes_live_texture) SDL_ReleaseGPUTexture(gpu_device, crt_lottes_live_texture);
    crt_lottes_live_texture = nullptr;
    crt_lottes_live_width = 0;
    crt_lottes_live_height = 0;
    if (advanced_scaling_pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu_device, advanced_scaling_pipeline);
    advanced_scaling_pipeline = nullptr;
    if (advanced_scaling_live_texture) SDL_ReleaseGPUTexture(gpu_device, advanced_scaling_live_texture);
    advanced_scaling_live_texture = nullptr;
    advanced_scaling_live_width = 0;
    advanced_scaling_live_height = 0;
    renderer_emu_texture = ImTextureID_Invalid;
    renderer_emu_debug_vram_background = ImTextureID_Invalid;
    renderer_emu_debug_vram_tiles = ImTextureID_Invalid;
    renderer_emu_debug_rom_inspector = ImTextureID_Invalid;
    renderer_crt_test_texture = ImTextureID_Invalid;
    renderer_composite_decode_texture = ImTextureID_Invalid;
    renderer_crt_consumer_texture = ImTextureID_Invalid;
    renderer_crt_lottes_texture = ImTextureID_Invalid;
    renderer_crt_lottes_live_texture = ImTextureID_Invalid;
    renderer_advanced_scaling_live_texture = ImTextureID_Invalid;
    for (SDL_GPUTexture* texture : auxiliary_textures)
        SDL_ReleaseGPUTexture(gpu_device, texture);
    auxiliary_textures.clear();
    emu_texture_width = 0;
    emu_texture_height = 0;
    gpu_device = nullptr;
    gpu_window = nullptr;
}

static ImTextureID upload_decoded_png(unsigned char* pixels, int image_width,
                                      int image_height, int* width, int* height)
{
    if (!gpu_device || !pixels || image_width <= 0 || image_height <= 0)
    {
        stbi_image_free(pixels);
        return ImTextureID_Invalid;
    }

    SDL_GPUTexture* texture = create_texture(image_width, image_height);
    SDL_GPUTransferBuffer* transfer = create_transfer(image_width, image_height);
    if (!texture || !transfer)
    {
        stbi_image_free(pixels);
        release_texture(texture, transfer);
        return ImTextureID_Invalid;
    }

    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, transfer, true);
    if (!mapped)
    {
        stbi_image_free(pixels);
        release_texture(texture, transfer);
        return ImTextureID_Invalid;
    }
    memcpy(mapped, pixels, (size_t)image_width * image_height * 4);
    SDL_UnmapGPUTransferBuffer(gpu_device, transfer);
    stbi_image_free(pixels);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(gpu_device);
    if (!command)
    {
        release_texture(texture, transfer);
        return ImTextureID_Invalid;
    }
    SDL_GPUTextureTransferInfo from = {};
    from.transfer_buffer = transfer;
    from.pixels_per_row = image_width;
    from.rows_per_layer = image_height;
    SDL_GPUTextureRegion to = {};
    to.texture = texture;
    to.w = image_width;
    to.h = image_height;
    to.d = 1;
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(command);
    SDL_UploadToGPUTexture(pass, &from, &to, false);
    SDL_EndGPUCopyPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(command))
    {
        release_texture(texture, transfer);
        return ImTextureID_Invalid;
    }
    SDL_WaitForGPUIdle(gpu_device);
    SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);

    auxiliary_textures.push_back(texture);
    if (width) *width = image_width;
    if (height) *height = image_height;
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
}

ImTextureID renderer_load_png_texture(const char* file_path, int* width, int* height)
{
    if (!gpu_device || !file_path)
        return ImTextureID_Invalid;

    int image_width = 0;
    int image_height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(file_path, &image_width, &image_height,
                                      &channels, 4);
    if (!pixels)
    {
        Log("Unable to load PNG texture %s: %s\n", file_path,
            stbi_failure_reason());
        return ImTextureID_Invalid;
    }
    return upload_decoded_png(pixels, image_width, image_height, width, height);
}

ImTextureID renderer_load_png_texture_memory(const unsigned char* data,
                                             std::size_t data_size,
                                             int* width, int* height)
{
    if (!gpu_device || !data || data_size == 0 || data_size > INT_MAX)
        return ImTextureID_Invalid;

    int image_width = 0;
    int image_height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, static_cast<int>(data_size), &image_width, &image_height,
        &channels, 4);
    if (!pixels)
    {
        Log("Unable to load embedded PNG texture: %s\n",
            stbi_failure_reason());
        return ImTextureID_Invalid;
    }
    return upload_decoded_png(pixels, image_width, image_height, width, height);
}

void renderer_release_texture(ImTextureID texture_id)
{
    if (!gpu_device || texture_id == ImTextureID_Invalid)
        return;
    SDL_WaitForGPUIdle(gpu_device);
    SDL_GPUTexture* texture =
        reinterpret_cast<SDL_GPUTexture*>(static_cast<uintptr_t>(texture_id));
    SDL_ReleaseGPUTexture(gpu_device, texture);
    auxiliary_textures.erase(
        std::remove(auxiliary_textures.begin(), auxiliary_textures.end(), texture),
        auxiliary_textures.end());
}

void renderer_begin_render(void)
{
    ensure_emulator_texture();
    ImGui_ImplSDLGPU3_NewFrame();
}

void renderer_render(void)
{
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(gpu_device);
    if (!command) return;
    upload_images(command);
    if (crt_test_enabled)
        render_crt_test_pass(command);
    if (composite_decode_enabled && composite_decode_ready)
        render_composite_decode_passes(command);
    if (crt_consumer_enabled)
        render_crt_consumer_pass(command);
    if (crt_lottes_enabled)
        render_crt_lottes_pass(command);
    if (active_video_output().postprocessing == POSTPROCESSING_CRT_LOTTES)
        render_crt_lottes_live_pass(command);
    if (active_video_output().postprocessing == POSTPROCESSING_ADVANCED_SCALING)
        render_advanced_scaling_live_pass(command);
    ImDrawData* draw_data = ImGui::GetDrawData();
    SDL_GPUTexture* swapchain = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, gpu_window, &swapchain, nullptr, nullptr))
    {
        SDL_CancelGPUCommandBuffer(command);
        return;
    }
    if (swapchain)
    {
        SDL_GPUColorTargetInfo target = {};
        target.texture = swapchain;
        target.clear_color = SDL_FColor{0.10f, 0.10f, 0.10f, 1.0f};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command);
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command, pass);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(command);
}

void renderer_end_render(void) {}

// Five call sites set this - the video menu, fast forward, the recorder -
// and none of them agreed on who remembered the result. The scheduler needs
// to know, because vertical sync blocking inside the present call is already
// a wait: sleeping again afterwards would pace the loop twice.
static bool vsync_enabled = true;

void renderer_set_vsync(bool enabled)
{
    vsync_enabled = enabled;
    if (gpu_device && gpu_window)
        SDL_SetGPUSwapchainParameters(gpu_device, gpu_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE);
}

bool renderer_get_vsync(void)
{
    return vsync_enabled;
}

RendererConversionStats renderer_get_conversion_stats(void)
{
    RendererConversionStats s;
    s.presentations = ghost_presentations;
    s.converted = ghost_converted;
    return s;
}

void renderer_set_crt_test_enabled(bool enabled)
{
    crt_test_enabled = enabled;
}

bool renderer_crt_test_enabled(void)
{
    return crt_test_enabled;
}

void renderer_set_crt_test_blur_enabled(bool enabled)
{
    crt_test_blur_enabled = enabled;
}

bool renderer_crt_test_blur_enabled(void)
{
    return crt_test_blur_enabled;
}

void renderer_set_crt_test_blur_radius(float radiusDots)
{
    crt_test_blur_radius = radiusDots;
}

float renderer_crt_test_blur_radius(void)
{
    return crt_test_blur_radius;
}

bool renderer_load_composite_decode_capture(const char* file_path, std::string* errorOut)
{
    composite_decode_ready = false;
    if (!gpu_device || file_path == nullptr)
    {
        if (errorOut) *errorOut = "renderer not initialised";
        return false;
    }

    CrtSignalCaptureLoader loader;
    std::string parseError;
    if (!loader.Load(file_path, &parseError))
    {
        if (errorOut) *errorOut = parseError;
        return false;
    }

    if (!ensure_composite_decode_textures())
    {
        if (errorOut) *errorOut = "GPU texture allocation failed";
        return false;
    }

    // One-shot upload outside the per-frame command buffer - this runs
    // once when a capture is (re)loaded from the GUI, not every frame.
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(gpu_device);
    if (!command)
    {
        if (errorOut) *errorOut = "could not acquire a command buffer";
        return false;
    }

    uint8_t* destination = static_cast<uint8_t*>(
        SDL_MapGPUTransferBuffer(gpu_device, composite_index_transfer, true));
    if (!destination)
    {
        SDL_CancelGPUCommandBuffer(command);
        if (errorOut) *errorOut = "could not map the transfer buffer";
        return false;
    }
    std::memcpy(destination, loader.indexTexels.data(), loader.indexTexels.size());
    SDL_UnmapGPUTransferBuffer(gpu_device, composite_index_transfer);

    SDL_GPUTextureTransferInfo from = {};
    from.transfer_buffer = composite_index_transfer;
    from.pixels_per_row = CrtSignalCaptureLoader::kPictureDots;
    from.rows_per_layer = CrtSignalCaptureLoader::kPictureLines;
    SDL_GPUTextureRegion to = {};
    to.texture = composite_index_texture;
    to.w = CrtSignalCaptureLoader::kPictureDots;
    to.h = CrtSignalCaptureLoader::kPictureLines;
    to.d = 1;
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(command);
    SDL_UploadToGPUTexture(copyPass, &from, &to, false);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(command);

    composite_decode_ready = true;
    return true;
}

bool renderer_composite_decode_ready(void)
{
    return composite_decode_ready;
}

void renderer_set_composite_decode_enabled(bool enabled)
{
    composite_decode_enabled = enabled;
}

bool renderer_composite_decode_enabled(void)
{
    return composite_decode_enabled;
}

void renderer_set_composite_decode_mode(bool composite)
{
    composite_decode_mode_is_composite = composite;
}

bool renderer_composite_decode_mode(void)
{
    return composite_decode_mode_is_composite;
}

void renderer_set_crt_consumer_enabled(bool enabled)
{
    crt_consumer_enabled = enabled;
}

bool renderer_crt_consumer_enabled(void)
{
    return crt_consumer_enabled;
}

void renderer_set_crt_consumer_params(const RendererCrtConsumerParams& params)
{
    crt_consumer_params = params;
}

RendererCrtConsumerParams renderer_crt_consumer_params(void)
{
    return crt_consumer_params;
}

void renderer_set_crt_consumer_debug_upscale(int upscale)
{
    if (upscale < kCrtConsumerDebugUpscaleMin) upscale = kCrtConsumerDebugUpscaleMin;
    if (upscale > kCrtConsumerDebugUpscaleMax) upscale = kCrtConsumerDebugUpscaleMax;
    if (upscale == crt_consumer_debug_upscale)
        return;
    crt_consumer_debug_upscale = upscale;
    if (emu_texture)
        recreate_crt_consumer_texture();
}

int renderer_crt_consumer_debug_upscale(void)
{
    return crt_consumer_debug_upscale;
}

void renderer_set_crt_lottes_enabled(bool enabled)
{
    crt_lottes_enabled = enabled;
}

bool renderer_crt_lottes_enabled(void)
{
    return crt_lottes_enabled;
}

void renderer_set_crt_lottes_params(const RendererCrtLottesParams& params)
{
    crt_lottes_params = params;
}

RendererCrtLottesParams renderer_crt_lottes_params(void)
{
    return crt_lottes_params;
}

void renderer_set_crt_lottes_debug_upscale(int upscale)
{
    if (upscale < kCrtConsumerDebugUpscaleMin) upscale = kCrtConsumerDebugUpscaleMin;
    if (upscale > kCrtConsumerDebugUpscaleMax) upscale = kCrtConsumerDebugUpscaleMax;
    if (upscale == crt_lottes_debug_upscale)
        return;
    crt_lottes_debug_upscale = upscale;
    if (emu_texture)
        recreate_crt_lottes_texture();
}

int renderer_crt_lottes_debug_upscale(void)
{
    return crt_lottes_debug_upscale;
}

void renderer_set_crt_lottes_output_size(int width, int height)
{
    if (width == crt_lottes_live_width && height == crt_lottes_live_height)
        return;
    recreate_crt_lottes_live_texture(width, height);
}

bool renderer_ensure_crt_lottes_pipeline(void)
{
    return ensure_crt_lottes_pipeline();
}

void renderer_set_advanced_scaling_output_size(int width, int height)
{
    if (width == advanced_scaling_live_width && height == advanced_scaling_live_height)
        return;
    recreate_advanced_scaling_live_texture(width, height);
}

bool renderer_ensure_advanced_scaling_pipeline(void)
{
    return ensure_advanced_scaling_pipeline();
}

void renderer_begin_emulator_image(void)
{
    ImGui::GetWindowDrawList()->AddCallback(set_emulator_sampler_callback, nullptr);
}

void renderer_end_emulator_image(void)
{
    ImGui::GetWindowDrawList()->AddCallback(reset_sampler_callback, nullptr);
}
