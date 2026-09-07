#include <metal_stdlib>
using namespace metal;

// Hand-written MSL twin of blur.frag - see blur_vertex.metal for why this
// is a separate file, and blur.frag itself for why the radius is runtime-
// tunable but horizontal-only (chroma bleed travels along the scanline).

struct VertexOut
{
    float4 position [[position]];
    float2 texCoord;
};

struct BlurUniforms
{
    float2 texelSize;
    float radiusDots;  // in dots, see blur.frag
    float _pad;
};

constant int kMaxRadius = 12;

// Gaussian weighting over a fixed sample window, not a box blur - see
// blur.frag for why: it is what makes the radius slider continuous
// instead of only visibly changing on whole-dot jumps.
fragment float4 main0(VertexOut in [[stage_in]],
                       texture2d<float> source [[texture(0)]],
                       sampler sourceSampler [[sampler(0)]],
                       constant BlurUniforms& ubo [[buffer(0)]])
{
    if (ubo.radiusDots <= 0.001)
        return source.sample(sourceSampler, in.texCoord);

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (int i = -kMaxRadius; i <= kMaxRadius; i++)
    {
        float offset = float(i);
        float w = exp(-(offset * offset) / (2.0 * ubo.radiusDots * ubo.radiusDots));
        sum += source.sample(sourceSampler,
            in.texCoord + float2(offset * ubo.texelSize.x, 0.0)).rgb * w;
        weightSum += w;
    }
    return float4(sum / weightSum, 1.0);
}
