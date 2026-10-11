#include "core/util.h"

#include <windows.h>
#include <shlobj.h>

#include <format>

namespace bl {

std::string toUtf8(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring toWide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

std::wstring appDataDir() {
  static std::wstring cached;
  if (!cached.empty()) return cached;
  PWSTR path = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &path))) {
    dir = path;
    CoTaskMemFree(path);
  } else {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    dir = tmp;
  }
  dir += L"\\BlurLive";
  CreateDirectoryW(dir.c_str(), nullptr);
  cached = dir;
  return cached;
}

std::string hrToString(long hr) {
  const char* name = nullptr;
  switch (static_cast<uint32_t>(hr)) {
    case 0x887A0005: name = "DXGI_ERROR_DEVICE_REMOVED"; break;
    case 0x887A0006: name = "DXGI_ERROR_DEVICE_HUNG"; break;
    case 0x887A0007: name = "DXGI_ERROR_DEVICE_RESET"; break;
    case 0x887A0001: name = "DXGI_ERROR_INVALID_CALL"; break;
    case 0x887A0004: name = "DXGI_ERROR_UNSUPPORTED"; break;
    case 0x80070005: name = "E_ACCESSDENIED"; break;
    case 0x80070057: name = "E_INVALIDARG"; break;
    case 0x80004005: name = "E_FAIL"; break;
    case 0x8007000E: name = "E_OUTOFMEMORY"; break;
    case 0x80004002: name = "E_NOINTERFACE"; break;
    case 0x80070578: name = "ERROR_INVALID_WINDOW_HANDLE"; break;
    default: break;
  }
  if (name) return std::format("0x{:08X} ({})", static_cast<uint32_t>(hr), name);
  return std::format("0x{:08X}", static_cast<uint32_t>(hr));
}

int64_t qpcTicks() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

double qpcFrequency() {
  static const double f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return double(v.QuadPart);
  }();
  return f;
}

}  // namespace bl
