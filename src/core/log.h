#pragma once
#include <format>
#include <string>
#include <vector>

namespace bl {

enum class LogLevel { Info, Warn, Error };

struct LogEntry {
  LogLevel level;
  std::string text;
  std::string timestamp;  // HH:MM:SS
};

// Opens the log file (truncated each launch). Safe to call once; logging works without it.
void logInit(const std::wstring& filePath);
void logWrite(LogLevel level, std::string message);
std::vector<LogEntry> logSnapshot(size_t maxEntries = 400);

template <class... A>
void logInfo(std::format_string<A...> f, A&&... a) { logWrite(LogLevel::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void logWarn(std::format_string<A...> f, A&&... a) { logWrite(LogLevel::Warn, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void logError(std::format_string<A...> f, A&&... a) { logWrite(LogLevel::Error, std::format(f, std::forward<A>(a)...)); }

}  // namespace bl
