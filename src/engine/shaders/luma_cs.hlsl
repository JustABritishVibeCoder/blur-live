// Full-resolution BGRA frame -> half-resolution luma (2x2 box filter). Pyramid level 1.
#include "common.hlsli"

Texture2DArray<float4> gSrc : register(t0);
RWTexture2DArray<float> gDst : register(u0);

cbuffer Params : register(b0) {
    uint2 gSrcSize;
    uint2 gDstSize;
    uint gSrcSlice;
    uint gDstSlice;
    uint2 gPad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gDstSize)) return;
    int2 s = int2(id.xy) * 2;
    int2 mx = int2(gSrcSize) - 1;
    float l = 0;
    [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x)
            l += Luma(gSrc.Load(int4(min(s + int2(x, y), mx), gSrcSlice, 0)).rgb);
    gDst[uint3(id.xy, gDstSlice)] = l * 0.25;
}
