// Hierarchical block-matching motion estimation (one pyramid level).
//
// The motion field lives on a grid of 4x4-pixel cells at every level. Each cell is matched with an
// overlapping 8x8 block centred on it (2x overlap, like SVP/MVTools' overlap=2), using the mean
// absolute luma difference. The vector v maps PREV -> CUR: content at x in prev is at x+v in cur.
//
//  * Coarsest level: exhaustive search in [-radius, radius]^2.
//  * Finer levels: candidates = upscaled vectors of the 5 nearest coarse cells + zero vector,
//    then an exhaustive refinement around the best candidate.
//  * Optional half-pixel refinement with bilinear sampling (used on the finest level).
// Output: xy = vector in FULL-RESOLUTION pixels, z = best block error, w = zero-motion error.
#include "common.hlsli"

Texture2DArray<float> gLuma : register(t0);
Texture2D<float4> gCoarse : register(t1);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinearClamp : register(s0);

cbuffer Params : register(b0) {
    int2 gLevelSize;
    int2 gGridSize;
    int2 gCoarseGridSize;
    uint gCurSlice;
    uint gPrevSlice;
    int gRadius;
    int gCoarsest;
    float gLevelScale;  // full-resolution pixels per level pixel
    float gLambda;      // smoothness penalty per level pixel of deviation from the predictor
    int gSubpixel;
    float3 gPad;
};

float LoadCur(int2 p) { return gLuma.Load(int4(clamp(p, int2(0, 0), gLevelSize - 1), gCurSlice, 0)); }
float LoadPrev(int2 p) { return gLuma.Load(int4(clamp(p, int2(0, 0), gLevelSize - 1), gPrevSlice, 0)); }

float BlockCost(float cur[64], int2 org, int2 v) {
    float s = 0;
    [unroll] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x)
            s += abs(cur[y * 8 + x] - LoadPrev(org + int2(x, y) - v));
    return s * (1.0 / 64.0);
}

float BlockCostF(float cur[64], int2 org, float2 v) {
    float s = 0;
    float2 inv = 1.0 / float2(gLevelSize);
    [unroll] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x) {
            float2 uv = (float2(org + int2(x, y)) + 0.5 - v) * inv;
            s += abs(cur[y * 8 + x] - gLuma.SampleLevel(gLinearClamp, float3(uv, gPrevSlice), 0));
        }
    return s * (1.0 / 64.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 cell = int2(id.xy);
    if (any(cell >= gGridSize)) return;
    int2 org = cell * 4 - 2;

    float cur[64];
    [unroll] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x)
            cur[y * 8 + x] = LoadCur(org + int2(x, y));

    float zeroCost = BlockCost(cur, org, int2(0, 0));
    float2 pred = float2(0, 0);
    int2 best = int2(0, 0);
    float bestRaw = zeroCost;
    float bestPen = zeroCost;

    if (gCoarsest != 0) {
        [loop] for (int dy = -gRadius; dy <= gRadius; ++dy) {
            [loop] for (int dx = -gRadius; dx <= gRadius; ++dx) {
                int2 v = int2(dx, dy);
                float raw = BlockCost(cur, org, v);
                float pen = raw + gLambda * length(float2(v));
                if (pen < bestPen) { bestPen = pen; bestRaw = raw; best = v; }
            }
        }
    } else {
        int2 cc = int2(id.xy >> 1);
        int2 cmax = gCoarseGridSize - 1;
        pred = gCoarse.Load(int3(clamp(cc, int2(0, 0), cmax), 0)).xy / gLevelScale;
        bestPen = zeroCost + gLambda * length(pred);

        static const int2 kOffsets[5] = { int2(0, 0), int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
        [unroll] for (int i = 0; i < 5; ++i) {
            float2 cv = gCoarse.Load(int3(clamp(cc + kOffsets[i], int2(0, 0), cmax), 0)).xy / gLevelScale;
            int2 v = int2(round(cv));
            float raw = BlockCost(cur, org, v);
            float pen = raw + gLambda * length(float2(v) - pred);
            if (pen < bestPen) { bestPen = pen; bestRaw = raw; best = v; }
        }

        int2 center = best;
        [loop] for (int dy = -gRadius; dy <= gRadius; ++dy) {
            [loop] for (int dx = -gRadius; dx <= gRadius; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int2 v = center + int2(dx, dy);
                float raw = BlockCost(cur, org, v);
                float pen = raw + gLambda * length(float2(v) - pred);
                if (pen < bestPen) { bestPen = pen; bestRaw = raw; best = v; }
            }
        }
    }

    float2 bestF = float2(best);
    if (gSubpixel != 0) {
        float2 c = bestF;
        [unroll] for (int sy = -1; sy <= 1; ++sy) {
            [unroll] for (int sx = -1; sx <= 1; ++sx) {
                if (sx == 0 && sy == 0) continue;
                float2 v = c + float2(sx, sy) * 0.5;
                float raw = BlockCostF(cur, org, v);
                if (raw < bestRaw) { bestRaw = raw; bestF = v; }
            }
        }
    }

    gOut[id.xy] = float4(bestF * gLevelScale, bestRaw, zeroCost);
}
