#include "common.hlsli"

Texture2D<float> texY : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState linearClamp : register(s0);

ConvOut main(VSOut i)
{
    int2 p = SourcePixel(i.pos.xy);
    float y = texY.Load(int3(p, 0));
    float2 uv = texUV.SampleLevel(linearClamp, (float2(p) + 0.5) / float2(size), 0);
    return FinishColor(YuvToRgb(y, uv.x, uv.y));
}
