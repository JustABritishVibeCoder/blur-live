#include "filters/lut.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace bl {

bool parseCubeLut(const std::string& text, CubeLut& out, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  CubeLut lut;
  std::istringstream in(text);
  std::string line;
  size_t count = 0;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    line = line.substr(b);
    std::istringstream ls(line);
    if (std::isalpha(static_cast<unsigned char>(line[0]))) {
      std::string key;
      ls >> key;
      if (key == "TITLE") {
        auto q1 = line.find('"'), q2 = line.rfind('"');
        if (q1 != std::string::npos && q2 > q1) lut.title = line.substr(q1 + 1, q2 - q1 - 1);
      } else if (key == "LUT_3D_SIZE") {
        ls >> lut.size;
        if (lut.size < 2 || lut.size > 128) return fail("LUT_3D_SIZE must be between 2 and 128.");
        lut.rgba.assign(size_t(lut.size) * lut.size * lut.size * 4, 1.0f);
      } else if (key == "LUT_1D_SIZE") {
        return fail("1D LUTs are not supported; use a 3D .cube LUT.");
      } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
        float a = 0, b2 = 0, c = 0;
        ls >> a >> b2 >> c;
        float expect = key == "DOMAIN_MIN" ? 0.0f : 1.0f;
        if (std::abs(a - expect) > 1e-4f || std::abs(b2 - expect) > 1e-4f || std::abs(c - expect) > 1e-4f)
          return fail("Only LUTs with a 0..1 input domain are supported.");
      }
      // Unknown keywords (LUT_3D_INPUT_RANGE etc.) are ignored.
      continue;
    }
    if (lut.size == 0) return fail("Data found before LUT_3D_SIZE (line " + std::to_string(lineNo) + ").");
    float r, g, bl;
    if (!(ls >> r >> g >> bl)) return fail("Malformed data on line " + std::to_string(lineNo) + ".");
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(bl)) return fail("Non-finite value in LUT data.");
    size_t total = size_t(lut.size) * lut.size * lut.size;
    if (count >= total) return fail("Too many data lines for LUT_3D_SIZE.");
    lut.rgba[count * 4 + 0] = r;
    lut.rgba[count * 4 + 1] = g;
    lut.rgba[count * 4 + 2] = bl;
    lut.rgba[count * 4 + 3] = 1.0f;
    ++count;
  }
  if (lut.size == 0) return fail("No LUT_3D_SIZE found: not a 3D .cube file.");
  if (count != size_t(lut.size) * lut.size * lut.size)
    return fail("Expected " + std::to_string(size_t(lut.size) * lut.size * lut.size) + " entries, found " +
                std::to_string(count) + ".");
  out = std::move(lut);
  return true;
}

bool loadCubeLut(const std::wstring& path, CubeLut& out, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "Could not open the LUT file.";
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return parseCubeLut(ss.str(), out, error);
}

}  // namespace bl
