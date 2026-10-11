// Subtle edge-preserving denoise: 5x5 bilateral filter in display space, blended by strength.
#include "fx_common.hlsli"

Texture2D<float4> gSrc : register(t0);
RWTexture2D<unorm float4> gDst : register(u0);

cbuffer Params : register(b0) {
    uint2 gSize;
    float gStrength;
    float gInvRangeSigma2;  // 1 / (2 sigma_r^2)
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gSize)) return;
    int2 p = int2(id.xy);
    int2 mx = int2(gSize) - 1;
    float3 c = gSrc.Load(int3(p, 0)).rgb;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            float3 s = gSrc.Load(int3(clamp(p + int2(x, y), int2(0, 0), mx), 0)).rgb;
            float3 d = s - c;
            float w = exp(-float(x * x + y * y) * 0.2222 - dot(d, d) * gInvRangeSigma2);  // sigma_s = 1.5
            sum += s * w;
            wsum += w;
        }
    }
    gDst[id.xy] = float4(lerp(c, sum / wsum, gStrength), 1);
}
