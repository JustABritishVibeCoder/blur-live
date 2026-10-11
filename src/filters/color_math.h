#pragma once
#include <array>

namespace bl::color {

using Vec3 = std::array<float, 3>;
using Mat3 = std::array<float, 9>;  // row-major

Mat3 identity();
Mat3 mul(const Mat3& a, const Mat3& b);
Vec3 mul(const Mat3& m, const Vec3& v);
Mat3 diag(float a, float b, float c);

// CIE xy chromaticity of a black-body radiator (Kim et al. cubic approximation, 1667 K .. 25000 K).
std::array<float, 2> planckianXY(float kelvin);

// Linear-sRGB matrix that re-renders a D65-white image as if lit by a warmer/cooler/tinted white.
// temperature/tint in [-1, 1] (0 = identity). Luminance of white is preserved.
Mat3 whiteBalanceMatrix(float temperature, float tint);
float temperatureToKelvin(float temperature);  // for display

// sRGB transfer function.
float srgbToLinear(float v);
float linearToSrgb(float v);

// OkLab / OkLCh (Bjorn Ottosson). Hue in turns [0, 1).
Vec3 linearToOklab(const Vec3& rgb);
Vec3 oklabToLinear(const Vec3& lab);
Vec3 oklchToSrgb(float L, float C, float hueTurns);  // display (gamma-encoded) colour, clipped to [0,1]

// A zero-luma colour offset in display space for tinting towards `hueTurns` (luma preserved).
Vec3 tintOffset(float hueTurns);

// Machado et al. 2009 colour-vision-deficiency simulation matrices (linear RGB, severity 1).
// type: 0 protan, 1 deutan, 2 tritan.
Mat3 cvdSimulation(int type);

}  // namespace bl::color
