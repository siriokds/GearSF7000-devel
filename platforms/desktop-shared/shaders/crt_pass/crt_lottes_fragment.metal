#include <metal_stdlib>
using namespace metal;

// Hand-written MSL twin of crt_lottes.frag - see that file for what this
// pass does, what was left out of the original crt-lottes-fast.glsl port,
// and why the uniform struct is laid out the way it is.

struct VertexOut
{
    float4 position [[position]];
    float2 texCoord;
};

struct Uniforms
{
    float4 SourceSize;
    float4 OutputSize;
    float4 Mask;
    float4 GammaPad;
};

constant float kPi2 = 6.28318530717958;

inline float FromSrgb1(float c, float gamma)
{
    return (c <= 0.04045) ? c * (1.0 / 12.92)
                           : pow(c * (1.0 / 1.055) + (0.055 / 1.055), gamma);
}

inline float3 FromSrgb(float3 c, float gamma)
{
    return float3(FromSrgb1(c.r, gamma), FromSrgb1(c.g, gamma), FromSrgb1(c.b, gamma));
}

inline float ToSrgb1(float c)
{
    return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
}

inline float3 ToSrgb(float3 c)
{
    return float3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

inline float3 Fetch(texture2d<float> source, sampler s, float2 uv, float gamma)
{
    return FromSrgb(source.sample(s, uv).rgb, gamma);
}

inline float3 CrtsMask(float2 pos, float dark, float maskType)
{
    if (maskType == 2.0)
    {
        float3 m = float3(dark, dark, dark);
        float x = fract(pos.x * (1.0 / 3.0));
        if (x < (1.0 / 3.0)) m.r = 1.0;
        else if (x < (2.0 / 3.0)) m.g = 1.0;
        else m.b = 1.0;
        return m;
    }
    if (maskType == 1.0)
    {
        float3 m = float3(1.0, 1.0, 1.0);
        float x = fract(pos.x * (1.0 / 3.0));
        if (x < (1.0 / 3.0)) m.r = dark;
        else if (x < (2.0 / 3.0)) m.g = dark;
        else m.b = dark;
        return m;
    }
    if (maskType == 3.0)
    {
        pos.x += pos.y * 2.9999;
        float3 m = float3(dark, dark, dark);
        float x = fract(pos.x * (1.0 / 6.0));
        if (x < (1.0 / 3.0)) m.r = 1.0;
        else if (x < (2.0 / 3.0)) m.g = 1.0;
        else m.b = 1.0;
        return m;
    }
    return float3(1.0, 1.0, 1.0);
}

inline float4 CrtsTone(float thin, float mask, float maskType)
{
    if (maskType == 0.0) mask = 1.0;
    if (maskType == 1.0) mask = 0.5 + mask * 0.5;
    float midOut = 0.18 / ((1.5 - thin) * (0.5 * mask + 0.5));
    const float pMidIn = 0.18;
    float4 ret;
    ret.x = 1.0;
    ret.y = ((-pMidIn) + midOut) / ((1.0 - pMidIn) * midOut);
    ret.z = ((-pMidIn) * midOut + pMidIn) / (midOut * (-pMidIn) + midOut);
    ret.w = 1.0;
    return ret;
}

fragment float4 main0(VertexOut in [[stage_in]],
                       texture2d<float> source [[texture(0)]],
                       sampler sourceSampler [[sampler(0)]],
                       constant Uniforms& ubo [[buffer(0)]])
{
    float maskType = ubo.Mask.x;
    float maskIntensity = ubo.Mask.y;
    float scanlineThinness = ubo.Mask.z;
    float scanBlur = ubo.Mask.w;
    float gamma = ubo.GammaPad.x;

    float thin = 0.5 + 0.5 * scanlineThinness;
    float blur = -1.0 * scanBlur;
    float mask = 1.0 - maskIntensity;

    float2 ipos = in.texCoord * ubo.OutputSize.xy;
    float2 pos = ipos * (ubo.SourceSize.xy / ubo.OutputSize.xy);

    float y0 = floor(pos.y - 0.5) + 0.5;
    float x0 = floor(pos.x - 1.5) + 0.5;
    float2 p = float2(x0 * ubo.SourceSize.z, y0 * ubo.SourceSize.w);

    float3 colA0 = Fetch(source, sourceSampler, p, gamma); p.x += ubo.SourceSize.z;
    float3 colA1 = Fetch(source, sourceSampler, p, gamma); p.x += ubo.SourceSize.z;
    float3 colA2 = Fetch(source, sourceSampler, p, gamma); p.x += ubo.SourceSize.z;
    float3 colA3 = Fetch(source, sourceSampler, p, gamma);
    p.y += ubo.SourceSize.w;
    float3 colB3 = Fetch(source, sourceSampler, p, gamma); p.x -= ubo.SourceSize.z;
    float3 colB2 = Fetch(source, sourceSampler, p, gamma); p.x -= ubo.SourceSize.z;
    float3 colB1 = Fetch(source, sourceSampler, p, gamma); p.x -= ubo.SourceSize.z;
    float3 colB0 = Fetch(source, sourceSampler, p, gamma);

    float off = pos.y - y0;
    float scanA = cos(min(0.5, off * thin) * kPi2) * 0.5 + 0.5;
    float scanB = cos(min(0.5, thin * (1.0 - off)) * kPi2) * 0.5 + 0.5;

    float off0 = pos.x - x0;
    float off1 = off0 - 1.0;
    float off2 = off0 - 2.0;
    float off3 = off0 - 3.0;
    float pix0 = exp2(blur * off0 * off0);
    float pix1 = exp2(blur * off1 * off1);
    float pix2 = exp2(blur * off2 * off2);
    float pix3 = exp2(blur * off3 * off3);
    float pixT = 1.0 / (pix0 + pix1 + pix2 + pix3);
    scanA *= pixT;
    scanB *= pixT;

    float3 color =
        (colA0 * pix0 + colA1 * pix1 + colA2 * pix2 + colA3 * pix3) * scanA +
        (colB0 * pix0 + colB1 * pix1 + colB2 * pix2 + colB3 * pix3) * scanB;

    color *= CrtsMask(ipos, mask, maskType);

    float4 tone = CrtsTone(thin, mask, maskType);
    float peak = max(1.0 / (256.0 * 65536.0), max(color.r, max(color.g, color.b)));
    float3 ratio = color / peak;
    peak = peak / (peak * tone.y + tone.z);
    color = ratio * peak;

    float blackThreshold = ubo.GammaPad.y;
    float boldness = ubo.GammaPad.z;
    float edgeRadius = ubo.GammaPad.w;
    float minLuma = 1.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            float2 uv = in.texCoord + float2(float(dx), float(dy)) * edgeRadius * ubo.OutputSize.zw;
            minLuma = min(minLuma, dot(Fetch(source, sourceSampler, uv, gamma), float3(0.299, 0.587, 0.114)));
        }
    }
    float boldFactor = boldness * (1.0 - smoothstep(0.0, blackThreshold, minLuma));
    color = mix(color, float3(0.0), boldFactor);

    return float4(ToSrgb(color), 1.0);
}
