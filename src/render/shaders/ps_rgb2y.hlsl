#include "common.hlsli"

// Encoder input: RGB -> Y plane of NV12 (BT.709, limited range). Drawn into an R8 view of the NV12
// texture; the viewport is the picture area, so scaling comes from the bilinear sampler.
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

float main(VSOut i) : SV_Target
{
    float3 rgb = tex.SampleLevel(smp, i.uv, 0).rgb;
    return 16.0 / 255.0 + (219.0 / 255.0) * dot(rgb, float3(0.2126, 0.7152, 0.0722));
}
