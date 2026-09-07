#include <metal_stdlib>
using namespace metal;

// Hand-written MSL twin of crt_consumer.frag - see that file for what
// this pass does, what was left out of the original crt_consumer.glsl
// port, and why the uniform struct has exactly two padding floats.

struct VertexOut
{
    float4 position [[position]];
    float2 texCoord;
};

struct Uniforms
{
    float4 SourceSize;
    float4 OutputSize;
    float4 BackgroundColor;
    float blurx, blury;
    float scanlow, scanhigh;
    float beamlow, beamhigh;
    float preserve;
    float brightboost1, brightboost2;
    float gammaOut;
    float _pad0, _pad1;
};

constant float kMaskDark = 0.2;
constant float kMaskSize = 1.0;

inline bool OutsideSource(float2 pos)
{
    return pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0;
}

inline float3 SampleSource(texture2d<float> source, sampler s, float2 pos, constant Uniforms& ubo)
{
    if (OutsideSource(pos))
        return ubo.BackgroundColor.rgb;
    return source.sample(s, pos).rgb;
}

inline float ScanlineWeight(float y, float luminance, constant Uniforms& ubo)
{
    float beam = mix(ubo.scanlow, ubo.scanhigh, y);
    float scan = mix(ubo.beamlow, ubo.beamhigh, luminance);
    float ex = y * scan;
    return exp2(-beam * ex * ex);
}

inline float3 DefaultMask(float2 pos, float luminance, constant Uniforms& ubo)
{
    pos = floor(pos / kMaskSize);
    float phase = fract(pos.x * 0.4999);
    float3 mask = phase < 0.4999 ? float3(1.0, kMaskDark, 1.0) : float3(kMaskDark, 1.0, kMaskDark);
    return mix(mask, float3(1.0), luminance * ubo.preserve);
}

fragment float4 main0(VertexOut in [[stage_in]],
                       texture2d<float> source [[texture(0)]],
                       sampler sourceSampler [[sampler(0)]],
                       constant Uniforms& ubo [[buffer(0)]])
{
    float2 pos = in.texCoord;
    float2 texSize = ubo.SourceSize.xy;
    float2 pixelPhase = fract(pos * texSize);

    float2 texel = pos * texSize;
    float2 texelFloored = floor(texel);
    float2 coords = (texelFloored + pixelPhase) / texSize;

    float3 sample1 = SampleSource(source, sourceSampler,
        float2(coords.x + ubo.blurx * ubo.SourceSize.z, coords.y - ubo.blury * ubo.SourceSize.w), ubo);
    float3 sample2 = SampleSource(source, sourceSampler, coords, ubo);
    float3 sample3 = SampleSource(source, sourceSampler,
        float2(coords.x - ubo.blurx * ubo.SourceSize.z, coords.y + ubo.blury * ubo.SourceSize.w), ubo);

    float3 color = float3(
        sample1.r * 0.5 + sample2.r * 0.5,
        sample1.g * 0.25 + sample2.g * 0.5 + sample3.g * 0.25,
        sample2.b * 0.5 + sample3.b * 0.5);

    color = 2.0 * pow(color, float3(2.8)) - pow(color, float3(3.6));

    float luminance = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;
    float scanPos = fract(pixelPhase.y - 0.5);
    color = color * ScanlineWeight(scanPos, luminance, ubo) + color * ScanlineWeight(1.0 - scanPos, luminance, ubo);

    float maskedLuminance = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;
    color *= DefaultMask(in.texCoord * ubo.OutputSize.xy, maskedLuminance, ubo);
    color *= mix(ubo.brightboost1, ubo.brightboost2, max(max(color.r, color.g), color.b));
    color = pow(max(color, float3(0.0)), float3(1.0 / ubo.gammaOut));

    return float4(color, 1.0);
}
