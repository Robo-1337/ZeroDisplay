#include "common.hlsli"

Texture2D<float4> tex : register(t0);

ConvOut main(VSOut i)
{
    return FinishColor(tex.Load(int3(SourcePixel(i.pos.xy), 0)).rgb);
}
