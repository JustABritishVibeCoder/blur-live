#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace bl {

struct WindowInfo {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  std::wstring title;
  std::wstring exeName;   // e.g. "cs2.exe"
  std::wstring exePath;
  std::wstring className;
  RECT rect{};            // DWM visual bounds (what WGC captures), physical pixels
  bool likelyGame = false;
  bool coversMonitor = false;  // borderless-fullscreen style window
  bool minimized = false;
  std::string gameHint;   // why we think it's a game ("Steam library", "Unreal Engine", ...)
};

// Top-level windows that Windows Graphics Capture can capture. Games are sorted first.
// excludePid: our own process (never offer to capture ourselves).
std::vector<WindowInfo> enumerateCaptureTargets(DWORD excludePid, bool includeAll);

// Visual bounds of a window in screen coordinates (DWMWA_EXTENDED_FRAME_BOUNDS).
RECT windowVisualRect(HWND hwnd);
// Client area (no title bar / borders) in screen coordinates.
RECT windowClientRect(HWND hwnd);

// True if `hwnd` (or a window owned by the same process) is the foreground window.
bool isTargetForeground(HWND target);

struct MonitorInfo {
  HMONITOR handle = nullptr;
  RECT rect{};
  std::wstring name;   // "\\.\DISPLAY1"
  std::wstring friendlyName;  // "AW2523HF" (EDID name), empty if unknown
  bool primary = false;
  int refreshHz = 0;
};
std::vector<MonitorInfo> enumerateMonitors();

}  // namespace bl
