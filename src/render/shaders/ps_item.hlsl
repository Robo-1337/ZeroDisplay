#include "common.hlsli"

// One scene layer: crop -> sharpen -> colour correction -> chroma key -> opacity.
// Output is premultiplied alpha (blended with ONE / INV_SRC_ALPHA).
Texture2D<float4> src : register(t0);
SamplerState smp : register(s0);

cbuffer ItemCB : register(b1) {
    float4 uvRect;      // crop: u0 v0 u1 v1
    float4 solid;       // solid colour (straight alpha) when misc.y > 0.5
    float4 keyColor;    // rgb, w = chroma key enabled
    float4 keyParams;   // similarity, smoothness, spill, source is premultiplied
    float4 adjust0;     // brightness, contrast, saturation, gamma
    float4 adjust1;     // hue (radians), colour correction enabled, sharpen, unused
    float4 misc;        // opacity, solid flag, texel width, texel height
};

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float2 Chroma(float3 c)
{
    return float2(dot(c, float3(-0.1146, -0.3854, 0.5)), dot(c, float3(0.5, -0.4542, -0.0458)));
}

float4 main(VSOut i) : SV_Target
{
    float2 uv = lerp(uvRect.xy, uvRect.zw, i.uv);
    float4 c;
    if (misc.y > 0.5) {
        c = solid;
    } else {
        c = src.SampleLevel(smp, uv, 0);
        if (keyParams.w > 0.5 && c.a > 0.0) c.rgb /= c.a;
        if (adjust1.z > 0.0) {
            float2 t = misc.zw;
            float3 n = src.SampleLevel(smp, uv + float2(t.x, 0), 0).rgb + src.SampleLevel(smp, uv - float2(t.x, 0), 0).rgb +
                       src.SampleLevel(smp, uv + float2(0, t.y), 0).rgb + src.SampleLevel(smp, uv - float2(0, t.y), 0).rgb;
            if (keyParams.w > 0.5 && c.a > 0.0) n /= c.a;
            c.rgb = saturate(c.rgb + adjust1.z * (4.0 * c.rgb - n));
        }
    }

    if (adjust1.y > 0.5) {
        float3 rgb = c.rgb + adjust0.x;
        rgb = (rgb - 0.5) * adjust0.y + 0.5;
        rgb = lerp(dot(rgb, kLuma).xxx, rgb, adjust0.z);
        if (adjust1.x != 0.0) {
            // rotate around the grey axis
            float cs = cos(adjust1.x), sn = sin(adjust1.x);
            float3 k = float3(0.57735, 0.57735, 0.57735);
            rgb = rgb * cs + cross(k, rgb) * sn + k * dot(k, rgb) * (1.0 - cs);
        }
        rgb = pow(saturate(rgb), 1.0 / max(adjust0.w, 0.01));
        c.rgb = saturate(rgb);
    }

    if (keyColor.w > 0.5) {
        float d = distance(Chroma(keyColor.rgb), Chroma(c.rgb));
        float base = d - keyParams.x * 0.5;
        c.a *= saturate(base / max(keyParams.y * 0.5, 1e-4));
        // spill suppression: pull colours close to the key towards grey
        float keep = saturate(base / max(keyParams.z * 0.5, 1e-4));
        c.rgb = lerp(dot(c.rgb, kLuma).xxx, c.rgb, keep);
    }

    c.a *= misc.x;
    return float4(c.rgb * c.a, c.a);
}
