#version 450 core
// Video > Postprocessing > Advanced Scaling - a sharper alternative to
// plain 2D Bilinear, not a horizontal-only fix (that was this pass'
// original, narrower brief - see the git history on this file's earlier
// name, hbilinear.frag, if that context is ever needed).
//
// The resampling kernel is crt_lottes.frag's own horizontal 4-tap
// Gaussian (CrtsFilter's "sharpen") applied on both axes as a symmetric
// 4x4 tap grid, instead of crt_lottes.frag's own vertical treatment
// (a 2-row cosine-window blend, built specifically to shape scanlines,
// not to resample generally). No phosphor mask, no auto-exposure tonemap,
// no curvature - this pass is scaling quality only.
//
// On top: "bold dark edges" - pixels near a dark (near-black) neighbor get
// pulled further toward black, making black lines/text strokes look
// thicker. First version of this used a global black-level crush +
// contrast stretch instead - measured to be the wrong tool: a global
// levels curve touches every color in the image (the green background
// included), when the actual ask was "only the black lines should look
// bolder, leave the other colors alone".
//
// Second version reused the same 4x4 neighborhood already sampled for the
// blur (radius in SOURCE texels) to find the darkest nearby sample - also
// measured wrong, confirmed with a screenshot: a source-texel radius covers
// far more real screen pixels than intended once the picture is scaled up,
// so the effect darkened whole character cells as flat boxes instead of
// tracing the actual glyph edges. This version searches a small, fixed
// radius in real OUTPUT pixels instead (see OutputSize below) - the search
// area stays visually the same few-screen-pixels size regardless of scale,
// so only pixels genuinely close to a dark edge on screen get pulled
// toward black.

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D Source;

layout(set = 3, binding = 0) uniform UBO
{
    vec4 SourceSize; // w, h, 1/w, 1/h
    vec4 OutputSize; // w, h, 1/w, 1/h - the real on-screen render target
    // sharpness: positive UI-facing magnitude, negated below for the
    // exp2() falloff exponent (same convention as crt_lottes.frag's own
    // Sharpness). blackThreshold/boldness/edgeRadius: see the "bold dark
    // edges" block in main() - boldness controls how strong the darkening
    // is, edgeRadius controls how far (in real output pixels) it reaches -
    // two independent knobs, not one.
    vec4 Params; // sharpness, blackThreshold, boldness, edgeRadius
} ubo;

// Fixed at the standard sRGB decode exponent - not exposed as a parameter,
// this pass has no tunables. Blending in linear light (decode, blur,
// re-encode) avoids the muddy/dark halos gamma-space blending produces at
// hard edges.
const float kGamma = 2.4;

float FromSrgb1(float c)
{
    return (c <= 0.04045) ? c * (1.0 / 12.92)
                           : pow(c * (1.0 / 1.055) + (0.055 / 1.055), kGamma);
}

vec3 FromSrgb(vec3 c)
{
    return vec3(FromSrgb1(c.r), FromSrgb1(c.g), FromSrgb1(c.b));
}

float ToSrgb1(float c)
{
    return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
}

vec3 ToSrgb(vec3 c)
{
    return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

vec3 Fetch(vec2 uv)
{
    return FromSrgb(texture(Source, uv).rgb);
}

void main()
{
    vec2 pos = vTexCoord * ubo.SourceSize.xy;
    vec2 base = floor(pos - 1.5) + 0.5;

    float blur = -ubo.Params.x;

    vec3 color = vec3(0.0);
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
            vec2 uv = vec2((base.x + float(i)) * ubo.SourceSize.z,
                            (base.y + float(j)) * ubo.SourceSize.w);
            color += Fetch(uv) * w;
            totalWeight += w;
        }
    }
    color /= totalWeight;

    // Bold dark edges - see this file's own header comment on why this
    // searches a small 3x3 neighborhood in real OUTPUT pixels (scaled by
    // edgeRadius) instead of reusing the blur's own source-texel grid.
    // smoothstep(0, blackThreshold, minLuma) is 0 when the darkest neighbor
    // is already at/below pure black and 1 once no neighbor is darker than
    // blackThreshold - so (1 - that) is 1 right next to a dark edge and
    // fades to 0 away from one, scaled by how strong the effect should be
    // (boldness).
    float blackThreshold = ubo.Params.y;
    float boldness = ubo.Params.z;
    float edgeRadius = ubo.Params.w;
    float minLuma = 1.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            vec2 uv = vTexCoord + vec2(float(dx), float(dy)) * edgeRadius * ubo.OutputSize.zw;
            minLuma = min(minLuma, dot(Fetch(uv), vec3(0.299, 0.587, 0.114)));
        }
    }
    float boldFactor = boldness * (1.0 - smoothstep(0.0, blackThreshold, minLuma));
    color = mix(color, vec3(0.0), boldFactor);

    FragColor = vec4(ToSrgb(color), 1.0);
}
