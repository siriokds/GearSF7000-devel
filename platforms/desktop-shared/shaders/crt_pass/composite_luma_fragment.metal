#include <metal_stdlib>
using namespace metal;

// Hand-written MSL twin of composite_luma.frag - see composite_chroma_
// fragment.metal for the entry-point naming note, and composite_luma.frag
// itself for what this pass does and why.

struct VertexOut
{
    float4 position [[position]];
    float2 texCoord;
};

struct Uniforms
{
    float compositeMode;
    float _pad0, _pad1, _pad2;
};

constant float kPi = 3.14159265359;
constant int kDotsPerLine = 342;
constant int kSamplesPerDot = 8;
constant int kPictureDots = 284;
constant int kPictureLines = 294;
constant float kSubcarrierCyclesPerLine = 283.7516;
constant float kCyclesPerSample = kSubcarrierCyclesPerLine / float(kDotsPerLine * kSamplesPerDot);
constant int kLumaHalfTaps = 4;

constant float3 kPalette[16] = {
    float3(0.05,  0.00,  0.00), float3(0.05,  0.00,  0.00),
    float3(0.53, -0.40, -0.27), float3(0.72, -0.25, -0.20),
    float3(0.34, -0.07,  0.53), float3(0.53, -0.04,  0.46),
    float3(0.39,  0.41, -0.13), float3(0.72, -0.53,  0.23),
    float3(0.51,  0.51, -0.20), float3(0.71,  0.39, -0.23),
    float3(0.72,  0.09, -0.40), float3(0.80,  0.10, -0.30),
    float3(0.47, -0.34, -0.24), float3(0.60,  0.26,  0.20),
    float3(0.83,  0.00,  0.00), float3(1.00,  0.00,  0.00)
};

inline float VSign(int row) { return (row % 2 == 0) ? 1.0 : -1.0; }

inline float3 ColourAt(texture2d<float> indexTex, int row, int dot)
{
    int clampedDot = clamp(dot, 0, kPictureDots - 1);
    int clampedRow = clamp(row, 0, kPictureLines - 1);
    float u = indexTex.read(uint2(clampedDot, clampedRow), 0).r;
    int index = int(round(u * 15.0));
    return kPalette[index];
}

inline float3 ToRGB(float y, float pr, float pb)
{
    return float3(
        0.978 * y + 0.767 * pr + 0.025 * pb + 0.035,
        1.037 * y - 0.364 * pr - 0.109 * pb + 0.022,
        1.031 * y - 0.053 * pr + 0.951 * pb - 0.013);
}

fragment float4 main0(VertexOut in [[stage_in]],
                       texture2d<float> indexTex [[texture(0)]],
                       texture2d<float> chromaTex [[texture(1)]],
                       sampler indexSampler [[sampler(0)]],
                       sampler chromaSampler [[sampler(1)]],
                       constant Uniforms& ubo [[buffer(0)]])
{
    int dot = int(floor(in.texCoord.x * float(kPictureDots)));
    int row = int(floor(in.texCoord.y * float(kPictureLines)));
    float vSign = VSign(row);
    float centreSample = float(dot * kSamplesPerDot + kSamplesPerDot / 2);

    float lumaSum = 0.0;
    for (int k = -kLumaHalfTaps; k <= kLumaHalfTaps; k++)
    {
        float n = centreSample + float(k);
        int sampleDot = int(floor(n / float(kSamplesPerDot)));
        float3 col = ColourAt(indexTex, row, sampleDot);

        float cycles = float(row) * kSubcarrierCyclesPerLine + n * kCyclesPerSample;
        float phase = fract(cycles) * 2.0 * kPi;
        float c = cos(phase);
        float s = sin(phase);

        float sampleU = float(sampleDot) / float(kPictureDots);
        float2 uv = chromaTex.sample(chromaSampler, float2(sampleU, in.texCoord.y)).rg;

        float chroma = col.b * c + col.g * vSign * s;
        float carried = mix(chroma, col.r + chroma, ubo.compositeMode);
        float residual = carried - (uv.x * c + uv.y * vSign * s);
        lumaSum += mix(col.r, residual, ubo.compositeMode);
    }
    float luma = lumaSum / float(2 * kLumaHalfTaps + 1);

    float2 uvHere = chromaTex.sample(chromaSampler, in.texCoord).rg;
    return float4(ToRGB(luma, uvHere.y, uvHere.x), 1.0);
}
