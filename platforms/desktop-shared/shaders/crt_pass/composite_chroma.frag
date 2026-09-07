#version 450 core

// Pass 1 of 2 of the real composite/S-Video decode (see composite_luma.frag
// for pass 2). Direct GLSL translation of DecodeFrame's chroma stage in
// tools/crt/bench_decode.cpp - same math, verified there pixel-for-pixel
// against the Python reference. This pass owns everything that produces
// the band-limited chroma signal: demodulation and the PAL delay line.
//
// The CPU version walks one line at a time and keeps the previous line's
// demodulated chroma in a local array to average against (the PAL delay
// line). A fragment shader has no such loop - but it does not need one:
// each output pixel is free to sample its own row's neighbourhood AND the
// row above's from the same source texture directly, which is exactly
// the delay line, expressed as a texture fetch instead of carried state.
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D IndexTex;
layout(set = 3, binding = 0) uniform UBO
{
    float compositeMode;  // 1.0 = composite (luma+chroma share a wire), 0.0 = S-Video
    float _pad0, _pad1, _pad2;
} ubo;

const float kPi = 3.14159265359;
const int kDotsPerLine = 342;
const int kSamplesPerDot = 8;
const int kPictureDots = 284;
const int kPictureLines = 294;
// Fixed by the PAL standard itself, not by the VDP - see
// TMS9918RasterTiming.h / the ICrtSignalSink work this decode is built on.
const float kSubcarrierCyclesPerLine = 283.7516;
const float kCyclesPerSample = kSubcarrierCyclesPerLine / float(kDotsPerLine * kSamplesPerDot);
// Chroma is the narrow-band signal - a wide window here is what gives
// composite its characteristic colour bleed.
const int kChromaHalfTaps = 16;

// Datasheet Table 2-3 with the hand calibration applied - identical table
// to kPalette in bench_decode.cpp.
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

// PAL alternates the V axis every line - literally what the name means
// (Phase Alternating Line).
float VSign(int row) { return (row % 2 == 0) ? 1.0 : -1.0; }

vec3 ColourAt(int row, int dot)
{
    int clampedDot = clamp(dot, 0, kPictureDots - 1);
    float u = texelFetch(IndexTex, ivec2(clampedDot, clamp(row, 0, kPictureLines - 1)), 0).r;
    int index = int(round(u * 15.0));
    return kPalette[index];
}

// Demodulates one row's chroma, raw - not yet combined with the row above.
// Mirrors the inner two LowPass calls in DecodeFrame plus the "undo the
// line alternation" step, but evaluated only at this one dot instead of
// across the whole line, since a fragment shader invocation only needs
// its own output position.
vec2 DemodulateRaw(int row, int dot)
{
    float vSign = VSign(row);
    float centreSample = float(dot * kSamplesPerDot + kSamplesPerDot / 2);

    float usum = 0.0;
    float vsum = 0.0;
    for (int k = -kChromaHalfTaps; k <= kChromaHalfTaps; k++)
    {
        float n = centreSample + float(k);
        int sampleDot = int(floor(n / float(kSamplesPerDot)));
        vec3 col = ColourAt(row, sampleDot);

        // Reduce the trig argument to less than one cycle before scaling by
        // 2*pi - row*cyclesPerLine alone would already be a huge angle by
        // the bottom of the frame, and float32 trig loses real precision at
        // large arguments.
        float cycles = float(row) * kSubcarrierCyclesPerLine + n * kCyclesPerSample;
        float phase = fract(cycles) * 2.0 * kPi;
        float c = cos(phase);
        float s = sin(phase);

        float chroma = col.b * c + col.g * vSign * s;
        float carried = mix(chroma, col.r + chroma, ubo.compositeMode);

        usum += 2.0 * carried * c;
        vsum += 2.0 * carried * s;
    }
    float taps = float(2 * kChromaHalfTaps + 1);
    return vec2(usum / taps, (vsum / taps) * vSign);
}

void main()
{
    int dot = int(floor(vTexCoord.x * float(kPictureDots)));
    int row = int(floor(vTexCoord.y * float(kPictureLines)));

    vec2 uv = DemodulateRaw(row, dot);
    if (row > 0)
        uv = (uv + DemodulateRaw(row - 1, dot)) * 0.5;

    FragColor = vec4(uv, 0.0, 1.0);
}
