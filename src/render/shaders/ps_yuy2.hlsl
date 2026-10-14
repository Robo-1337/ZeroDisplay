#include "common.hlsli"

// Packed 4:2:2 viewed as RGBA8 at half width: YUY2 = (Y0 U Y1 V), UYVY = (U Y0 V Y1).
Texture2D<float4> tex : register(t0);

ConvOut main(VSOut i)
{
    int2 p = SourcePixel(i.pos.xy);
    int mx = p.x >> 1;
    bool uyvy = (flags & 1) != 0;
    float4 t = tex.Load(int3(mx, p.y, 0));
    float y0 = uyvy ? t.g : t.r;
    float y1 = uyvy ? t.a : t.b;
    float u = uyvy ? t.r : t.g;
    float v = uyvy ? t.b : t.a;
    float y = (p.x & 1) ? y1 : y0;
    if (p.x & 1) {
        // odd pixels sit between two chroma samples: average with the next macropixel
        int nx = min(mx + 1, int(size.x / 2) - 1);
        float4 t2 = tex.Load(int3(nx, p.y, 0));
        u = 0.5 * (u + (uyvy ? t2.r : t2.g));
        v = 0.5 * (v + (uyvy ? t2.b : t2.a));
    }
    return FinishColor(YuvToRgb(y, u, v));
}
