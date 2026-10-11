// Reduces the final motion field into per-frame statistics (used for scene-cut detection on the
// GPU, and read back asynchronously for the UI).
#include "common.hlsli"

Texture2D<float4> gFlow : register(t0);
Texture2DArray<float> gLumaCoarse : register(t1);  // pyramid level 4
RWByteAddressBuffer gStats : register(u0);

cbuffer Params : register(b0) {
    int2 gGridSize;
    int2 gLumaSize;
    uint gSlice;
    uint3 gPad;
};

groupshared float sBest[256];
groupshared float sZero[256];
groupshared float sLuma[256];
groupshared float sCount[256];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    float b = 0, z = 0, l = 0, c = 0;
    if (all(int2(id.xy) < gGridSize)) {
        float4 f = gFlow.Load(int3(id.xy, 0));
        b = f.z;
        z = f.w;
        int2 lp = min(int2(id.xy >> 1), gLumaSize - 1);
        l = gLumaCoarse.Load(int4(lp, gSlice, 0));
        c = 1;
    }
    sBest[gi] = b; sZero[gi] = z; sLuma[gi] = l; sCount[gi] = c;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1) {
        if (gi < s) {
            sBest[gi] += sBest[gi + s];
            sZero[gi] += sZero[gi + s];
            sLuma[gi] += sLuma[gi + s];
            sCount[gi] += sCount[gi + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0 && sCount[0] > 0) {
        uint ignored;
        gStats.InterlockedAdd(0, uint(sBest[0] * kStatScale + 0.5), ignored);
        gStats.InterlockedAdd(4, uint(sZero[0] * kStatScale + 0.5), ignored);
        gStats.InterlockedAdd(8, uint(sCount[0] + 0.5), ignored);
        gStats.InterlockedAdd(12, uint(sLuma[0] * kStatScale + 0.5), ignored);
    }
}
