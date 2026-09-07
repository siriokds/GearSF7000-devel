# CRT pass shaders

Five independent shader sets live here, all reference source only -
GearSF7000's own build never touches these files or the tools below; only
regenerating the two headers they feed does, following the same pattern as
`backends/sdlgpu3/shader.vert`/`.frag` in the Dear ImGui repo (see its
`build_instructions.txt`). All of them share `blur.vert`/`blur_vertex.metal`
as their vertex stage - a generic full-screen triangle, nothing
pass-specific happens before the fragment shader runs.

**`blur.*`** (source for `../crt_pass_shaders.h`): the GPU test pass, a
tunable horizontal Gaussian blur proving the render-to-texture plumbing
works - not a real decode. See `debug_window_crt_test_pass` in
`gui_debug.cpp` (Debug > Video > Show CRT Test Pass).

**`composite_chroma.*` / `composite_luma.*`** (source for
`../composite_decode_shaders.h`): the real composite/S-Video decode, a
direct translation of `DecodeFrame` in `tools/crt/bench_decode.cpp` - two
passes, chroma demodulation + PAL delay line first, luma reconstruction
and the final RGB conversion second. Each file's own header comment
explains what that pass does and why; see also the `275ca9b` commit
message for the full picture. Runs on a captured GCRT file (`dump_crt_signal`
over MCP), not live gameplay - `debug_window_composite_decode_test` in
`gui_debug.cpp` (Debug > Video > Show CRT Composite Decode).

**`crt_consumer.*`** (source for `../crt_consumer_shaders.h`): display-stage
test, a trimmed direct translation of GearSystem's `crt_consumer.glsl`
(itself a port of libretro's crt-consumer, GPLv2+) - blur, luminance-aware
scanline weighting, aperture-grille mask, bright-boost tone compensation and
output gamma. Deliberately left out: curvature/warp, corner rounding, glow,
noise, vignette, color temperature and interlace flicker - see the file's
own header comment. Runs on `renderer_emu_texture` directly (live gameplay,
not a captured file), at native resolution rather than the real final draw
target - `OutputSize` in the uniform block equals `SourceSize`, so the mask
falls one stripe per source pixel column rather than per real screen pixel;
wiring this into an actual final-output pass (see `DOCS/manuals/crt-display/
output_pipeline_architecture.md`) will need to push the genuine window
resolution instead. Parameter defaults are not derived from any reference -
tune them by eye, same as the original shader is meant to be tuned per
display. See `debug_window_crt_consumer_test` in `gui_debug.cpp` (Debug >
Video > Show CRT Consumer Test). **Parked after live testing** - "sembra
più sfocato che altro" - kept as reference/comparison, not the chosen
direction; see `crt_lottes.*` below.

**`crt_lottes.*`** (source for `../crt_lottes_shaders.h`): display-stage
test, second candidate, a trimmed direct translation of Timothy Lottes'
`crt-lottes-fast.glsl` (public domain/Unlicense, RetroArch adaptation by
hunterk - see `tmp/crt-lottes-fast.glsl` in the repo root for the untouched
reference, not committed). Kept: the 4-tap horizontal Gaussian + cosine-
window vertical scanline filter (a different shape from crt_consumer's
Gaussian-in-luminance one), the phosphor mask (all four of the original's
mask types, selectable at runtime via `MaskType` - defaults to off/0 so the
pass starts scanline-only, per an explicit request to keep the mask
implemented but test it against disabled rather than drop it), the sRGB↔
linear round trip the filter math assumes, and the built-in auto-exposure
tonemap (`CrtsTone`) that compensates for the brightness scanlines and mask
remove - this is what `crt_consumer.frag` was missing (there, bright-boost
was manual and never tuned). Left out on request: `CRTS_WARP` (curvature -
the original bundles its vignette into the same branch, so vignette is gone
too, not a separate cut), `CORNER`/`TRINITRON_CURVE` (meaningless without
warp), `CRTS_CONTRAST`/`CRTS_SATURATION` (already off in the reference's
own defaults). Same native-resolution/debug-upscale situation as
`crt_consumer.frag` - see that entry above and `crt_lottes.frag`'s own
`OutputSize` comment. See `debug_window_crt_lottes_test` in `gui_debug.cpp`
(Debug > Video > Show CRT Lottes Test).

**Chosen after live testing**: "Questo vince a mani basse" - this is the
current pick for the display stage of the `Video → Output` pipeline
(`DOCS/manuals/crt-display/output_pipeline_architecture.md`), ahead of
`crt_consumer.glsl`. Later gained a "bold dark edges" pass (see
`advanced_scaling.*` below for the full story) - `GammaPad`'s three
otherwise-unused padding floats now carry `blackThreshold`/`boldness`/
`edgeRadius`, no uniform buffer growth.

**`advanced_scaling.*`** (source for `../advanced_scaling_shaders.h`):
`Video > Postprocessing > Advanced Scaling`, a sharper general-purpose XY
resampling filter than plain 2D Bilinear - not a display-stage CRT
candidate like `crt_consumer.*`/`crt_lottes.*` above, no mask/scanlines/
curvature at all. The resampling kernel is `crt_lottes.frag`'s own
horizontal 4-tap Gaussian ("sharpen") applied symmetrically on both axes
as a 4x4 tap grid, instead of `crt_lottes.frag`'s own vertical treatment (a
2-row cosine-window blend built specifically to shape scanlines, not to
resample generally). On top: "bold dark edges" - pixels near a dark
(near-black) neighbor get pulled toward black, making thin black lines/
text look less washed out by the blur. Went through two wrong designs
before this one, both measured wrong rather than assumed: a global
black-level crush + contrast stretch touched every color in the image, not
just dark lines; a neighborhood search sized in *source texels* (reusing
the same grid as the resampling blur) covered far more real screen pixels
than intended once scaled up, darkening whole character cells as flat
boxes instead of tracing actual glyph edges. This version searches a small
3x3 neighborhood sized in real *output* pixels instead (scaled by
`edgeRadius`), independent of source resolution or scale factor - see the
file's own header comment for the complete history. No debug window (no
embedded preview needed - the real game view already shows the live
result) - `window_advanced_scaling_setup` in `gui.cpp` (Video >
Postprocessing > Configure Advanced Scaling...).

Every `.frag`/`.vert` file here is GLSL, compiled straight to SPIRV - no
SDL_shadercross needed for that half. Every `_vertex.metal`/`_fragment.metal`
file is hand-written MSL, kept in step by hand instead of cross-compiled
from the SPIRV. Metal disallows a function named `main` (hence `main0`,
matching the SPIRV-Cross convention already used elsewhere in this
codebase) and requires vertex/fragment entry points in separate compilation
units, which is why each GLSL fragment shader has a matching `_fragment.metal`
file instead of vertex and fragment sharing one `.metal` file.

DXIL is generated rather than hand-written: SPIRV-Cross turns each SPIRV
blob into HLSL and `dxc` compiles that to DXIL, so it stays in step with the
GLSL automatically and only the MSL needs keeping in sync by hand. This was
left out originally for want of a Windows machine to verify it on; there is
one now, and every pass carries all three formats. Until then SDL picked
`direct3d12` on Windows, `create_crt_pass_shader` had nothing to hand it,
and every pass silently disabled itself - Advanced Scaling and CRT Lottes
were selectable in the menu and simply never ran.

No rebinding is needed on the way through. `SDL_CreateGPUShader` documents
fragment shaders as taking sampled textures at set 2 and uniform buffers at
set 3 in SPIRV, and `(t/s[n], space2)`/`(b[n], space3)` in DXIL; the `.frag`
files here already declare `set = 2`/`set = 3`, and SPIRV-Cross maps a
descriptor set straight onto the matching register space at shader model
5.1+. `blur.vert` declares no resources at all, so the vertex stage is a
plain translation.

## Regenerating

The vertex shader only needs building once (shared by every pass):

```
glslc -o blur.vert.spv -fshader-stage=vertex --target-env=vulkan1.1 blur.vert

export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xcrun -sdk macosx metal -o blur_vertex.air -c blur_vertex.metal
xcrun -sdk macosx metallib -o blur_vertex.metallib blur_vertex.air
```

Each fragment shader follows the same pattern - swap in the file name:

```
glslc -o blur.frag.spv -fshader-stage=fragment --target-env=vulkan1.1 blur.frag
xcrun -sdk macosx metal -o blur_fragment.air -c blur_fragment.metal
xcrun -sdk macosx metallib -o blur_fragment.metallib blur_fragment.air

glslc -o composite_chroma.frag.spv -fshader-stage=fragment --target-env=vulkan1.1 composite_chroma.frag
xcrun -sdk macosx metal -o composite_chroma_fragment.air -c composite_chroma_fragment.metal
xcrun -sdk macosx metallib -o composite_chroma_fragment.metallib composite_chroma_fragment.air

glslc -o composite_luma.frag.spv -fshader-stage=fragment --target-env=vulkan1.1 composite_luma.frag
xcrun -sdk macosx metal -o composite_luma_fragment.air -c composite_luma_fragment.metal
xcrun -sdk macosx metallib -o composite_luma_fragment.metallib composite_luma_fragment.air

glslc -o crt_consumer.frag.spv -fshader-stage=fragment --target-env=vulkan1.1 crt_consumer.frag
xcrun -sdk macosx metal -o crt_consumer_fragment.air -c crt_consumer_fragment.metal
xcrun -sdk macosx metallib -o crt_consumer_fragment.metallib crt_consumer_fragment.air

glslc -o crt_lottes.frag.spv -fshader-stage=fragment --target-env=vulkan1.1 crt_lottes.frag
xcrun -sdk macosx metal -o crt_lottes_fragment.air -c crt_lottes_fragment.metal
xcrun -sdk macosx metallib -o crt_lottes_fragment.metallib crt_lottes_fragment.air
```

The byte arrays must be comma-separated with no other whitespace inside
each line (see the emit helper any of these headers were generated with) -
`textwrap.fill` or similar word-wrapping on a string with no spaces in it
will happily split a multi-digit byte in half instead of wrapping at a
comma, producing a header that fails to compile with a cryptic "expected
'}'" pointing at the split line. Chunk by item count instead (e.g. 16 bytes
per line) and join with commas explicitly.

The DXIL half runs from the SPIRV rather than from the GLSL, so it needs no
separate authoring - on Windows, per shader:

```
spirv-cross --hlsl --shader-model 60 --output blur.vert.hlsl blur.vert.spv
dxc -T vs_6_0 -E main -Fo blur.vert.dxil blur.vert.hlsl
```

`ps_6_0` instead of `vs_6_0` for the fragment shaders; the entry point stays
`main` (SPIRV-Cross emits a `main` wrapper around `vert_main`/`frag_main`).
`dxc.exe` ships with the Windows SDK - `Windows Kits/10/bin/<version>/x64` -
and `dxil.dll` must sit beside it, which it does there. Without that DLL dxc
emits unsigned bytecode that D3D12 refuses to load. SPIRV-Cross has no
binary release; build it from source with CMake, which needs no dependencies
beyond a compiler. Cloning it on Windows fails to check out some `reference/`
and `shaders-msl/` test data whose filenames exceed MAX_PATH - harmless, the
sources it needs all land.

`glslc` comes from `brew install shaderc`. The Metal compiler needs a
component Xcode does not install by default even with the full app present
(only the command-line tools ship on this machine) - fetch it once with
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild
-downloadComponent MetalToolchain` (a real download, a few hundred MB).
`DEVELOPER_DIR` is set per-command above deliberately, not via
`xcode-select -s` - that would change the system-wide default toolchain
for every other project on the machine, not just this one.

Then turn the output files into their header's `uint8_t` arrays -
`blur.*` into `spirv_crt_blur_*`/`metallib_crt_blur_*` in
`../crt_pass_shaders.h`, `composite_chroma.*`/`composite_luma.*` into
`spirv_composite_*_fragment`/`metallib_composite_*_fragment` in
`../composite_decode_shaders.h`, `crt_consumer.*` into
`spirv_crt_consumer_fragment`/`metallib_crt_consumer_fragment` in
`../crt_consumer_shaders.h`, `crt_lottes.*` into
`spirv_crt_lottes_fragment`/`metallib_crt_lottes_fragment` in
`../crt_lottes_shaders.h` (none of the four duplicate the vertex
shader - every pipeline reuses `crt_pass_shaders.h`'s vertex arrays), and
each `.dxil` into a `dxil_`-prefixed array beside its `spirv_` counterpart
in the same header. Any bin-to-C-array tool works, e.g.
`misc/fonts/binary_to_compressed_c.cpp` in the Dear ImGui repo.
