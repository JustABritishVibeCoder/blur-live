#include "filters/color_math.h"

#include <algorithm>
#include <cmath>

namespace bl::color {

Mat3 identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1}; }

Mat3 mul(const Mat3& a, const Mat3& b) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      r[size_t(i * 3 + j)] = a[size_t(i * 3)] * b[size_t(j)] + a[size_t(i * 3 + 1)] * b[size_t(3 + j)] +
                             a[size_t(i * 3 + 2)] * b[size_t(6 + j)];
  return r;
}

Vec3 mul(const Mat3& m, const Vec3& v) {
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
          m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 diag(float a, float b, float c) { return {a, 0, 0, 0, b, 0, 0, 0, c}; }

std::array<float, 2> planckianXY(float T) {
  T = std::clamp(T, 1667.0f, 25000.0f);
  const double t = T, t2 = t * t, t3 = t2 * t;
  double x = T <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                         : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
  double x2 = x * x, x3 = x2 * x;
  double y = T <= 2222.0   ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
             : T <= 4000.0 ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
                           : 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
  return {float(x), float(y)};
}

float temperatureToKelvin(float temperature) {
  // +1 = 4000 K look (warm), -1 = ~10500 K look (cool), 0 = 6500 K (D65, identity).
  return 6504.0f * std::pow(2.0f, -0.7f * std::clamp(temperature, -1.0f, 1.0f));
}

Mat3 whiteBalanceMatrix(float temperature, float tint) {
  if (temperature == 0.0f && tint == 0.0f) return identity();
  static const Mat3 kRgbToXyz = {0.4124564f, 0.3575761f, 0.1804375f, 0.2126729f, 0.7151522f,
                                 0.0721750f, 0.0193339f, 0.1191920f, 0.9503041f};
  static const Mat3 kXyzToRgb = {3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
                                 0.0415560f, 0.0556434f,  -0.2040259f, 1.0572252f};
  static const Mat3 kBradford = {0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f, -0.0685f, 1.0296f};
  static const Mat3 kBradfordInv = {0.9869929f, -0.1470543f, 0.1599627f, 0.4323053f, 0.5183603f,
                                    0.0492912f, -0.0085287f, 0.0400428f, 0.9684867f};
  auto xyToXYZ = [](float x, float y) { return Vec3{x / y, 1.0f, (1.0f - x - y) / y}; };

  // Destination white: black-body chromaticity shifted towards magenta/green by tint.
  auto d65 = std::array<float, 2>{0.31271f, 0.32902f};
  auto bb = planckianXY(temperatureToKelvin(temperature));
  auto bb65 = planckianXY(6504.0f);  // keep the D65 offset from the locus so temperature 0 == D65 exactly
  float x = bb[0] + (d65[0] - bb65[0]);
  float y = bb[1] + (d65[1] - bb65[1]) - 0.015f * std::clamp(tint, -1.0f, 1.0f);

  Vec3 src = mul(kBradford, xyToXYZ(d65[0], d65[1]));
  Vec3 dst = mul(kBradford, xyToXYZ(x, y));
  // We want D65-white content to come out as the destination white: scale cone responses dst/src.
  Mat3 adapt = mul(kBradfordInv, mul(diag(dst[0] / src[0], dst[1] / src[1], dst[2] / src[2]), kBradford));
  Mat3 m = mul(kXyzToRgb, mul(adapt, kRgbToXyz));
  // Preserve the luminance of white.
  Vec3 white = mul(m, Vec3{1, 1, 1});
  float Y = 0.2126729f * white[0] + 0.7151522f * white[1] + 0.0721750f * white[2];
  for (float& v : m) v /= Y;
  return m;
}

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) {
  v = std::max(v, 0.0f);
  return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

Vec3 linearToOklab(const Vec3& c) {
  float l = 0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2];
  float m = 0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2];
  float s = 0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2];
  l = std::cbrt(l);
  m = std::cbrt(m);
  s = std::cbrt(s);
  return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
          1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
          0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

Vec3 oklabToLinear(const Vec3& lab) {
  float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
  float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
  float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
  l = l * l * l;
  m = m * m * m;
  s = s * s * s;
  return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
          -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
          -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
}

Vec3 oklchToSrgb(float L, float C, float hueTurns) {
  float h = hueTurns * 6.28318531f;
  Vec3 lin = oklabToLinear({L, C * std::cos(h), C * std::sin(h)});
  return {std::clamp(linearToSrgb(lin[0]), 0.0f, 1.0f), std::clamp(linearToSrgb(lin[1]), 0.0f, 1.0f),
          std::clamp(linearToSrgb(lin[2]), 0.0f, 1.0f)};
}

Vec3 tintOffset(float hueTurns) {
  Vec3 c = oklchToSrgb(0.75f, 0.12f, hueTurns);
  float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
  return {c[0] - y, c[1] - y, c[2] - y};
}

Mat3 cvdSimulation(int type) {
  switch (type) {
    case 0:
      return {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f};
    case 2:
      return {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.148084f, 0.004733f, 0.691367f, 0.303900f};
    default:
      return {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f};
  }
}

}  // namespace bl::color
