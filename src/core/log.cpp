#include "core/log.h"

#include <windows.h>

#include <share.h>

#include <cstdio>
#include <deque>
#include <mutex>

namespace bl {

namespace {
std::mutex g_mutex;
std::deque<LogEntry> g_entries;
FILE* g_file = nullptr;
constexpr size_t kMaxEntries = 2000;
}  // namespace

void logInit(const std::wstring& filePath) {
  std::lock_guard lock(g_mutex);
  if (g_file) return;
  // Shared so the log can be opened in an editor while Blur Live is running.
  g_file = _wfsopen(filePath.c_str(), L"w", _SH_DENYNO);
}

void logWrite(LogLevel level, std::string message) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::string ts = std::format("{:02}:{:02}:{:02}", st.wHour, st.wMinute, st.wSecond);
  const char* tag = level == LogLevel::Error ? "ERROR" : level == LogLevel::Warn ? "WARN " : "INFO ";

  std::lock_guard lock(g_mutex);
  if (g_file) {
    std::fprintf(g_file, "[%s] %s %s\n", ts.c_str(), tag, message.c_str());
    std::fflush(g_file);
  }
  OutputDebugStringA(std::format("[BlurLive] {} {}\n", tag, message).c_str());
  g_entries.push_back({level, std::move(message), std::move(ts)});
  while (g_entries.size() > kMaxEntries) g_entries.pop_front();
}

std::vector<LogEntry> logSnapshot(size_t maxEntries) {
  std::lock_guard lock(g_mutex);
  size_t n = std::min(maxEntries, g_entries.size());
  return std::vector<LogEntry>(g_entries.end() - std::ptrdiff_t(n), g_entries.end());
}

}  // namespace bl
