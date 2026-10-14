#include "common.hlsli"

// Scene transition: A = outgoing, B = incoming.
Texture2D<float4> texA : register(t0);
Texture2D<float4> texB : register(t1);
SamplerState smp : register(s0);

cbuffer TransitionCB : register(b1) {
    float4 params;   // x = progress 0..1, y = mode (0 fade, 1 slide, 2 wipe)
};

float4 main(VSOut i) : SV_Target
{
    float t = params.x;
    int mode = int(params.y + 0.5);
    if (mode == 1) {
        // B pushes A out to the left
        float2 ub = i.uv + float2(1.0 - t, 0.0);
        if (ub.x < 1.0) return float4(texB.SampleLevel(smp, ub, 0).rgb, 1.0);
        return float4(texA.SampleLevel(smp, i.uv - float2(t, 0.0), 0).rgb, 1.0);
    }
    float4 a = texA.SampleLevel(smp, i.uv, 0);
    float4 b = texB.SampleLevel(smp, i.uv, 0);
    if (mode == 2) {
        float edge = 1.0 - smoothstep(t * 1.1 - 0.05, t * 1.1, i.uv.x);
        return float4(lerp(a.rgb, b.rgb, edge), 1.0);
    }
    return float4(lerp(a.rgb, b.rgb, t), 1.0);
}
