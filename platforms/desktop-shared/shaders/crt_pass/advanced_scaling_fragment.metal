#include <metal_stdlib>
using namespace metal;

// Hand-written MSL twin of advanced_scaling.frag - see that file for what
// this pass does and why.

struct VertexOut
{
    float4 position [[position]];
    float2 texCoord;
};

struct Uniforms
{
    float4 SourceSize;
    float4 OutputSize;
    float4 Params; // sharpness, blackThreshold, boldness, edgeRadius
};

constant float kGamma = 2.4;

inline float FromSrgb1(float c)
{
    return (c <= 0.04045) ? c * (1.0 / 12.92)
                           : pow(c * (1.0 / 1.055) + (0.055 / 1.055), kGamma);
}

inline float3 FromSrgb(float3 c)
{
    return float3(FromSrgb1(c.r), FromSrgb1(c.g), FromSrgb1(c.b));
}

inline float ToSrgb1(float c)
{
    return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
}

inline float3 ToSrgb(float3 c)
{
    return float3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

inline float3 Fetch(texture2d<float> source, sampler s, float2 uv)
{
    return FromSrgb(source.sample(s, uv).rgb);
}

fragment float4 main0(VertexOut in [[stage_in]],
                       texture2d<float> source [[texture(0)]],
                       sampler sourceSampler [[sampler(0)]],
                       constant Uniforms& ubo [[buffer(0)]])
{
    float2 pos = in.texCoord * ubo.SourceSize.xy;
    float2 base = floor(pos - 1.5) + 0.5;

    float blur = -ubo.Params.x;

    float3 color = float3(0.0);
    float totalWeight = 0.0;
    for (int j = 0; j < 4; ++j)
    {
        float oy = pos.y - (base.y + float(j));
        float wy = exp2(blur * oy * oy);
        for (int i = 0; i < 4; ++i)
        {
            float ox = pos.x - (base.x + float(i));
            float wx = exp2(blur * ox * ox);
            float w = wx * wy;
            float2 uv = float2((base.x + float(i)) * ubo.SourceSize.z,
                                (base.y + float(j)) * ubo.SourceSize.w);
            color += Fetch(source, sourceSampler, uv) * w;
            totalWeight += w;
        }
    }
    color /= totalWeight;

    float blackThreshold = ubo.Params.y;
    float boldness = ubo.Params.z;
    float edgeRadius = ubo.Params.w;
    float minLuma = 1.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            float2 uv = in.texCoord + float2(float(dx), float(dy)) * edgeRadius * ubo.OutputSize.zw;
            minLuma = min(minLuma, dot(Fetch(source, sourceSampler, uv), float3(0.299, 0.587, 0.114)));
        }
    }
    float boldFactor = boldness * (1.0 - smoothstep(0.0, blackThreshold, minLuma));
    color = mix(color, float3(0.0), boldFactor);

    return float4(ToSrgb(color), 1.0);
}
