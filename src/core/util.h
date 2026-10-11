#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace bl {

std::string toUtf8(std::wstring_view w);
std::wstring toWide(std::string_view s);

// %APPDATA%\BlurLive (created on first use).
std::wstring appDataDir();

// "0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)" style description.
std::string hrToString(long hr);

// High-resolution clock helpers (QueryPerformanceCounter based).
int64_t qpcTicks();
double qpcFrequency();
inline double qpcSeconds() { return double(qpcTicks()) / qpcFrequency(); }
// QPC time expressed in 100 ns units, the same timebase as WGC SystemRelativeTime.
inline double qpcSecondsFrom100ns(int64_t t100ns) { return double(t100ns) * 1e-7; }

}  // namespace bl
