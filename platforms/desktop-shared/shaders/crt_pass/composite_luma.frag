#version 450 core

// Pass 2 of 2: reads pass 1's band-limited chroma (already at dot
// resolution, one Pb/Pr pair per output dot) plus the raw index texture,
// reconstructs luma by subtracting the re-modulated chroma from the
// carried signal over a short window, and converts to RGB. Direct
// translation of the second half of DecodeFrame in bench_decode.cpp.
//
// Sampling ChromaTex with a linear filter is what stands in for the CPU
// version's per-sub-sample chroma array here: chroma is already smooth
// (chromaTaps=32 in pass 1 heavily band-limits it), so interpolating
// between adjacent dot-centre samples over the much narrower luma window
// (+-4 sub-samples, under one dot wide) is a good approximation of the
// same continuous curve the CPU version keeps explicitly.
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D IndexTex;
layout(set = 2, binding = 1) uniform sampler2D ChromaTex;
layout(set = 3, binding = 0) uniform UBO
{
    float compositeMode;
    float _pad0, _pad1, _pad2;
} ubo;

const float kPi = 3.14159265359;
const int kDotsPerLine = 342;
const int kSamplesPerDot = 8;
const int kPictureDots = 284;
const int kPictureLines = 294;
const float kSubcarrierCyclesPerLine = 283.7516;
const float kCyclesPerSample = kSubcarrierCyclesPerLine / float(kDotsPerLine * kSamplesPerDot);
// Luma keeps whatever subcarrier the chroma filter did not remove; this
// window is deliberately much narrower than chroma's - see blur.frag's
// own note on why a narrow luma filter and wide chroma filter is the
// physically correct split, not an arbitrary choice.
const int kLumaHalfTaps = 4;

const vec3 kPalette[16] = vec3[16](
    vec3(0.05,  0.00,  0.00), vec3(0.05,  0.00,  0.00),
    vec3(0.53, -0.40, -0.27), vec3(0.72, -0.25, -0.20),
    vec3(0.34, -0.07,  0.53), vec3(0.53, -0.04,  0.46),
    vec3(0.39,  0.41, -0.13), vec3(0.72, -0.53,  0.23),
    vec3(0.51,  0.51, -0.20), vec3(0.71,  0.39, -0.23),
    vec3(0.72,  0.09, -0.40), vec3(0.80,  0.10, -0.30),
    vec3(0.47, -0.34, -0.24), vec3(0.60,  0.26,  0.20),
    vec3(0.83,  0.00,  0.00), vec3(1.00,  0.00,  0.00)
);

float VSign(int row) { return (row % 2 == 0) ? 1.0 : -1.0; }

vec3 ColourAt(int row, int dot)
{
    int clampedDot = clamp(dot, 0, kPictureDots - 1);
    float u = texelFetch(IndexTex, ivec2(clampedDot, clamp(row, 0, kPictureLines - 1)), 0).r;
    int index = int(round(u * 15.0));
    return kPalette[index];
}

// Same Y/Pr/Pb -> RGB coefficients as to_rgb() in decode_crt.py / ToRGB()
// in bench_decode.cpp, fitted against the hardware-verified palette.
vec3 ToRGB(float y, float pr, float pb)
{
    return vec3(
        0.978 * y + 0.767 * pr + 0.025 * pb + 0.035,
        1.037 * y - 0.364 * pr - 0.109 * pb + 0.022,
        1.031 * y - 0.053 * pr + 0.951 * pb - 0.013);
}

void main()
{
    int dot = int(floor(vTexCoord.x * float(kPictureDots)));
    int row = int(floor(vTexCoord.y * float(kPictureLines)));
    float vSign = VSign(row);
    float centreSample = float(dot * kSamplesPerDot + kSamplesPerDot / 2);

    float lumaSum = 0.0;
    for (int k = -kLumaHalfTaps; k <= kLumaHalfTaps; k++)
    {
        float n = centreSample + float(k);
        int sampleDot = int(floor(n / float(kSamplesPerDot)));
        vec3 col = ColourAt(row, sampleDot);

        float cycles = float(row) * kSubcarrierCyclesPerLine + n * kCyclesPerSample;
        float phase = fract(cycles) * 2.0 * kPi;
        float c = cos(phase);
        float s = sin(phase);

        float sampleU = float(sampleDot) / float(kPictureDots);
        vec2 uv = texture(ChromaTex, vec2(sampleU, vTexCoord.y)).rg;

        float chroma = col.b * c + col.g * vSign * s;
        float carried = mix(chroma, col.r + chroma, ubo.compositeMode);
        float residual = carried - (uv.x * c + uv.y * vSign * s);
        // S-Video: luma never met the subcarrier, nothing to subtract -
        // matches DecodeFrame's separate luma_clean path exactly.
        lumaSum += mix(col.r, residual, ubo.compositeMode);
    }
    float luma = lumaSum / float(2 * kLumaHalfTaps + 1);

    vec2 uvHere = texture(ChromaTex, vTexCoord).rg;
    FragColor = vec4(ToRGB(luma, uvHere.y, uvHere.x), 1.0);
}
