#include "common.hlsli"

// HDR output: the UI layer (premultiplied, sRGB) at the screen's SDR brightness, blended "one, inverse source alpha".
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);   // point, 1:1

cbuffer HdrOutCB : register(b1) {
    float4 outp;    // SDR white / 80, ...
    float4 uvRect;
};

float4 main(VSOut i) : SV_Target
{
    float4 c = tex.SampleLevel(smp, i.uv, 0);
    if (c.a <= 0.0) return float4(0, 0, 0, 0);
    return float4(SrgbToLinear(saturate(c.rgb / c.a)) * c.a * outp.x, c.a);
}
