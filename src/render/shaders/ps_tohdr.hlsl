#include "common.hlsli"

// HDR output (scRGB, 1.0 = 80 nits): the SDR scene (everything drawn as usual) at the screen's SDR brightness.
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);   // point, 1:1

cbuffer HdrOutCB : register(b1) {
    float4 outp;    // SDR white / 80, display peak nits, picture white nits, picture peak nits
    float4 uvRect;  // u0, v0, u1, v1 (HDR picture crop)
};

float4 main(VSOut i) : SV_Target
{
    return float4(SrgbToLinear(tex.SampleLevel(smp, i.uv, 0).rgb) * outp.x, 1.0);
}
