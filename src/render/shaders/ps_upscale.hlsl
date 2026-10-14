#include "common.hlsli"

// Enhance, step 2 when enlarging: edge-aware upscaling (our own). Around each output pixel it reads the 12 nearest
// source pixels (4x4 without the corners), estimates the local edge direction from luma gradients, and filters
// with a kernel that has a small negative lobe (like Catmull-Rom's), stretched along the edge and tightened across
// it: edges stay straight and crisp instead of stair-stepped or soft. The result is clamped to the 4 nearest
// pixels so the negative lobe can't ring. Half precision where it is enough; texels come in with 4-texel gathers.

Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);   // clamp addressing (gathers ignore the filter)

cbuffer ScaleCB : register(b1) {
    float2 texSize;
    float2 scalePad;
};

min16float Luma(min16float3 c) { return dot(c, min16float3(0.2126, 0.7152, 0.0722)); }

// Radial kernel on the squared distance x = r^2 (no square root): 1 at the centre, 0 at r = 1 and r = 2, a
// small negative lobe in between, which is what keeps edges crisp.
min16float Kernel(min16float x)
{
    const min16float a = saturate(min16float(1.0) - min16float(0.25) * x);
    const min16float w = a * a * (min16float(1.0) - x);
    return w < min16float(0.0) ? w * min16float(0.3) : w;
}

float4 main(VSOut i) : SV_Target
{
    const float2 pos = i.uv * texSize - 0.5;   // texel-centre coordinates
    const float2 cell = floor(pos);
    const float2 f = pos - cell;
    const float2 inv = 1.0 / texSize;
    // Four 2x2 gathers around the cell: A covers x -1..0, y -1..0; B x 1..2, y -1..0; C x -1..0, y 1..2; D x 1..2, y 1..2.
    // Gather order: w = (left, top), z = (right, top), x = (left, bottom), y = (right, bottom).
    const float2 ua = cell * inv, ub = (cell + float2(2, 0)) * inv, uc = (cell + float2(0, 2)) * inv, ud = (cell + 2.0) * inv;
    const min16float4 ra = min16float4(tex.GatherRed(smp, ua)), ga = min16float4(tex.GatherGreen(smp, ua)), ba = min16float4(tex.GatherBlue(smp, ua));
    const min16float4 rb = min16float4(tex.GatherRed(smp, ub)), gb = min16float4(tex.GatherGreen(smp, ub)), bb = min16float4(tex.GatherBlue(smp, ub));
    const min16float4 rc = min16float4(tex.GatherRed(smp, uc)), gc = min16float4(tex.GatherGreen(smp, uc)), bc = min16float4(tex.GatherBlue(smp, uc));
    const min16float4 rd = min16float4(tex.GatherRed(smp, ud)), gd = min16float4(tex.GatherGreen(smp, ud)), bd = min16float4(tex.GatherBlue(smp, ud));

    // Taps (x, y) relative to the cell: 0 (0,-1) 1 (1,-1) | 2 (-1,0) 3 (0,0) 4 (1,0) 5 (2,0) | 6 (-1,1) 7 (0,1) 8 (1,1) 9 (2,1) | 10 (0,2) 11 (1,2)
    min16float3 c[12];
    c[0] = min16float3(ra.z, ga.z, ba.z);
    c[1] = min16float3(rb.w, gb.w, bb.w);
    c[2] = min16float3(ra.x, ga.x, ba.x);
    c[3] = min16float3(ra.y, ga.y, ba.y);
    c[4] = min16float3(rb.x, gb.x, bb.x);
    c[5] = min16float3(rb.y, gb.y, bb.y);
    c[6] = min16float3(rc.w, gc.w, bc.w);
    c[7] = min16float3(rc.z, gc.z, bc.z);
    c[8] = min16float3(rd.w, gd.w, bd.w);
    c[9] = min16float3(rd.z, gd.z, bd.z);
    c[10] = min16float3(rc.y, gc.y, bc.y);
    c[11] = min16float3(rd.x, gd.x, bd.x);
    static const float2 offs[12] = {float2(0, -1), float2(1, -1), float2(-1, 0), float2(0, 0), float2(1, 0), float2(2, 0),
                                    float2(-1, 1), float2(0, 1), float2(1, 1), float2(2, 1), float2(0, 2), float2(1, 2)};
    min16float l[12];
    [unroll] for (int k = 0; k < 12; ++k) l[k] = Luma(c[k]);

    // Central-difference gradients at the centre quad (3, 4, 7, 8), blended bilinearly to the sample position.
    const min16float2 fh = min16float2(f);
    const min16float2 g00 = min16float2(l[4] - l[2], l[7] - l[0]);
    const min16float2 g10 = min16float2(l[5] - l[3], l[8] - l[1]);
    const min16float2 g01 = min16float2(l[8] - l[6], l[10] - l[3]);
    const min16float2 g11 = min16float2(l[9] - l[7], l[11] - l[4]);
    const float2 g = float2(lerp(lerp(g00, g10, fh.x), lerp(g01, g11, fh.x), fh.y));

    const float lo = min(min(l[3], l[4]), min(l[7], l[8]));
    const float hi = max(max(l[3], l[4]), max(l[7], l[8]));
    const float glen = length(g);
    // Edge confidence: a gradient that is large compared to the local range is a clean edge; noise and texture
    // stay close to the plain radial kernel.
    const float edge = saturate(glen / (2.0 * (hi - lo) + 0.02)) * saturate(glen * 8.0);
    const float2 n = glen > 1e-5 ? g / glen : float2(1, 0);   // across the edge
    const float2 t = float2(-n.y, n.x);                         // along the edge
    const min16float2 ta = min16float2(t / (1.0 + edge));       // longer reach along the edge
    const min16float2 na = min16float2(n * (1.0 + 0.6 * edge)); // shorter reach across it

    min16float3 sum = 0;
    min16float wsum = 0;
    [unroll] for (int k2 = 0; k2 < 12; ++k2) {
        const min16float2 d = min16float2(offs[k2]) - fh;
        const min16float2 e = min16float2(dot(d, ta), dot(d, na));
        const min16float w = Kernel(dot(e, e));
        sum += c[k2] * w;
        wsum += w;
    }
    const min16float3 col = sum / max(wsum, min16float(1e-3));
    const min16float3 cmin = min(min(c[3], c[4]), min(c[7], c[8]));
    const min16float3 cmax = max(max(c[3], c[4]), max(c[7], c[8]));
    return float4(float3(clamp(col, cmin, cmax)), 1.0);
}
