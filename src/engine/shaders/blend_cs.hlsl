// Classic temporal frame blending (no motion compensation): weighted average of the last N frames.
// This is what Blur does when interpolation is disabled. Cheap, but produces discrete "ghost"
// copies of moving objects instead of continuous streaks.
#include "common.hlsli"

Texture2DArray<float4> gFrames : register(t0);
ByteAddressBuffer gStats : register(t1);
RWTexture2D<unorm float4> gOut : register(u0);

cbuffer Params : register(b0) {
    uint2 gSize;
    uint gCount;
    uint gEnabled;
    float gGamma;
    float gInvGamma;
    float gSceneAbs;
    float gSceneRel;
    uint4 gSlices[4];   // newest first
    float4 gWeights[4];
};

float3 ToLinear(float3 c) { return gGamma == 1.0 ? c : pow(max(c, 0.0), gGamma); }
float3 FromLinear(float3 c) { return gGamma == 1.0 ? c : pow(max(c, 0.0), gInvGamma); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gSize)) return;
    uint newestSlice = gSlices[0].x;
    float3 newest = gFrames.Load(int4(id.xy, newestSlice, 0)).rgb;
    if (gEnabled == 0 || gCount <= 1) {
        gOut[id.xy] = float4(newest, 1);
        return;
    }
    float3 acc = 0;
    float totW = 0;
    [loop] for (uint i = 0; i < gCount; ++i) {
        uint slice = gSlices[i >> 2][i & 3];
        float w = gWeights[i >> 2][i & 3];
        acc += ToLinear(gFrames.Load(int4(id.xy, slice, 0)).rgb) * w;
        totW += w;
        // Statistics of `slice` describe the pair (older frame -> slice); stop before crossing a cut.
        if (IsSceneCut(gStats, slice, gSceneAbs, gSceneRel)) break;
    }
    gOut[id.xy] = float4(totW > 0 ? FromLinear(acc / totW) : newest, 1);
}
