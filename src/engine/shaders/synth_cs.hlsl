// Motion-compensated shutter integral (the real-time equivalent of Blur's "interpolate to a very
// high frame rate, then average the frames inside the shutter window").
//
// For each output pixel p, the causal shutter window [now - S, now] is split into segments between
// consecutive captured frames. Inside a segment (prev -> cur, flow v), the interpolated image at
// parameter u is reconstructed by bidirectional warping:
//     I_u(p) = lerp( prev(p - u*v), cur(p + (1-u)*v), u )
// and the shutter integral is approximated with stratified samples weighted by the weighting LUT.
// Where the motion estimate is unreliable (high block error) that pixel's contribution is replaced
// by the newest frame, so occlusions and failed matches stay sharp instead of ghosting.
// A detected scene cut terminates the window (older frames belong to a different shot).
#include "common.hlsli"

Texture2DArray<float4> gFrames : register(t0);
Texture2DArray<float4> gFlows : register(t1);
ByteAddressBuffer gStats : register(t2);
RWTexture2D<unorm float4> gOut : register(u0);
SamplerState gLinearClamp : register(s0);

struct Segment {
    uint prevSlice;
    uint curSlice;
    uint samples;
    uint pad;
    float u0;
    float u1;
    float tau0;
    float tau1;
};

#define MAX_SEGMENTS 12

cbuffer Params : register(b0) {
    uint2 gSize;
    float2 gInvSize;
    float2 gFlowUVScale;
    uint gSegCount;
    uint gNewestSlice;
    float gGamma;
    float gInvGamma;
    float gConfLo;
    float gConfHi;
    float gSceneAbs;
    float gSceneRel;
    uint gEnabled;
    uint gPad0;
    Segment gSeg[MAX_SEGMENTS];
    float4 gLut[16];
};

float LutWeight(float tau) {
    float x = saturate(tau) * 63.0;
    uint i0 = (uint)x;
    uint i1 = min(i0 + 1, 63u);
    float f = x - float(i0);
    return lerp(gLut[i0 >> 2][i0 & 3], gLut[i1 >> 2][i1 & 3], f);
}

float3 ToLinear(float3 c) { return gGamma == 1.0 ? c : pow(max(c, 0.0), gGamma); }
float3 FromLinear(float3 c) { return gGamma == 1.0 ? c : pow(max(c, 0.0), gInvGamma); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gSize)) return;
    float3 newest = gFrames.Load(int4(id.xy, gNewestSlice, 0)).rgb;
    if (gEnabled == 0 || gSegCount == 0) {
        gOut[id.xy] = float4(newest, 1);
        return;
    }

    float2 p = float2(id.xy) + 0.5;
    float3 acc = 0;
    float accW = 0;
    float totW = 0;

    [loop] for (uint s = 0; s < gSegCount; ++s) {
        Segment sg = gSeg[s];
        if (IsSceneCut(gStats, sg.curSlice, gSceneAbs, gSceneRel)) break;

        float4 f = gFlows.SampleLevel(gLinearClamp, float3(p * gFlowUVScale, sg.curSlice), 0);
        float2 v = f.xy;
        float conf = 1.0 - saturate((f.z - gConfLo) / max(gConfHi - gConfLo, 1e-4));

        float n = float(sg.samples);
        float span = (sg.tau1 - sg.tau0) / n;
        [loop] for (uint k = 0; k < sg.samples; ++k) {
            float t = (float(k) + 0.5) / n;
            float u = lerp(sg.u0, sg.u1, t);
            float w = LutWeight(lerp(sg.tau0, sg.tau1, t)) * span;
            float3 a = gFrames.SampleLevel(gLinearClamp, float3((p - u * v) * gInvSize, sg.prevSlice), 0).rgb;
            float3 b = gFrames.SampleLevel(gLinearClamp, float3((p + (1.0 - u) * v) * gInvSize, sg.curSlice), 0).rgb;
            float3 c = lerp(ToLinear(a), ToLinear(b), u);
            float wc = w * conf;
            acc += c * wc;
            accW += wc;
            totW += w;
        }
    }

    float3 res = newest;
    if (totW > 1e-6) res = FromLinear((acc + (totW - accW) * ToLinear(newest)) / totW);
    gOut[id.xy] = float4(res, 1);
}
