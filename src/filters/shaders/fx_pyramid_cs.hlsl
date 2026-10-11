// 2x luma downsample with 4 bilinear taps (= 4x4 tent filter). Level 1 (FROM_COLOR) reads the
// display-space colour frame; deeper levels read the previous luma level.
#include "fx_common.hlsli"

SamplerState gLinear : register(s0);
RWTexture2D<float> gDst : register(u0);

#ifdef FROM_COLOR
Texture2D<float4> gSrc : register(t0);
float Fetch(float2 uv) { return Luma(gSrc.SampleLevel(gLinear, uv, 0).rgb); }
#else
Texture2D<float> gSrc : register(t0);
float Fetch(float2 uv) { return gSrc.SampleLevel(gLinear, uv, 0); }
#endif

cbuffer Params : register(b0) {
    uint2 gDstSize;
    float2 gSrcInvSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gDstSize)) return;
    float2 c = (float2(id.xy) * 2.0 + 1.0) * gSrcInvSize;  // centre of the 2x2 source block
    float2 o = gSrcInvSize;
    gDst[id.xy] = 0.25 * (Fetch(c + float2(-o.x, -o.y)) + Fetch(c + float2(o.x, -o.y)) +
                          Fetch(c + float2(-o.x, o.y)) + Fetch(c + float2(o.x, o.y)));
}
