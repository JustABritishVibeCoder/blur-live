// Luma pyramid level n -> level n+1 (2x2 box filter).
#include "common.hlsli"

Texture2DArray<float> gSrc : register(t0);
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
    float l = gSrc.Load(int4(min(s, mx), gSrcSlice, 0))
            + gSrc.Load(int4(min(s + int2(1, 0), mx), gSrcSlice, 0))
            + gSrc.Load(int4(min(s + int2(0, 1), mx), gSrcSlice, 0))
            + gSrc.Load(int4(min(s + int2(1, 1), mx), gSrcSlice, 0));
    gDst[uint3(id.xy, gDstSlice)] = l * 0.25;
}
