#version 450 core
// Trimmed direct translation of Timothy Lottes' crt-lottes-fast.glsl
// (public domain/Unlicense, RetroArch adaptation by hunterk - see
// tmp/crt-lottes-fast.glsl in the repo root for the untouched reference
// this was ported from, not committed here). Kept: the 4-tap horizontal
// Gaussian + cosine-window vertical scanline filter (CrtsFilter's non-WARP,
// non-2-TAP path), the phosphor mask (all four of the original's mask
// types, selectable at runtime via MaskType - test against MaskType=0 to
// compare with the mask off, as requested), the sRGB<->linear round trip
// the filter math assumes, and the built-in auto-exposure tonemap that
// compensates for the brightness the scanlines and mask remove (CRTS_TONE,
// contrast/saturation fixed at their no-op defaults since CRTS_CONTRAST/
// CRTS_SATURATION were already off in the reference itself).
//
// Left out on request: CRTS_WARP (screen curvature) - the original bundles
// its vignette into the same branch, so vignette is gone too, not a
// separate cut. CORNER and TRINITRON_CURVE are meaningless without warp,
// also dropped. No extra color grading was added beyond the round trip
// above - the project already has its own real signal/color pipeline from
// the composite/S-Video decode work (composite_chroma.frag/
// composite_luma.frag) if that rigor is ever needed here instead.
//
// Added later, ported over from advanced_scaling.frag once that pass'
// version of it was verified working there: "bold dark edges" - a small
// search in real OUTPUT pixels (not source texels - see
// advanced_scaling.frag's own header comment on why that distinction
// matters, a source-texel radius smears across whole character cells once
// scaled up) that pulls a pixel toward black if it's near a dark neighbor,
// making thin black lines/text look less washed out by the horizontal
// blur/vertical scanline blend above.

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D Source;

layout(set = 3, binding = 0) uniform UBO
{
    vec4 SourceSize;   // w, h, 1/w, 1/h
    // Real render-target resolution, not SourceSize - like crt_consumer.
    // frag, the vertical scanline term needs OutputSize.y > SourceSize.y
    // to have any row-to-row variation to compute at all (fract(pos.y)
    // otherwise lands on the same phase for every output row).
    vec4 OutputSize;   // w, h, 1/w, 1/h
    vec4 Mask;         // maskType (0-3), maskIntensity (0-1), scanlineThinness (0-1), scanBlur
    // crtGamma, then the three bold-dark-edges parameters (see main()) -
    // reusing what used to be unused padding, no uniform buffer growth.
    vec4 GammaPad;     // crtGamma, blackThreshold, boldness, edgeRadius
} ubo;

const float kPi2 = 6.28318530717958;

float FromSrgb1(float c, float gamma)
{
    return (c <= 0.04045) ? c * (1.0 / 12.92)
                           : pow(c * (1.0 / 1.055) + (0.055 / 1.055), gamma);
}

vec3 FromSrgb(vec3 c, float gamma)
{
    return vec3(FromSrgb1(c.r, gamma), FromSrgb1(c.g, gamma), FromSrgb1(c.b, gamma));
}

float ToSrgb1(float c)
{
    return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
}

vec3 ToSrgb(vec3 c)
{
    return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

vec3 Fetch(vec2 uv, float gamma)
{
    return FromSrgb(texture(Source, uv).rgb, gamma);
}

// Faithful translation of CrtsMask - 'pos' is real screen-pixel
// coordinates (fragCoord-equivalent), 'dark' is the masked channel's
// exposure (0 = fully off, 1 = no effect).
vec3 CrtsMask(vec2 pos, float dark, float maskType)
{
    if (maskType == 2.0)
    {
        vec3 m = vec3(dark, dark, dark);
        float x = fract(pos.x * (1.0 / 3.0));
        if (x < (1.0 / 3.0)) m.r = 1.0;
        else if (x < (2.0 / 3.0)) m.g = 1.0;
        else m.b = 1.0;
        return m;
    }
    if (maskType == 1.0)
    {
        vec3 m = vec3(1.0, 1.0, 1.0);
        float x = fract(pos.x * (1.0 / 3.0));
        if (x < (1.0 / 3.0)) m.r = dark;
        else if (x < (2.0 / 3.0)) m.g = dark;
        else m.b = dark;
        return m;
    }
    if (maskType == 3.0)
    {
        pos.x += pos.y * 2.9999;
        vec3 m = vec3(dark, dark, dark);
        float x = fract(pos.x * (1.0 / 6.0));
        if (x < (1.0 / 3.0)) m.r = 1.0;
        else if (x < (2.0 / 3.0)) m.g = 1.0;
        else m.b = 1.0;
        return m;
    }
    return vec3(1.0, 1.0, 1.0);
}

// Faithful translation of CrtsTone with contrast fixed at 1.0 (pow(x,1)=x,
// a no-op) and saturation fixed at 0.0 (CRTS_SATURATION was off in the
// reference, so ret.w is computed but never consumed here either).
vec4 CrtsTone(float thin, float mask, float maskType)
{
    if (maskType == 0.0) mask = 1.0;
    if (maskType == 1.0) mask = 0.5 + mask * 0.5;
    float midOut = 0.18 / ((1.5 - thin) * (0.5 * mask + 0.5));
    const float pMidIn = 0.18; // pow(0.18, contrast) with contrast == 1.0
    vec4 ret;
    ret.x = 1.0;
    ret.y = ((-pMidIn) + midOut) / ((1.0 - pMidIn) * midOut);
    ret.z = ((-pMidIn) * midOut + pMidIn) / (midOut * (-pMidIn) + midOut);
    ret.w = 1.0;
    return ret;
}

void main()
{
    float maskType = ubo.Mask.x;
    float maskIntensity = ubo.Mask.y;
    float scanlineThinness = ubo.Mask.z;
    float scanBlur = ubo.Mask.w;
    float gamma = ubo.GammaPad.x;

    // See crt-lottes-fast.glsl's own INPUT_THIN/INPUT_BLUR/INPUT_MASK
    // #defines for these same three remaps.
    float thin = 0.5 + 0.5 * scanlineThinness;
    float blur = -1.0 * scanBlur;
    float mask = 1.0 - maskIntensity;

    // ipos = real screen-pixel coordinates (fragCoord-equivalent); pos =
    // the same position in source-texel space. Algebraically pos reduces
    // to vTexCoord*SourceSize regardless of OutputSize (the OutputSize
    // factors cancel) - only the mask, which reads ipos directly, and the
    // implicit output-row density behind vTexCoord's own interpolation
    // depend on OutputSize actually being the real target resolution.
    vec2 ipos = vTexCoord * ubo.OutputSize.xy;
    vec2 pos = ipos * (ubo.SourceSize.xy / ubo.OutputSize.xy);

    float y0 = floor(pos.y - 0.5) + 0.5;
    float x0 = floor(pos.x - 1.5) + 0.5;
    vec2 p = vec2(x0 * ubo.SourceSize.z, y0 * ubo.SourceSize.w);

    vec3 colA0 = Fetch(p, gamma); p.x += ubo.SourceSize.z;
    vec3 colA1 = Fetch(p, gamma); p.x += ubo.SourceSize.z;
    vec3 colA2 = Fetch(p, gamma); p.x += ubo.SourceSize.z;
    vec3 colA3 = Fetch(p, gamma);
    p.y += ubo.SourceSize.w;
    vec3 colB3 = Fetch(p, gamma); p.x -= ubo.SourceSize.z;
    vec3 colB2 = Fetch(p, gamma); p.x -= ubo.SourceSize.z;
    vec3 colB1 = Fetch(p, gamma); p.x -= ubo.SourceSize.z;
    vec3 colB0 = Fetch(p, gamma);

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

    vec3 color =
        (colA0 * pix0 + colA1 * pix1 + colA2 * pix2 + colA3 * pix3) * scanA +
        (colB0 * pix0 + colB1 * pix1 + colB2 * pix2 + colB3 * pix3) * scanB;

    color *= CrtsMask(ipos, mask, maskType);

    vec4 tone = CrtsTone(thin, mask, maskType);
    float peak = max(1.0 / (256.0 * 65536.0), max(color.r, max(color.g, color.b)));
    vec3 ratio = color / peak;
    peak = peak / (peak * tone.y + tone.z);
    color = ratio * peak;

    // Bold dark edges - see this file's own header comment and
    // advanced_scaling.frag for the full story.
    float blackThreshold = ubo.GammaPad.y;
    float boldness = ubo.GammaPad.z;
    float edgeRadius = ubo.GammaPad.w;
    float minLuma = 1.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            vec2 uv = vTexCoord + vec2(float(dx), float(dy)) * edgeRadius * ubo.OutputSize.zw;
            minLuma = min(minLuma, dot(Fetch(uv, gamma), vec3(0.299, 0.587, 0.114)));
        }
    }
    float boldFactor = boldness * (1.0 - smoothstep(0.0, blackThreshold, minLuma));
    color = mix(color, vec3(0.0), boldFactor);

    FragColor = vec4(ToSrgb(color), 1.0);
}
