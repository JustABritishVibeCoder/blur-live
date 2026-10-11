// Bloom mip chain in linear light (dual-filter style):
//   mode 0: prefilter  - display frame -> 1/2 res, soft threshold, Karis average against fireflies
//   mode 1: downsample - 4 bilinear taps
//   mode 2: upsample   - current level + weight * 9-tap tent of the smaller level
#include "fx_common.hlsli"

Texture2D<float4> gSrc : register(t0);    // mode 0/1: source level; mode 2: smaller (coarser) level
Texture2D<float4> gCur : register(t1);    // mode 2: current level
RWTexture2D<float4> gDst : register(u0);
SamplerState gLinear : register(s0);

cbuffer Params : register(b0) {
    uint2 gDstSize;
    float2 gSrcInvSize;
    uint gMode;
    float gThreshold;  // linear
    float gKnee;
    float gUpWeight;
};

float3 Prefilter(float3 display) {
    float3 lin = SrgbToLinear(display);
    float br = max(lin.r, max(lin.g, lin.b));
    float soft = clamp(br - gThreshold + gKnee, 0.0, 2.0 * gKnee);
    soft = soft * soft / (4.0 * gKnee + 1e-5);
    float contrib = max(soft, br - gThreshold) / max(br, 1e-5);
    return lin * contrib;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gDstSize)) return;
    if (gMode == 2) {
        float2 uv = (float2(id.xy) + 0.5) / float2(gDstSize);
        float2 o = gSrcInvSize;  // texel size of the smaller level
        float3 s = gSrc.SampleLevel(gLinear, uv, 0).rgb * 4.0;
        s += (gSrc.SampleLevel(gLinear, uv + float2(o.x, 0), 0).rgb + gSrc.SampleLevel(gLinear, uv - float2(o.x, 0), 0).rgb +
              gSrc.SampleLevel(gLinear, uv + float2(0, o.y), 0).rgb + gSrc.SampleLevel(gLinear, uv - float2(0, o.y), 0).rgb) * 2.0;
        s += gSrc.SampleLevel(gLinear, uv + o, 0).rgb + gSrc.SampleLevel(gLinear, uv - o, 0).rgb +
             gSrc.SampleLevel(gLinear, uv + float2(o.x, -o.y), 0).rgb + gSrc.SampleLevel(gLinear, uv + float2(-o.x, o.y), 0).rgb;
        gDst[id.xy] = float4(gCur.Load(int3(id.xy, 0)).rgb + gUpWeight * s / 16.0, 1);
        return;
    }
    float2 c = (float2(id.xy) * 2.0 + 1.0) * gSrcInvSize;
    float2 o = gSrcInvSize;
    float3 t[4] = { gSrc.SampleLevel(gLinear, c + float2(-o.x, -o.y), 0).rgb, gSrc.SampleLevel(gLinear, c + float2(o.x, -o.y), 0).rgb,
                    gSrc.SampleLevel(gLinear, c + float2(-o.x, o.y), 0).rgb, gSrc.SampleLevel(gLinear, c + float2(o.x, o.y), 0).rgb };
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        float3 v = gMode == 0 ? Prefilter(t[i]) : t[i];
        float w = gMode == 0 ? 1.0 / (1.0 + Luma(v)) : 1.0;  // Karis average on the first level
        sum += v * w;
        wsum += w;
    }
    gDst[id.xy] = float4(sum / wsum, 1);
}
