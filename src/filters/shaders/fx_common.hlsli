#ifndef BLURLIVE_FX_COMMON_HLSLI
#define BLURLIVE_FX_COMMON_HLSLI

// Rec.709 luma weights. Used on gamma-encoded values as a perceptual luma (like video luma Y').
static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);
float Luma(float3 c) { return dot(c, kLuma709); }

// Exact sRGB transfer functions (piecewise), so decode -> encode round-trips 8-bit values exactly.
float3 SrgbToLinear(float3 c) {
    c = saturate(c);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}
float3 LinearToSrgb(float3 c) {
    c = max(c, 0.0);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

float Cbrt(float x) { return sign(x) * pow(abs(x), 1.0 / 3.0); }

// OkLab (Bjorn Ottosson), from/to linear sRGB.
float3 LinearToOklab(float3 c) {
    float l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    float m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    float s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    l = Cbrt(l); m = Cbrt(m); s = Cbrt(s);
    return float3(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                  1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                  0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
}
float3 OklabToLinear(float3 lab) {
    float l = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    l = l * l * l; m = m * m * m; s = s * s * s;
    return float3(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                  -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                  -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

// Brings an out-of-gamut linear colour back into [0,1] by moving it towards grey of the same
// luminance (keeps hue and lightness, reduces chroma only as much as needed).
float3 GamutCompress(float3 c) {
    float y = saturate(dot(c, float3(0.2126729, 0.7151522, 0.0721750)));
    float t = 1.0;
    [unroll] for (int i = 0; i < 3; ++i) {
        float d = c[i] - y;
        if (c[i] > 1.0 && d > 1e-6) t = min(t, (1.0 - y) / d);
        if (c[i] < 0.0 && d < -1e-6) t = min(t, (0.0 - y) / d);
    }
    return y + (c - y) * saturate(t);
}

float Hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

static const float kPi = 3.14159265;
static const float kTwoPi = 6.28318531;
float HueDistance(float a, float b) {  // radians, result in [0, pi]
    float d = abs(a - b);
    return min(d, kTwoPi - d);
}

#endif
