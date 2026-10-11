// 3x3 vector median filter on a motion field: picks the neighbour vector with the smallest summed
// L1 distance to all others. Removes isolated outliers while keeping motion boundaries sharp.
#include "common.hlsli"

Texture2D<float4> gIn : register(t0);
RWTexture2D<float4> gOut : register(u0);

cbuffer Params : register(b0) {
    int2 gGridSize;
    int2 gPad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    if (any(p >= gGridSize)) return;

    float4 s[9];
    int k = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
            s[k++] = gIn.Load(int3(clamp(p + int2(dx, dy), int2(0, 0), gGridSize - 1), 0));

    int bi = 4;
    float bd = 1e30;
    [unroll] for (int i = 0; i < 9; ++i) {
        float d = 0;
        [unroll] for (int j = 0; j < 9; ++j) d += abs(s[i].x - s[j].x) + abs(s[i].y - s[j].y);
        if (d < bd) { bd = d; bi = i; }
    }
    float4 r = s[4];
    r.xy = s[bi].xy;
    // Keep the error of the chosen vector if it is worse, so confidence stays conservative.
    r.z = max(s[4].z, s[bi].z);
    gOut[id.xy] = r;
}
