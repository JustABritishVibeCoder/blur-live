#include "core/weighting.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace bl {

const char* weightingName(Weighting w) {
  switch (w) {
    case Weighting::Equal: return "Equal";
    case Weighting::Gaussian: return "Gaussian";
    case Weighting::GaussianSym: return "Gaussian (symmetric)";
    case Weighting::Pyramid: return "Pyramid";
    case Weighting::Ascending: return "Ascending";
    case Weighting::Descending: return "Descending";
    case Weighting::Vegas: return "Vegas";
    case Weighting::Custom: return "Custom";
    default: return "?";
  }
}

const char* weightingDescription(Weighting w) {
  switch (w) {
    case Weighting::Equal: return "Every moment of the shutter counts equally. Blur's default; a true box shutter.";
    case Weighting::Gaussian: return "Bell curve placed with mean/bound. Blur's defaults peak at the newest moment, giving a fading trail.";
    case Weighting::GaussianSym: return "Bell curve centred on the window. Soft start and end, no hard edges on streaks.";
    case Weighting::Pyramid: return "Linear ramp up to the middle of the window and back down.";
    case Weighting::Ascending: return "Newer moments weigh more. Feels the most responsive (trail fades behind objects).";
    case Weighting::Descending: return "Older moments weigh more. Heavy smear, increases perceived lag.";
    case Weighting::Vegas: return "1,2,2,...,2,1 for even counts: an equal shutter with half-weight endpoints.";
    case Weighting::Custom: return "Your own comma-separated weights, stretched across the window (oldest first).";
    default: return "";
  }
}

bool parseCustomWeights(const std::string& s, std::vector<double>& out) {
  out.clear();
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    auto b = tok.find_first_not_of(" \t");
    auto e = tok.find_last_not_of(" \t");
    if (b == std::string::npos) return false;
    tok = tok.substr(b, e - b + 1);
    try {
      size_t used = 0;
      double v = std::stod(tok, &used);
      if (used != tok.size() || !std::isfinite(v)) return false;
      out.push_back(v);
    } catch (...) {
      return false;
    }
  }
  return !out.empty();
}

namespace {

std::vector<double> scaleRange(int n, double a, double b) {
  std::vector<double> r(size_t(std::max(n, 0)), a);
  if (n > 1)
    for (int i = 0; i < n; ++i) r[size_t(i)] = a + double(i) * (b - a) / double(n - 1);
  return r;
}

std::vector<float> normalize(std::vector<double> w) {
  if (w.empty()) return {};
  double mn = *std::min_element(w.begin(), w.end());
  if (mn < 0)
    for (double& x : w) x = x - mn + 1.0;
  double sum = std::accumulate(w.begin(), w.end(), 0.0);
  std::vector<float> out(w.size());
  if (!(sum > 0) || !std::isfinite(sum)) {
    std::fill(out.begin(), out.end(), 1.0f / float(w.size()));
    return out;
  }
  for (size_t i = 0; i < w.size(); ++i) out[i] = float(w[i] / sum);
  return out;
}

}  // namespace

std::vector<float> computeWeights(int n, const WeightingParams& p, std::string* error) {
  if (n <= 0) return {};
  std::vector<double> w(size_t(n), 1.0);
  const double sd = std::max(1e-3, double(p.gaussStdDev));
  switch (p.type) {
    case Weighting::Equal: break;
    case Weighting::Gaussian: {
      auto x = scaleRange(n, p.gaussBoundLo, p.gaussBoundHi);
      for (int i = 0; i < n; ++i) {
        double d = x[size_t(i)] - p.gaussMean;
        w[size_t(i)] = std::exp(-(d * d) / (2.0 * sd * sd));
      }
      break;
    }
    case Weighting::GaussianSym: {
      double m = std::max(std::abs(double(p.gaussBoundLo)), std::abs(double(p.gaussBoundHi)));
      auto x = scaleRange(n, -m, m);
      for (int i = 0; i < n; ++i) w[size_t(i)] = std::exp(-(x[size_t(i)] * x[size_t(i)]) / (2.0 * sd * sd));
      break;
    }
    case Weighting::Pyramid: {
      double h = double(n - 1) / 2.0;
      for (int i = 0; i < n; ++i) w[size_t(i)] = h - std::abs(double(i) - h) + 1.0;
      break;
    }
    case Weighting::Ascending:
      for (int i = 0; i < n; ++i) w[size_t(i)] = double(i + 1);
      break;
    case Weighting::Descending:
      for (int i = 0; i < n; ++i) w[size_t(i)] = double(n - i);
      break;
    case Weighting::Vegas:
      if (n % 2 == 0 && n >= 2) {
        for (int i = 0; i < n; ++i) w[size_t(i)] = 2.0;
        w.front() = 1.0;
        w.back() = 1.0;
      }
      break;
    case Weighting::Custom: {
      std::vector<double> c;
      if (!parseCustomWeights(p.custom, c)) {
        if (error) *error = "Custom weights must be a comma-separated list of numbers, e.g. 1,2,3,2,1";
        break;  // equal fallback
      }
      // Stretch the custom list across n frames.
      for (int i = 0; i < n; ++i) w[size_t(i)] = c[size_t(i) * c.size() / size_t(n)];
      break;
    }
    default: break;
  }
  return normalize(std::move(w));
}

std::array<float, kWeightLutSize> buildWeightLut(const WeightingParams& p) {
  std::array<float, kWeightLutSize> lut{};
  auto w = computeWeights(kWeightLutSize, p);
  for (int i = 0; i < kWeightLutSize; ++i) lut[size_t(i)] = w[size_t(i)] * float(kWeightLutSize);
  return lut;
}

}  // namespace bl
