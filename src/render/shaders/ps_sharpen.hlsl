#include "common.hlsli"

// Enhance, step 2: contrast-adaptive sharpening (our own), drawn 1:1. Unsharp masking on luma whose gain
// depends on the local contrast: soft areas (little contrast) are sharpened most, already strong edges least,
// and the result stays (nearly) inside the range of the pixel and its four neighbours, so there are no halos.

Texture2D<float4> tex : register(t0);

cbuffer SharpenCB : register(b1) {
    float2 origin;     // source pixel shown at the viewport's top-left
    float2 viewSize;   // viewport size in pixels
    float2 lo, hi;     // neighbours are clamped to this source rectangle
    float strength;    // 0..1
    float3 sharpenPad;
};

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 main(VSOut i) : SV_Target
{
    const int2 a = int2(lo), z = int2(hi);
    const int2 p = clamp(int2(origin) + int2(i.uv * viewSize), a, z);
    const float3 c = tex.Load(int3(p, 0)).rgb;
    const float3 n = tex.Load(int3(clamp(p + int2(0, -1), a, z), 0)).rgb;
    const float3 s = tex.Load(int3(clamp(p + int2(0, 1), a, z), 0)).rgb;
    const float3 w = tex.Load(int3(clamp(p + int2(-1, 0), a, z), 0)).rgb;
    const float3 e = tex.Load(int3(clamp(p + int2(1, 0), a, z), 0)).rgb;

    const float3 mn = min(c, min(min(n, s), min(w, e)));
    const float3 mx = max(c, max(max(n, s), max(w, e)));
    const float lc = Luma(c);
    const float detail = lc - Luma(n + s + w + e) * 0.25;
    const float contrast = Luma(mx) - Luma(mn);
    // Gain: up to 2.5x the detail in flat-ish areas, fading out on strong edges.
    const float gain = strength * 2.5 * (1.0 - smoothstep(0.08, 0.5, contrast));
    const float3 col = c + gain * detail;
    // Stay inside the neighbourhood's range (widened a little at high strength): no halos.
    const float3 slack = (mx - mn) * (0.2 * strength);
    return float4(saturate(clamp(col, mn - slack, mx + slack)), 1.0);
}
