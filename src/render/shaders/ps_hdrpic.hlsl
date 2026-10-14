#include "common.hlsli"

// HDR output: an HDR picture at its real brightness (linear light, 1.0 = its reference white -> nits -> scRGB), with
// a smooth roll-off above 80 % of the display's peak so brighter highlights don't clip hard.
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);   // linear

cbuffer HdrOutCB : register(b1) {
    float4 outp;    // SDR white / 80, display peak nits, picture white nits, picture peak nits
    float4 uvRect;  // u0, v0, u1, v1
};

float4 main(VSOut i) : SV_Target
{
    float2 uv = lerp(uvRect.xy, uvRect.zw, i.uv);
    float3 nits = max(tex.SampleLevel(smp, uv, 0).rgb, -0.5) * outp.z;   // slightly negative: colours outside BT.709
    float dmax = outp.y, ks = 0.8 * dmax;
    float m = max(nits.r, max(nits.g, nits.b));
    if (m > ks) {
        float range = dmax - ks;
        float xMax = max((outp.w - ks) / range, 1.001);
        float x = (m - ks) / range;
        float f = x * (1.0 + x / (xMax * xMax)) / (1.0 + x);
        nits *= (ks + range * min(f, 1.0)) / m;
    }
    return float4(nits / 80.0, 1.0);
}
