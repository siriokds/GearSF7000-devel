#version 450 core

// Proof-of-infrastructure pass, not a real composite decode: a horizontal
// Gaussian blur, radius (sigma) tunable continuously at runtime. The point
// of this shader is not the blur itself - it is proving the render-to-
// texture plumbing (a second SDL_GPU pipeline sampling the emulator
// texture and writing to a new target) works end to end, with a filter
// simple enough to eyeball correctness on sight before any real decode
// math goes in this slot.
//
// Horizontal only, deliberately: composite chroma bleed travels along the
// scanline (the subcarrier advances in time, i.e. across dots), not
// between lines, so a vertical component would be physically wrong here,
// not just unnecessary.
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SDL_GPU's fixed binding convention: fragment samplers start at set 2,
// fragment uniform buffers at set 3 (matches backends/sdlgpu3/shader.frag
// already in this codebase's ImGui backend).
layout(set = 2, binding = 0) uniform sampler2D Source;
layout(set = 3, binding = 0) uniform UBO
{
    vec2 texelSize;
    // In dots (TMS9918 raster dots, one texel each), not a fraction of
    // output width - the physical bleed distance is a fixed number of
    // dot-clock cycles, independent of how much of the raster is on
    // screen (256-dot cropped picture vs 342-dot Full Frame).
    float radiusDots;
    float _pad;
} ubo;

// Fixed sample window regardless of the runtime radius, so cost is
// identical every frame - only the Gaussian weighting below changes.
const int kMaxRadius = 12;

void main()
{
    // radiusDots is the Gaussian's sigma, in dots - continuous, not
    // rounded to a whole texel. A box blur (fixed integer tap count) can
    // only ever change strength in whole-dot jumps, which is what made the
    // slider feel dead between them; weighting a fixed sample window by a
    // continuously-widening Gaussian instead means every value the slider
    // can produce looks different, not just every integer it passes through.
    if (ubo.radiusDots <= 0.001)
    {
        FragColor = texture(Source, vTexCoord);
        return;
    }

    vec3 sum = vec3(0.0);
    float weightSum = 0.0;
    for (int i = -kMaxRadius; i <= kMaxRadius; i++)
    {
        float offset = float(i);
        float w = exp(-(offset * offset) / (2.0 * ubo.radiusDots * ubo.radiusDots));
        sum += texture(Source, vTexCoord + vec2(offset * ubo.texelSize.x, 0.0)).rgb * w;
        weightSum += w;
    }
    FragColor = vec4(sum / weightSum, 1.0);
}
