#include "common.hlsli"

// Sharp bicubic (Catmull-Rom) resampling in 9 bilinear fetches instead of 16 point fetches: the two
// middle taps of each axis are merged into one bilinear fetch at the weighted position. Used when the
// picture is drawn at a size other than its own, where plain bilinear looks soft.

Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);   // linear, clamp

cbuffer ScaleCB : register(b1) {
    float2 texSize;
    float2 scalePad;
};

float4 main(VSOut i) : SV_Target
{
    const float2 pos = i.uv * texSize;
    const float2 t1 = floor(pos - 0.5) + 0.5;
    const float2 f = pos - t1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 p0 = (t1 - 1.0) / texSize;
    const float2 p3 = (t1 + 2.0) / texSize;
    const float2 p12 = (t1 + w2 / w12) / texSize;

    float3 c = 0;
    c += tex.SampleLevel(smp, float2(p0.x, p0.y), 0).rgb * w0.x * w0.y;
    c += tex.SampleLevel(smp, float2(p12.x, p0.y), 0).rgb * w12.x * w0.y;
    c += tex.SampleLevel(smp, float2(p3.x, p0.y), 0).rgb * w3.x * w0.y;
    c += tex.SampleLevel(smp, float2(p0.x, p12.y), 0).rgb * w0.x * w12.y;
    c += tex.SampleLevel(smp, float2(p12.x, p12.y), 0).rgb * w12.x * w12.y;
    c += tex.SampleLevel(smp, float2(p3.x, p12.y), 0).rgb * w3.x * w12.y;
    c += tex.SampleLevel(smp, float2(p0.x, p3.y), 0).rgb * w0.x * w3.y;
    c += tex.SampleLevel(smp, float2(p12.x, p3.y), 0).rgb * w12.x * w3.y;
    c += tex.SampleLevel(smp, float2(p3.x, p3.y), 0).rgb * w3.x * w3.y;
    return float4(saturate(c), 1.0);
}
