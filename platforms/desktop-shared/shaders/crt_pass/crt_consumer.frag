#version 450 core

// Display-stage test: a trimmed direct translation of GearSystem's
// crt_consumer.glsl (itself a port of libretro's crt-consumer, GPLv2+),
// run on top of our own Composite/S-Video/RGB signal-stage output. Not a
// full port - warp/corner-round/glow/noise/vignette/colour-temperature/
// contrast/interlace-flicker are all left out on purpose: real features
// of the original, but they add surface area without changing what this
// test is actually checking (blur + scanline + mask + tone), and
// curvature specifically was asked to be left aside for now.
//
// Unlike blur.frag/composite_*.frag, the constants here (scanlow,
// scanhigh, beamlow, beamhigh, preserve, brightboost, gamma) are not
// derived from a datasheet or a verified reference - they are a
// reasonable starting point to be tuned by eye against the actual
// picture, same as the original shader's own runtime parameters are
// meant to be tuned per display.
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D Source;
layout(set = 3, binding = 0) uniform UBO
{
    vec4 SourceSize;   // xy = size in texels, zw = 1/size
    vec4 OutputSize;   // xy = size in pixels, zw unused
    vec4 BackgroundColor;
    float blurx, blury;
    float scanlow, scanhigh;
    float beamlow, beamhigh;
    float preserve;
    float brightboost1, brightboost2;
    float gammaOut;
    // 10 scalars above; 2 pads bring the block to 96 bytes (48 from the
    // three vec4 fields + 40 + 8), a multiple of 16 - checked by hand,
    // not left to whichever side's automatic std140/MSL packing rules
    // happen to agree.
    float _pad0, _pad1;
} ubo;

const float kMaskDark = 0.2;
const float kMaskSize = 1.0;

bool OutsideSource(vec2 pos)
{
    return pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0;
}

vec3 SampleSource(vec2 pos)
{
    if (OutsideSource(pos))
        return ubo.BackgroundColor.rgb;
    return texture(Source, pos).rgb;
}

// Continuous falloff, not a hard on/off step - and the beam width itself
// depends on luminance (a brighter line "blooms" wider), which is why
// this takes luminance as an input at all.
float ScanlineWeight(float y, float luminance)
{
    float beam = mix(ubo.scanlow, ubo.scanhigh, y);
    float scan = mix(ubo.beamlow, ubo.beamhigh, luminance);
    float ex = y * scan;
    return exp2(-beam * ex * ex);
}

vec3 DefaultMask(vec2 pos, float luminance)
{
    pos = floor(pos / kMaskSize);
    float phase = fract(pos.x * 0.4999);
    vec3 mask = phase < 0.4999 ? vec3(1.0, kMaskDark, 1.0) : vec3(kMaskDark, 1.0, kMaskDark);
    return mix(mask, vec3(1.0), luminance * ubo.preserve);
}

void main()
{
    vec2 pos = vTexCoord;
    vec2 texSize = ubo.SourceSize.xy;
    vec2 pixelPhase = fract(pos * texSize);

    vec2 texel = pos * texSize;
    vec2 texelFloored = floor(texel);
    vec2 coords = (texelFloored + pixelPhase) / texSize;

    // Colour-channel split on the horizontal blur - a nod to real NTSC
    // chroma subsampling, not a symmetric RGB blur: R and B each lean on
    // one neighbour, G blends all three.
    vec3 sample1 = SampleSource(vec2(coords.x + ubo.blurx * ubo.SourceSize.z, coords.y - ubo.blury * ubo.SourceSize.w));
    vec3 sample2 = SampleSource(coords);
    vec3 sample3 = SampleSource(vec2(coords.x - ubo.blurx * ubo.SourceSize.z, coords.y + ubo.blury * ubo.SourceSize.w));

    vec3 color = vec3(
        sample1.r * 0.5 + sample2.r * 0.5,
        sample1.g * 0.25 + sample2.g * 0.5 + sample3.g * 0.25,
        sample2.b * 0.5 + sample3.b * 0.5);

    // Rough CRT gamma approximation, matching the original rather than
    // doing a formal sRGB<->linear round trip (which the original also
    // does not do here - that is a real gap, not something we removed).
    color = 2.0 * pow(color, vec3(2.8)) - pow(color, vec3(3.6));

    float luminance = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;
    float scanPos = fract(pixelPhase.y - 0.5);
    color = color * ScanlineWeight(scanPos, luminance) + color * ScanlineWeight(1.0 - scanPos, luminance);

    float maskedLuminance = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;
    color *= DefaultMask(vTexCoord * ubo.OutputSize.xy, maskedLuminance);
    color *= mix(ubo.brightboost1, ubo.brightboost2, max(max(color.r, color.g), color.b));
    color = pow(max(color, vec3(0.0)), vec3(1.0 / ubo.gammaOut));

    FragColor = vec4(color, 1.0);
}
