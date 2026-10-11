#pragma once
#include <string>
#include <vector>

namespace bl {

// 3D colour look-up table in Adobe/Resolve .cube format.
struct CubeLut {
  std::string title;
  int size = 0;                 // N (N x N x N entries)
  std::vector<float> rgba;      // N^3 * 4, red varies fastest (matches a Texture3D x/y/z layout)
};

// Parses a .cube file (LUT_3D_SIZE 2..128, DOMAIN_MIN 0 0 0 / DOMAIN_MAX 1 1 1). 1D LUTs and
// non-unit domains are rejected with a message.
bool loadCubeLut(const std::wstring& path, CubeLut& out, std::string* error);
bool parseCubeLut(const std::string& text, CubeLut& out, std::string* error);

}  // namespace bl
