struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

cbuffer ConvertCB : register(b0) {
    float4 row0;     // rgb = (row0|row1|row2) . float4(y, u, v, 1)
    float4 row1;
    float4 row2;
    uint2 size;      // source size in pixels
    uint flags;      // bit0: UYVY byte order
    uint pad;
    int4 xform;      // crop origin x, y and cropped width, height (source pixels)
    uint4 xform2;    // quarter turns clockwise, mirror, output width, height
    float4 hdr;      // transfer (0 SDR, 1 PQ, 2 HLG), BT.2020 primaries, reference white nits, peak nits
};

// ---- HDR signals shown on SDR: the curve undone (to nits), BT.2020 -> BT.709 colours, highlights rolled off ----
float3 PqToNits(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float3 HlgToNits(float3 e)   // inverse OETF, then the reference OOTF for a 1000-nit display (system gamma 1.2)
{
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = saturate(e);
    float3 lo = e * e / 3.0, hi = (exp((e - c) / a) + b) / 12.0;
    float3 s = float3(e.r <= 0.5 ? lo.r : hi.r, e.g <= 0.5 ? lo.g : hi.g, e.b <= 0.5 ? lo.b : hi.b);
    float ys = max(dot(s, float3(0.2627, 0.6780, 0.0593)), 1e-6);
    return 1000.0 * pow(ys, 0.2) * s;
}

static const float3x3 kBt2020To709 = {1.6605, -0.5876, -0.0728,
                                      -0.1246, 1.1329, -0.0083,
                                      -0.0182, -0.1006, 1.1187};

float3 SrgbToLinear(float3 c)
{
    c = saturate(c);
    float3 lo = c / 12.92, hi = pow((c + 0.055) / 1.055, 2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r, c.g <= 0.04045 ? lo.g : hi.g, c.b <= 0.04045 ? lo.b : hi.b);
}

// Conversion output: the SDR picture (always) and, for an HDR screen, linear light (1.0 = white; only written when a
// second target is bound).
struct ConvOut {
    float4 sdr : SV_Target0;
    float4 lin : SV_Target1;
};

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    float3 lo = v * 12.92, hi = 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    return float3(v.r <= 0.0031308 ? lo.r : hi.r, v.g <= 0.0031308 ? lo.g : hi.g, v.b <= 0.0031308 ? lo.b : hi.b);
}

// Linear light (1.0 = SDR white) of an HDR signal value, in BT.709 primaries (may be > 1, or < 0 for colours outside
// BT.709).
float3 HdrToLinear(float3 rgb)
{
    float3 nits = hdr.x < 1.5 ? PqToNits(rgb) : HlgToNits(rgb);
    if (hdr.y > 0.5) nits = mul(kBt2020To709, nits);
    return nits / hdr.z;
}

// SDR picture of an HDR signal: up to 75 % of white unchanged, above that a smooth roll-off (slope 1 at the knee) that
// reaches white at the source's peak; scaled on the brightest channel so colours keep their hue.
float3 ToneMap(float3 v)
{
    v = max(v, 0.0);
    const float ks = 0.75;
    float m = max(v.r, max(v.g, v.b));
    if (m > ks) {
        float range = 1.0 - ks;
        float xMax = max((hdr.w / hdr.z - ks) / range, 1e-3);
        float x = (m - ks) / range;
        float f = x * (1.0 + x / (xMax * xMax)) / (1.0 + x);
        v *= (ks + range * min(f, 1.0)) / m;
    }
    return LinearToSrgb(v);
}

// HDR encoding: linear light in BT.709 primaries (1.0 = `white` nits; or an sRGB SDR picture, whose white is `white`)
// -> PQ-encoded BT.2020 R'G'B'.
static const float3x3 kBt709To2020 = {0.6274, 0.3293, 0.0433,
                                      0.0691, 0.9195, 0.0114,
                                      0.0164, 0.0880, 0.8956};
float3 EncodePq2020(float3 c, float white, bool srgb)
{
    float3 lin = srgb ? SrgbToLinear(c) : c;
    float3 nits = max(mul(kBt709To2020, lin), 0.0) * white;
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

// What every conversion shader returns: SDR as it is, HDR tone-mapped; plus linear light for an HDR screen.
ConvOut FinishColor(float3 rgb)
{
    ConvOut o;
    o.sdr = float4(hdr.x < 0.5 ? rgb : ToneMap(HdrToLinear(rgb)), 1.0);
    o.lin = float4(hdr.x < 0.5 ? SrgbToLinear(rgb) : HdrToLinear(rgb), 1.0);
    return o;
}

// The source pixel an output pixel shows: mirrored (in the output), turned back, then offset by the crop.
int2 SourcePixel(float2 pos)
{
    int2 o = int2(pos);
    if (xform2.y != 0) o.x = int(xform2.z) - 1 - o.x;
    int2 c = o;
    if (xform2.x == 1) c = int2(o.y, xform.w - 1 - o.x);
    else if (xform2.x == 2) c = int2(xform.z - 1 - o.x, xform.w - 1 - o.y);
    else if (xform2.x == 3) c = int2(xform.z - 1 - o.y, o.x);
    return c + xform.xy;
}

float3 YuvToRgb(float y, float u, float v)
{
    float4 p = float4(y, u, v, 1.0);
    return saturate(float3(dot(row0, p), dot(row1, p), dot(row2, p)));
}
