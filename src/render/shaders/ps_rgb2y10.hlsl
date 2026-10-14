#include "common.hlsli"

// HDR encoder input: linear light (1.0 = the picture's white; or an SDR picture, sRGB) -> Y plane of P010
// (BT.2020, PQ, 10-bit limited range in the top bits of 16). Drawn into an R16 view of the P010 texture.
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

cbuffer EncCB : register(b2) {
    float4 enc;   // white nits, input is an SDR (sRGB) picture
};

float main(VSOut i) : SV_Target
{
    float3 pq = EncodePq2020(tex.SampleLevel(smp, i.uv, 0).rgb, enc.x, enc.y > 0.5);
    float y = dot(pq, float3(0.2627, 0.6780, 0.0593));
    return (64.0 + 876.0 * y) * 64.0 / 65535.0;
}
