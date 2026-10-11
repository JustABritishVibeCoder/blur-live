#pragma once
#include <array>
#include <string>
#include <vector>

namespace bl {

// Temporal weighting functions for the shutter integral. The shapes mirror the options offered
// by f0e/blur (equal, gaussian, gaussian_sym, pyramid, ascending, descending, vegas, custom),
// reimplemented here from their mathematical definitions. Index 0 / tau 0 is the OLDEST point
// of the shutter window, the last index / tau 1 is the NEWEST.
enum class Weighting { Equal = 0, Gaussian, GaussianSym, Pyramid, Ascending, Descending, Vegas, Custom, Count };

struct WeightingParams {
  Weighting type = Weighting::Equal;
  float gaussStdDev = 1.0f;
  float gaussMean = 2.0f;
  float gaussBoundLo = 0.0f;
  float gaussBoundHi = 2.0f;
  std::string custom = "1,2,3,2,1";
  bool operator==(const WeightingParams&) const = default;
};

const char* weightingName(Weighting w);
const char* weightingDescription(Weighting w);

// n discrete weights summing to 1. Invalid custom strings fall back to equal weights and set *error.
std::vector<float> computeWeights(int n, const WeightingParams& p, std::string* error = nullptr);

// Continuous weight function over tau in [0,1], sampled at 64 points and scaled so its mean is 1
// (i.e. it integrates to 1 over the window). Consumed by the synthesis shader.
constexpr int kWeightLutSize = 64;
std::array<float, kWeightLutSize> buildWeightLut(const WeightingParams& p);

// Parses "1, 2, 3" into numbers. Returns false on malformed input.
bool parseCustomWeights(const std::string& s, std::vector<double>& out);

}  // namespace bl
