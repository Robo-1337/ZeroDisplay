#include "common.hlsli"

// HDR encoder input -> interleaved UV plane of P010 (BT.2020 non-constant luminance, PQ, 10-bit limited range), half
// resolution; the bilinear sampler averages each 2x2 block (in linear light).
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

cbuffer EncCB : register(b2) {
    float4 enc;   // white nits, input is an SDR (sRGB) picture
};

float2 main(VSOut i) : SV_Target
{
    float3 pq = EncodePq2020(tex.SampleLevel(smp, i.uv, 0).rgb, enc.x, enc.y > 0.5);
    float y = dot(pq, float3(0.2627, 0.6780, 0.0593));
    float2 c = float2((pq.b - y) / 1.8814, (pq.r - y) / 1.4746);
    return (512.0 + 896.0 * c) * 64.0 / 65535.0;
}
