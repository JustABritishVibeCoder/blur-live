// Blur Live filter suite: every per-pixel effect in ONE full-resolution compute pass.
//
// Order of operations (each step is skipped by a uniform branch when its filter is off):
//   display space : chromatic aberration -> sharpen -> detail / clarity / local contrast
//   linear light  : exposure -> white balance + RGB gains (one 3x3 matrix) -> bloom -> colour-vision
//   OkLCh         : saturation, vibrance, selective colour, colour focus (+ gamut compression)
//   display space : shadows/highlights -> brightness -> contrast -> gamma -> levels -> lift/gamma/gain
//                   -> colour grading -> 3D LUT -> vignette -> film grain -> dither
// Effects that need neighbourhoods at large scales read small pre-computed pyramids (fx_pyramid_cs,
// fx_bloom_cs). With every filter off this pass does not run at all.
#include "fx_common.hlsli"

Texture2D<float4> gSrc : register(t0);           // processed frame (motion blur output), display-encoded
Texture2DArray<float4> gOriginal : register(t1); // raw captured frames (for compare view)
Texture2D<float> gPyr1 : register(t2);           // luma 1/2
Texture2D<float> gPyr2 : register(t3);           // luma 1/4
Texture2D<float> gPyr3 : register(t4);           // luma 1/8
Texture2D<float> gPyr4 : register(t5);           // luma 1/16
Texture2D<float4> gBloom : register(t6);         // bloom, linear, 1/2
Texture3D<float4> gLut : register(t7);
RWTexture2D<unorm float4> gOut : register(u0);
SamplerState gLinear : register(s0);

// Bit = 1 << FilterId (see filter_registry.h)
#define F_SHARPEN       (1u << 0)
#define F_DETAIL        (1u << 1)
#define F_CLARITY       (1u << 2)
#define F_LOCALCONTRAST (1u << 3)
#define F_DENOISE       (1u << 4)
#define F_EXPOSURE      (1u << 5)
#define F_BRIGHTNESS    (1u << 6)
#define F_CONTRAST      (1u << 7)
#define F_GAMMA         (1u << 8)
#define F_SATURATION    (1u << 9)
#define F_VIBRANCE      (1u << 10)
#define F_WHITEBALANCE  (1u << 11)
#define F_RGB           (1u << 12)
#define F_SELECTIVE     (1u << 13)
#define F_SHADOWSHI     (1u << 14)
#define F_LEVELS        (1u << 15)
#define F_LGG           (1u << 16)
#define F_GRADING       (1u << 17)
#define F_LUT           (1u << 18)
#define F_BLOOM         (1u << 19)
#define F_VIGNETTE      (1u << 20)
#define F_GRAIN         (1u << 21)
#define F_CHROMATIC     (1u << 22)
#define F_CVD           (1u << 23)
#define F_FOCUS         (1u << 24)

cbuffer FxParams : register(b0) {
    uint2 gSize; float2 gInvSize;
    uint gFlags; uint gFrame; float gCompareSplit; uint gCompare;
    uint gOrigSlice; uint gDither; float2 gPyrSize1;
    float2 gPyrSize2; float2 gPyrSize3;
    float2 gPyrSize4; float2 gPad0;
    float4 gSharpen;      // intensity, detail, edges, radius
    float4 gSharpen2;     // threshold, halo, -, -
    float4 gDetail;       // detail, clarity, clarity radius, local contrast
    float4 gExposure;     // exp2(ev), protect, brightness, contrast (curve exponent)
    float4 gTone;         // 1/gamma, saturation, vibrance, -
    float4 gColorM0; float4 gColorM1; float4 gColorM2;  // white balance * RGB gains (linear)
    float4 gShadowHi;     // shadows, highlights, locality, -
    float4 gLevels;       // in black, in white, out black, out white
    float4 gLift; float4 gInvGamma; float4 gGain;        // lift / gamma / gain per channel
    float4 gGradeShadow;  // rgb offset, w balance
    float4 gGradeHigh;    // rgb offset
    float4 gGradeAmbient; // rgb offset
    float4 gLutParams;    // strength, scale, offset, -
    float4 gBloomParams;  // intensity
    float4 gVignette;     // amount, inner radius, falloff width, aspect
    float4 gGrain;        // amount, size
    float4 gChroma;       // amount (uv units)
    float4 gCvd0; float4 gCvd1; float4 gCvd2;
    float4 gCvdParams;    // strength, separation, simulate, tritan
    float4 gFocus;        // boost, hue (rad), width (rad), mute
    float4 gSelective[8]; // hue shift (rad), saturation, lightness, band centre hue (rad)
    float4 gSelParams;    // strength
};

bool On(uint f) { return (gFlags & f) != 0; }

// Fast B-spline bicubic sampling with 4 bilinear taps: smooth upsampling of the coarse pyramid
// (plain bilinear magnification would imprint a block grid into local-contrast effects).
float SampleBicubic(Texture2D<float> tex, float2 uv, float2 texSize) {
    float2 invSize = 1.0 / texSize;
    float2 iTc = uv * texSize - 0.5;
    float2 tc = floor(iTc);
    float2 f = iTc - tc;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;
    float2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    float2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    float2 w3 = f3 / 6.0;
    float2 g0 = w0 + w1, g1 = w2 + w3;
    float2 p0 = (tc - 1.0 + w1 / g0 + 0.5) * invSize;
    float2 p1 = (tc + 1.0 + w3 / g1 + 0.5) * invSize;
    return g0.y * (g0.x * tex.SampleLevel(gLinear, p0, 0) + g1.x * tex.SampleLevel(gLinear, float2(p1.x, p0.y), 0)) +
           g1.y * (g0.x * tex.SampleLevel(gLinear, float2(p0.x, p1.y), 0) + g1.x * tex.SampleLevel(gLinear, p1, 0));
}

// Smoothly limits a detail signal so strong edges don't produce halos.
float Compress(float d, float k) { return d * rsqrt(1.0 + d * d * k * k); }

float SrcLuma(float2 uv) { return Luma(gSrc.SampleLevel(gLinear, uv, 0).rgb); }

// Contrast-adaptive luminance sharpening (CAS-style amount, two kernels, anti-halo clamp).
float3 Sharpen(float2 uv, float3 c) {
    float2 o = gSharpen.w * gInvSize;
    float lc = Luma(gSrc.SampleLevel(gLinear, uv, 0).rgb);
    float n = SrcLuma(uv + float2(0, -o.y)), s = SrcLuma(uv + float2(0, o.y));
    float e = SrcLuma(uv + float2(o.x, 0)), w = SrcLuma(uv + float2(-o.x, 0));
    float nw = SrcLuma(uv + float2(-o.x, -o.y)), ne = SrcLuma(uv + float2(o.x, -o.y));
    float sw = SrcLuma(uv + float2(-o.x, o.y)), se = SrcLuma(uv + float2(o.x, o.y));
    float mn = min(lc, min(min(min(n, s), min(e, w)), min(min(nw, ne), min(sw, se))));
    float mx = max(lc, max(max(max(n, s), max(e, w)), max(max(nw, ne), max(sw, se))));
    // Less sharpening where local contrast is already high (AMD CAS idea): avoids over-sharpened edges.
    float amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));

    float fine = lc - 0.25 * (n + s + e + w);
    float edge = 0.0;
    if (gSharpen.z > 0.0) {
        float2 o2 = o * 2.0;
        float ring = SrcLuma(uv + float2(0, -o2.y)) + SrcLuma(uv + float2(0, o2.y)) + SrcLuma(uv + float2(o2.x, 0)) +
                     SrcLuma(uv + float2(-o2.x, 0)) + SrcLuma(uv + float2(-o2.x, -o2.y)) +
                     SrcLuma(uv + float2(o2.x, -o2.y)) + SrcLuma(uv + float2(-o2.x, o2.y)) +
                     SrcLuma(uv + float2(o2.x, o2.y));
        edge = lc - ring * 0.125;
    }
    float thr = gSharpen2.x;
    if (thr > 0.0) {
        fine *= smoothstep(thr, 2.0 * thr, abs(fine));
        edge *= smoothstep(thr, 2.0 * thr, abs(edge));
    }
    float delta = gSharpen.x * amp * (gSharpen.y * 2.0 * fine + gSharpen.z * 1.5 * edge);
    float allow = (1.0 - gSharpen2.y) * 0.1;  // default halo 0.8 -> at most ~5/255 overshoot
    float ln = clamp(lc + delta, mn - allow, mx + allow);
    return c + (ln - lc);
}

float3 Daltonize(float3 lin) {
    float3 sim = float3(dot(gCvd0.xyz, lin), dot(gCvd1.xyz, lin), dot(gCvd2.xyz, lin));
    if (gCvdParams.z > 0.5) return lerp(lin, sim, gCvdParams.x);  // simulate
    float3 err = (lin - sim) * (gCvdParams.y * 2.0);
    float3 corr = gCvdParams.w > 0.5 ? float3(err.r + 0.7 * err.b, err.g + 0.7 * err.b, 0.0)   // tritan
                                     : float3(0.0, 0.7 * err.r + err.g, 0.7 * err.r + err.b); // protan/deutan
    return lin + gCvdParams.x * corr;
}

float3 OkLchAdjust(float3 lin) {
    float3 lab = LinearToOklab(max(lin, 0.0));
    float C = length(lab.yz);
    float h = atan2(lab.z, lab.y);
    if (h < 0.0) h += kTwoPi;
    float L = lab.x;
    float chromaGate = smoothstep(0.0, 0.04, C);  // greys have no meaningful hue

    if (On(F_SATURATION)) C *= max(0.0, 1.0 + gTone.y);
    if (On(F_VIBRANCE)) C *= max(0.0, 1.0 + gTone.z * (1.0 - smoothstep(0.0, 0.25, C)));
    if (On(F_SELECTIVE)) {
        float dh = 0.0, ds = 0.0, dl = 0.0;
        [unroll] for (int i = 0; i < 8; ++i) {
            float wgt = saturate(1.0 - HueDistance(h, gSelective[i].w) / 0.87);  // ~50 deg half-width
            wgt = wgt * wgt * (3.0 - 2.0 * wgt);
            dh += wgt * gSelective[i].x;
            ds += wgt * gSelective[i].y;
            dl += wgt * gSelective[i].z;
        }
        float k = gSelParams.x * chromaGate;
        h += dh * k;
        C *= max(0.0, 1.0 + ds * k);
        L += dl * k * 0.15;
    }
    if (On(F_FOCUS)) {
        float d = HueDistance(h, gFocus.y);
        float wgt = 1.0 - smoothstep(gFocus.z * 0.5, gFocus.z, d);
        wgt *= chromaGate;
        C *= 1.0 + gFocus.x * wgt * 0.9;
        L += gFocus.x * wgt * 0.06 * (1.0 - L);
        C *= 1.0 - gFocus.w * (1.0 - wgt);
    }
    return OklabToLinear(float3(L, C * cos(h), C * sin(h)));
}

float ContrastCurve(float x, float k) {  // S-curve around 0.5, maps [0,1] -> [0,1]
    x = saturate(x);
    return x < 0.5 ? 0.5 * pow(2.0 * x, k) : 1.0 - 0.5 * pow(2.0 - 2.0 * x, k);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gSize)) return;
    float2 uv = (float2(id.xy) + 0.5) * gInvSize;

    // ---- Compare view: original capture on the left of the split line ----
    if (gCompare != 0) {
        float splitX = gCompareSplit * float(gSize.x);
        if (abs(float(id.x) + 0.5 - splitX) < 1.0) { gOut[id.xy] = float4(0.67, 0.55, 0.98, 1); return; }
        if (float(id.x) < splitX) { gOut[id.xy] = float4(gOriginal.Load(int4(id.xy, gOrigSlice, 0)).rgb, 1); return; }
    }

    float3 c = gSrc.Load(int3(id.xy, 0)).rgb;
    if (gFlags == 0) { gOut[id.xy] = float4(c, 1); return; }

    // ---- Display-space spatial effects ----
    if (On(F_CHROMATIC)) {
        float2 off = (uv - 0.5) * gChroma.x;
        c.r = gSrc.SampleLevel(gLinear, uv + off, 0).r;
        c.b = gSrc.SampleLevel(gLinear, uv - off, 0).b;
    }
    if (On(F_SHARPEN)) c = Sharpen(uv, c);
    if (On(F_DETAIL | F_CLARITY | F_LOCALCONTRAST)) {
        float l = Luma(c);
        float nl = l;
        if (On(F_DETAIL)) nl += gDetail.x * 1.5 * Compress(l - gPyr1.SampleLevel(gLinear, uv, 0), 8.0);
        if (On(F_CLARITY)) {
            float b = lerp(SampleBicubic(gPyr2, uv, gPyrSize2), SampleBicubic(gPyr3, uv, gPyrSize3), gDetail.z);
            float mid = 1.0 - pow(abs(2.0 * l - 1.0), 2.0);
            nl += gDetail.y * 1.2 * mid * Compress(l - b, 5.0);
        }
        if (On(F_LOCALCONTRAST)) nl += gDetail.w * 0.9 * Compress(l - SampleBicubic(gPyr4, uv, gPyrSize4), 3.0);
        c += nl - l;
    }

    // ---- Linear-light colour ----
    const bool linearStage = On(F_EXPOSURE | F_WHITEBALANCE | F_RGB | F_BLOOM | F_CVD | F_SATURATION | F_VIBRANCE |
                                F_SELECTIVE | F_FOCUS);
    if (linearStage) {
        float3 lin = SrgbToLinear(c);
        if (On(F_EXPOSURE) && gExposure.x != 1.0) {
            lin *= gExposure.x;
            if (gExposure.x > 1.0 && gExposure.y > 0.0) {
                const float knee = 0.75;  // soft shoulder above the knee instead of hard clipping
                float3 over = max(lin - knee, 0.0);
                float3 soft = knee + (1.0 - knee) * (1.0 - exp(-over / (1.0 - knee)));
                lin = lerp(lin, lin > knee ? soft : lin, gExposure.y);
            }
        }
        if (On(F_WHITEBALANCE | F_RGB))
            lin = float3(dot(gColorM0.xyz, lin), dot(gColorM1.xyz, lin), dot(gColorM2.xyz, lin));
        if (On(F_BLOOM)) lin += gBloom.SampleLevel(gLinear, uv, 0).rgb * gBloomParams.x;
        if (On(F_CVD)) lin = Daltonize(lin);
        if (On(F_SATURATION | F_VIBRANCE | F_SELECTIVE | F_FOCUS)) lin = OkLchAdjust(lin);
        c = LinearToSrgb(GamutCompress(lin));
    }

    // ---- Display-space tone ----
    if (On(F_SHADOWSHI)) {
        float l = Luma(c);
        float local = lerp(SampleBicubic(gPyr2, uv, gPyrSize2), SampleBicubic(gPyr3, uv, gPyrSize3), 0.5);
        // Halo-free locality: a dark pixel next to something bright must not get less shadow lift than
        // the same dark pixel elsewhere (and vice versa for highlights), so the neighbourhood can only
        // extend a mask, never weaken it: min() for the shadow mask, max() for the highlight mask.
        float lmS = saturate(lerp(l, min(l, local), gShadowHi.z));
        float lmH = saturate(lerp(l, max(l, local), gShadowHi.z));
        float ms = 1.0 - smoothstep(0.0, 0.55, lmS); ms *= ms;
        float mh = smoothstep(0.45, 1.0, lmH); mh *= mh;
        float lt = l;
        lt = gShadowHi.x >= 0.0 ? lt + gShadowHi.x * ms * 0.3 * (1.0 - lt) : lt * (1.0 + gShadowHi.x * ms * 0.5);
        lt = gShadowHi.y >= 0.0 ? lt + gShadowHi.y * mh * 0.3 * (1.0 - lt) : lt * (1.0 + gShadowHi.y * mh * 0.35);
        float3 additive = c + (lt - l);
        float3 ratio = l > 0.002 ? c * (lt / l) : additive;
        c = lerp(additive, ratio, 0.5);
    }
    if (On(F_BRIGHTNESS)) {
        float b = gExposure.z;
        c = b >= 0.0 ? c + b * 0.4 * (1.0 - saturate(c)) : c * (1.0 + b * 0.5);
    }
    if (On(F_CONTRAST) && gExposure.w != 1.0)
        c = float3(ContrastCurve(c.r, gExposure.w), ContrastCurve(c.g, gExposure.w), ContrastCurve(c.b, gExposure.w));
    if (On(F_GAMMA) && gTone.x != 1.0) c = pow(saturate(c), gTone.x);
    if (On(F_LEVELS)) {
        c = saturate((c - gLevels.x) / max(gLevels.y - gLevels.x, 1e-3));
        c = gLevels.z + c * (gLevels.w - gLevels.z);
    }
    if (On(F_LGG)) c = pow(saturate(gGain.rgb * (c + gLift.rgb * (1.0 - c))), gInvGamma.rgb);
    if (On(F_GRADING)) {
        float l = Luma(c) + gGradeShadow.w * 0.3;
        float ws = 1.0 - smoothstep(0.0, 1.0, l);
        float wh = smoothstep(0.0, 1.0, l);
        c += gGradeShadow.rgb * ws + gGradeHigh.rgb * wh + gGradeAmbient.rgb;
    }
    if (On(F_LUT)) {
        float3 lc = gLut.SampleLevel(gLinear, saturate(c) * gLutParams.y + gLutParams.z, 0).rgb;
        c = lerp(c, lc, gLutParams.x);
    }

    // ---- Finishing effects ----
    if (On(F_VIGNETTE)) {
        float2 d = (uv - 0.5) * float2(gVignette.w, 1.0);
        float r = length(d) / length(float2(gVignette.w, 1.0) * 0.5);
        float v = smoothstep(gVignette.y, gVignette.y + gVignette.z, r);
        c *= 1.0 - gVignette.x * v;
    }
    if (On(F_GRAIN)) {
        float2 gp = float2(id.xy) / gGrain.y;
        float2 ip = floor(gp), fp = frac(gp);
        fp = fp * fp * (3.0 - 2.0 * fp);
        float seed = float(gFrame % 1024u) * 17.31;
        float a = Hash12(ip + seed), b = Hash12(ip + float2(1, 0) + seed);
        float cc = Hash12(ip + float2(0, 1) + seed), d = Hash12(ip + float2(1, 1) + seed);
        float n = lerp(lerp(a, b, fp.x), lerp(cc, d, fp.x), fp.y) * 2.0 - 1.0;
        float l = saturate(Luma(c));
        c += n * gGrain.x * 0.09 * (0.3 + 2.8 * l * (1.0 - l));
    }
    if (gDither != 0) {  // triangular dither: hides banding after strong grading of 8-bit data
        float r1 = Hash12(float2(id.xy) + float(gFrame % 64u) * 3.7);
        float r2 = Hash12(float2(id.yx) + 11.3 + float(gFrame % 64u) * 1.3);
        c += (r1 + r2 - 1.0) / 255.0;
    }
    gOut[id.xy] = float4(saturate(c), 1);
}
