#ifndef BLURLIVE_COMMON_HLSLI
#define BLURLIVE_COMMON_HLSLI

static const float3 kLumaWeights = float3(0.299, 0.587, 0.114);
float Luma(float3 c) { return dot(c, kLumaWeights); }

// Per-frame motion statistics written by stats_cs (fixed point, 1e4 scale), 16 bytes per slot:
//   x = sum of best block error, y = sum of zero-motion block error, z = block count, w = sum luma
static const float kStatScale = 10000.0;

bool IsSceneCut(ByteAddressBuffer stats, uint slice, float absThreshold, float relThreshold) {
    uint4 s = stats.Load4(slice * 16);
    if (s.z == 0) return false;
    float inv = 1.0 / (kStatScale * float(s.z));
    float best = float(s.x) * inv;
    float zero = float(s.y) * inv;
    // A cut is a frame pair where even the best motion vector leaves a large error AND motion
    // compensation barely helps compared to no motion at all.
    return best > absThreshold && best > relThreshold * zero;
}

#endif
