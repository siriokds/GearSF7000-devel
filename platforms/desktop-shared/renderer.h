/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#ifndef RENDERER_H
#define	RENDERER_H

#include <cstddef>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "imgui/imgui.h"

#ifdef RENDERER_IMPORT
    #define EXTERN
#else
    #define EXTERN extern
#endif

EXTERN ImTextureID renderer_emu_texture;
EXTERN ImTextureID renderer_emu_debug_vram_background;
EXTERN ImTextureID renderer_emu_debug_vram_tiles;
EXTERN ImTextureID renderer_emu_debug_vram_sprites[64];
// ROM Inspector (DOCS/ROM_INSPECTOR_PLAN.md point 4) - decodes an
// arbitrary external scratch buffer, not live VRAM. Fixed at
// ROM_INSPECTOR_TEXTURE_SIZE so the GPU texture never needs recreating
// when the user changes tiles-per-row; only the UV window shown changes.
EXTERN ImTextureID renderer_emu_debug_rom_inspector;
// Output of the CRT test pass (see shaders/crt_pass/README.md) - a plain
// GPU-side horizontal blur of renderer_emu_texture, proving the
// render-to-texture plumbing for a future real decode, not a decode
// itself. Only written when renderer_set_crt_test_enabled(true).
EXTERN ImTextureID renderer_crt_test_texture;
// Real two-pass composite/S-Video decode (see shaders/crt_pass/
// composite_chroma.frag / composite_luma.frag), running on a captured
// GCRT signal file rather than live gameplay - see
// renderer_load_composite_decode_capture. Output is 284x294, fixed by the
// shaders' own hardcoded picture geometry.
EXTERN ImTextureID renderer_composite_decode_texture;
// Display-stage test (trimmed GearSystem crt_consumer.glsl port, see
// shaders/crt_pass/crt_consumer.frag) - runs on renderer_emu_texture
// directly (live RGB, whatever produced it), matching real gameplay
// resolution.
EXTERN ImTextureID renderer_crt_consumer_texture;
// Display-stage test (trimmed crt-lottes-fast.glsl port, see shaders/
// crt_pass/crt_lottes.frag) - same role as renderer_crt_consumer_texture,
// a second candidate for the same display stage, run and compared
// side by side rather than replacing it.
EXTERN ImTextureID renderer_crt_lottes_texture;
// Live-gameplay CRT (Lottes) output (Video > Postprocessing > CRT
// (Lottes)) - always the real on-screen pixel size, see
// renderer_set_crt_lottes_output_size. Distinct from
// renderer_crt_lottes_texture, which is the Debug window's own scratch
// texture at a manufactured debug-upscale resolution.
EXTERN ImTextureID renderer_crt_lottes_live_texture;
// Live-gameplay Advanced Scaling output (Video > Postprocessing >
// Advanced Scaling) - same real-on-screen-size reasoning as
// renderer_crt_lottes_live_texture.
EXTERN ImTextureID renderer_advanced_scaling_live_texture;
EXTERN const char* renderer_gpu_driver;

// Not derived from a datasheet or a verified reference - a starting
// point to tune by eye, same as the original shader's own parameters are
// meant to be tuned per display. See crt_consumer.frag for what each one
// does.
struct RendererCrtConsumerParams
{
    float blurx = 0.30f;
    float blury = 0.0f;
    float scanlow = 3.0f;
    float scanhigh = 7.0f;
    float beamlow = 0.5f;
    float beamhigh = 1.2f;
    float preserve = 0.0f;
    float brightboost1 = 1.0f;
    float brightboost2 = 1.2f;
    float gammaOut = 2.2f;
};

// Same disclaimer as RendererCrtConsumerParams - starting points to tune
// by eye. maskType mirrors crt-lottes-fast.glsl's own MASK parameter:
// 0 = none, 1 = aperture grille, 2 = aperture grille (brighter/subtractive),
// 3 = shadow mask (horizontally stretched). Defaults to 0 (mask off) so the
// pass starts as scanline-only, as requested - compare against other
// values from the debug window rather than assuming the mask should be on.
// Defaults match config_video's crt_lottes_* ones - tuned by eye and
// confirmed ("valori ok") after the bold-dark-edges port landed.
struct RendererCrtLottesParams
{
    float maskType = 0.0f;
    float maskIntensity = 0.12f;
    float scanlineThinness = 0.39f;
    float scanBlur = 6.0f;
    float crtGamma = 2.54f;
    // "Bold dark edges" - see advanced_scaling.frag / RendererCrtLottesParams'
    // own field-by-field twin in config_video for the full story.
    float blackThreshold = 0.15f;
    float boldness = 0.42f;
    float edgeRadius = 0.41f;
};

EXTERN bool renderer_init(SDL_GPUDevice* device, SDL_Window* window);
EXTERN void renderer_destroy(void);
EXTERN void renderer_begin_render(void);
EXTERN void renderer_render(void);
EXTERN void renderer_end_render(void);
EXTERN void renderer_set_vsync(bool enabled);
EXTERN bool renderer_get_vsync(void);
EXTERN ImTextureID renderer_load_png_texture(const char* file_path, int* width, int* height);
EXTERN ImTextureID renderer_load_png_texture_memory(const unsigned char* data,
                                                    std::size_t data_size,
                                                    int* width, int* height);
EXTERN void renderer_release_texture(ImTextureID texture_id);

// How many presentations the frame converter has seen since it was last
// switched on, and how many of those it actually had to blend. The point of
// the box filter is that the second number stays small - 774 of 775 frames
// pass through untouched in NTSC - so it is worth being able to check rather
// than take on trust. Both reset when the converter stops.
struct RendererConversionStats
{
    unsigned long long presentations;
    unsigned long long converted;
};
EXTERN RendererConversionStats renderer_get_conversion_stats(void);
EXTERN void renderer_set_crt_test_enabled(bool enabled);
EXTERN bool renderer_crt_test_enabled(void);
// Horizontal-only blur radius, in TMS9918 dots (see shaders/crt_pass/
// blur.frag for why dots and not a fraction of output width).
EXTERN void renderer_set_crt_test_blur_enabled(bool enabled);
EXTERN bool renderer_crt_test_blur_enabled(void);
EXTERN void renderer_set_crt_test_blur_radius(float radiusDots);
EXTERN float renderer_crt_test_blur_radius(void);
// Parses file_path as a GCRT capture (see crt_signal_capture_loader.h) and
// uploads it as the decode's source texture. Returns false and leaves the
// previous capture (if any) in place on failure - errorOut, if non-null,
// receives a human-readable reason.
EXTERN bool renderer_load_composite_decode_capture(const char* file_path, std::string* errorOut);
EXTERN bool renderer_composite_decode_ready(void);
EXTERN void renderer_set_composite_decode_enabled(bool enabled);
EXTERN bool renderer_composite_decode_enabled(void);
// true = composite (luma/chroma share a wire, dot crawl); false = S-Video
// (separate wires, no dot crawl but chroma bandwidth still bleeds).
EXTERN void renderer_set_composite_decode_mode(bool composite);
EXTERN bool renderer_composite_decode_mode(void);
EXTERN void renderer_set_crt_consumer_enabled(bool enabled);
EXTERN bool renderer_crt_consumer_enabled(void);
EXTERN void renderer_set_crt_consumer_params(const RendererCrtConsumerParams& params);
EXTERN RendererCrtConsumerParams renderer_crt_consumer_params(void);
// Debug-only: how many render-target rows this test pass allocates per
// source row (clamped to [1, 16]), so the scanline term has real row-to-row
// variation to work with - see renderer.cpp's kCrtConsumerDebugUpscale*
// comment for why. A real final pass would never need this; it gets the
// same effect for free from the window's own resolution. Changing it
// recreates renderer_crt_consumer_texture immediately.
EXTERN void renderer_set_crt_consumer_debug_upscale(int upscale);
EXTERN int renderer_crt_consumer_debug_upscale(void);
EXTERN void renderer_set_crt_lottes_enabled(bool enabled);
EXTERN bool renderer_crt_lottes_enabled(void);
EXTERN void renderer_set_crt_lottes_params(const RendererCrtLottesParams& params);
EXTERN RendererCrtLottesParams renderer_crt_lottes_params(void);
// Same role/reasoning as renderer_set_crt_consumer_debug_upscale, kept as
// an independent control so the two display-stage candidates can be tuned
// and compared without one's setting affecting the other.
EXTERN void renderer_set_crt_lottes_debug_upscale(int upscale);
EXTERN int renderer_crt_lottes_debug_upscale(void);
// Sets the real on-screen pixel size the live CRT (Lottes) pass should
// render at (see gui.cpp's main_window_width/height) - a no-op if
// unchanged from last call, recreates renderer_crt_lottes_live_texture
// otherwise. Call every frame from wherever that size is computed; cheap
// when nothing changed.
EXTERN void renderer_set_crt_lottes_output_size(int width, int height);
// Attempts (once - cached after) to build the CRT (Lottes) GPU pipeline and
// reports whether it actually exists. False on a backend with no bytecode
// for this shader (see shaders/crt_pass/README.md's "No DXBC yet" note) -
// call this before deciding whether to draw
// renderer_crt_lottes_live_texture, since that texture gets created either
// way but is only ever written to if this returns true. Cheap to call every
// frame once cached.
EXTERN bool renderer_ensure_crt_lottes_pipeline(void);
// Same pair of calls as the CRT (Lottes) ones above, for Advanced Scaling.
EXTERN void renderer_set_advanced_scaling_output_size(int width, int height);
EXTERN bool renderer_ensure_advanced_scaling_pipeline(void);
// Surround an emulator image with these calls so the UI font atlas retains
// ImGui's linear sampler while the emulated pixels follow Video/Bilinear.
EXTERN void renderer_begin_emulator_image(void);
EXTERN void renderer_end_emulator_image(void);

#undef RENDERER_IMPORT
#undef EXTERN
#endif	/* RENDERER_H */
