#include "common.hlsli"

// Encoder input: RGB -> interleaved UV plane of NV12 (BT.709, limited range), half resolution. Each
// output pixel samples the centre of its 2x2 source block, so the bilinear sampler averages the four.
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

float2 main(VSOut i) : SV_Target
{
    float3 rgb = tex.SampleLevel(smp, i.uv, 0).rgb;
    float u = dot(rgb, float3(-0.1146, -0.3854, 0.5));
    float v = dot(rgb, float3(0.5, -0.4542, -0.0458));
    return 128.0 / 255.0 + (224.0 / 255.0) * float2(u, v);
}
