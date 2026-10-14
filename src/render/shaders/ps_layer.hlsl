#include "common.hlsli"

// The UI layer (premultiplied alpha), blended over the picture with "one, inverse source alpha".
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);   // point, 1:1

float4 main(VSOut i) : SV_Target
{
    return tex.SampleLevel(smp, i.uv, 0);
}
