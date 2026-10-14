#include "common.hlsli"

// Screen filters for retro consoles (display only).
//   1 Sharp pixels: crisp pixel edges at any (non-integer) scale ("sharp bilinear").
//   2 Scanlines:    dark gaps between the source's lines; brighter lines bloom wider.
//   3 CRT:          scanlines + aperture-grille phosphor mask + a little glow.
//   4 LCD grid:     thin gaps between pixels, like a handheld's screen.
// `virt` is the picture's original resolution (e.g. 240 lines for a console the source upscaled).

Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);   // linear

cbuffer FilterCB : register(b1) {
    float2 srcSize;    // texture size in pixels
    float2 dstSize;    // size the whole texture is drawn at, in output pixels
    float2 virt;       // original picture resolution
    uint mode;
    float strength;    // 0..1
};

float3 SharpSample(float2 uv, float2 size, float2 scale)
{
    float2 texel = uv * size;
    float2 base = floor(texel);
    float2 d = frac(texel) - 0.5;
    float2 range = 0.5 - 0.5 / max(scale, 1.0);
    float2 f = (d - clamp(d, -range, range)) * max(scale, 1.0) + 0.5;
    return tex.SampleLevel(smp, (base + f) / size, 0).rgb;
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float Scanline(float2 uv, float3 c, float lines, float lineScale)
{
    float y = uv.y * lines;
    float d = frac(y) - 0.5;
    float beam = lerp(16.0, 5.0, saturate(Luma(c)));   // bright lines are wider
    float w = exp(-d * d * beam);
    // Fewer than ~2.5 output pixels per line can't show a clean gap: fade the effect out.
    float s = strength * saturate((lineScale - 1.5) / 1.0);
    return lerp(1.0, w * 1.35, s);
}

float4 main(VSOut i) : SV_Target
{
    float2 scale = dstSize / srcSize;
    float2 virtScale = dstSize / virt;
    float3 c;
    if (mode == 1) {
        c = SharpSample(i.uv, srcSize, scale);
    } else if (mode == 2) {
        c = SharpSample(i.uv, float2(srcSize.x, virt.y), float2(scale.x, virtScale.y));
        c *= Scanline(i.uv, c, virt.y, virtScale.y);
    } else if (mode == 3) {
        c = SharpSample(i.uv, float2(srcSize.x, virt.y), float2(scale.x, virtScale.y));
        float3 glow = tex.SampleLevel(smp, i.uv + float2(1.5, 0) / dstSize, 0).rgb + tex.SampleLevel(smp, i.uv - float2(1.5, 0) / dstSize, 0).rgb;
        c = lerp(c, max(c, glow * 0.5), 0.35 * strength);
        c *= Scanline(i.uv, c, virt.y, virtScale.y);
        // Aperture grille: R, G, B stripes across output pixels.
        uint px = uint(i.pos.x) % 3;
        float3 mask = px == 0 ? float3(1.0, 0.72, 0.72) : (px == 1 ? float3(0.72, 1.0, 0.72) : float3(0.72, 0.72, 1.0));
        c *= lerp(1.0, mask * 1.25, strength * saturate((scale.x - 1.0)));
    } else if (mode == 4) {
        c = SharpSample(i.uv, virt, virtScale);
        float2 cell = frac(i.uv * virt);
        float2 edge = min(cell, 1.0 - cell) * virtScale;   // distance to the cell edge in output pixels
        float g = saturate(min(edge.x, edge.y) / 0.9);
        float s = strength * saturate((min(virtScale.x, virtScale.y) - 2.0) / 1.5);
        c *= lerp(1.0, lerp(0.55, 1.08, g), s);
    } else {
        c = tex.SampleLevel(smp, i.uv, 0).rgb;
    }
    return float4(saturate(c), 1.0);
}
