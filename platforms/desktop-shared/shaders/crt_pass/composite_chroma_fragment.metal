#include <metal_stdlib>
using namespace metal;

// Hand-written MSL twin of composite_chroma.frag - see blur_vertex.metal
// for why entry points across this codebase's shaders are all "main0",
// and composite_chroma.frag itself for what this pass does and why.

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
constant int kChromaHalfTaps = 16;

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

inline float2 DemodulateRaw(texture2d<float> indexTex, int row, int dot, float compositeMode)
{
    float vSign = VSign(row);
    float centreSample = float(dot * kSamplesPerDot + kSamplesPerDot / 2);

    float usum = 0.0;
    float vsum = 0.0;
    for (int k = -kChromaHalfTaps; k <= kChromaHalfTaps; k++)
    {
        float n = centreSample + float(k);
        int sampleDot = int(floor(n / float(kSamplesPerDot)));
        float3 col = ColourAt(indexTex, row, sampleDot);

        float cycles = float(row) * kSubcarrierCyclesPerLine + n * kCyclesPerSample;
        float phase = fract(cycles) * 2.0 * kPi;
        float c = cos(phase);
        float s = sin(phase);

        float chroma = col.b * c + col.g * vSign * s;
        float carried = mix(chroma, col.r + chroma, compositeMode);

        usum += 2.0 * carried * c;
        vsum += 2.0 * carried * s;
    }
    float taps = float(2 * kChromaHalfTaps + 1);
    return float2(usum / taps, (vsum / taps) * vSign);
}

fragment float4 main0(VertexOut in [[stage_in]],
                       texture2d<float> indexTex [[texture(0)]],
                       sampler indexSampler [[sampler(0)]],
                       constant Uniforms& ubo [[buffer(0)]])
{
    int dot = int(floor(in.texCoord.x * float(kPictureDots)));
    int row = int(floor(in.texCoord.y * float(kPictureLines)));

    float2 uv = DemodulateRaw(indexTex, row, dot, ubo.compositeMode);
    if (row > 0)
        uv = (uv + DemodulateRaw(indexTex, row - 1, dot, ubo.compositeMode)) * 0.5;

    return float4(uv, 0.0, 1.0);
}
