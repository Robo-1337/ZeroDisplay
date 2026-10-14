#include "common.hlsli"

Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

float4 main(VSOut i) : SV_Target
{
    return float4(tex.SampleLevel(smp, i.uv, 0).rgb, 1.0);
}
