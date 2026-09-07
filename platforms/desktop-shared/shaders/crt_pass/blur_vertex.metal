#include <metal_stdlib>
using namespace metal;

// Hand-written MSL twin of blur.vert, not run through SDL_shadercross - the
// two are kept in step by hand, fine for a pair this small. Metal requires
// vertex and fragment entry points in separate compilation units even when
// both are named "main0" (the convention this codebase already uses for
// SPIRV-Cross output, see imgui_impl_sdlgpu3_shaders.h) - that is the only
// reason this is a second file instead of one.

struct VertexOut
{
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut main0(uint vertexID [[vertex_id]])
{
    float2 uv = float2(float((vertexID << 1) & 2), float(vertexID & 2));
    VertexOut out;
    // See blur.vert for why V is flipped here - same clip-space-vs-texture-
    // row-zero mismatch, same fix, kept in step by hand between the two.
    out.texCoord = float2(uv.x, 1.0 - uv.y);
    out.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return out;
}
