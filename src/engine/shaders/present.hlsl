// Fullscreen-triangle blit of the processed frame into a swap-chain back buffer.
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

Texture2D<float4> gTex : register(t0);
SamplerState gSampler : register(s0);

float4 ps_main(VSOut i) : SV_Target {
    return float4(gTex.SampleLevel(gSampler, i.uv, 0).rgb, 1.0);
}
