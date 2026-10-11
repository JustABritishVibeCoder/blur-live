#include "capture/window_list.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwctype>

namespace bl {

namespace {

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = wchar_t(std::towlower(c));
  return s;
}

bool isCloaked(HWND h) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

std::wstring processPath(DWORD pid) {
  std::wstring out;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return out;
  wchar_t buf[MAX_PATH * 2];
  DWORD n = DWORD(std::size(buf));
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
  CloseHandle(p);
  return out;
}

// Processes that are never games; keeps the default list focused.
const wchar_t* kNonGames[] = {
    L"explorer.exe", L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"code.exe",
    L"devenv.exe", L"discord.exe", L"slack.exe", L"teams.exe", L"ms-teams.exe", L"spotify.exe", L"obs64.exe",
    L"steamwebhelper.exe", L"epicgameslauncher.exe", L"textinputhost.exe", L"applicationframehost.exe",
    L"systemsettings.exe", L"searchhost.exe", L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
    L"lockapp.exe", L"windowsterminal.exe", L"notepad.exe", L"claude.exe", L"lghub.exe", L"nvidia app.exe",
    L"nvcontainer.exe", L"tiktok live studio.exe", L"blurlive.exe"};

struct EngineHint {
  const wchar_t* className;
  const char* hint;
};
const EngineHint kEngineClasses[] = {
    {L"UnrealWindow", "Unreal Engine"}, {L"UnityWndClass", "Unity"}, {L"SDL_app", "SDL"},
    {L"Valve001", "Source engine"}, {L"SDL2", "SDL"}, {L"CryENGINE", "CryEngine"},
    {L"LaunchUnrealUWindowsClient", "Unreal Engine"}, {L"GLFW30", "GLFW"}, {L"Riot Window Class", "Riot"},
    {L"grcWindow", "RAGE"}, {L"Godot_Engine", "Godot"}, {L"RobloxApp", "Roblox"}, {L"WINDOWSCLIENT", "Roblox"},
};

const struct {
  const wchar_t* fragment;
  const char* hint;
} kGamePaths[] = {
    {L"\\steamapps\\", "Steam library"}, {L"\\epic games\\", "Epic Games"}, {L"\\riot games\\", "Riot Games"},
    {L"\\ubisoft", "Ubisoft"}, {L"\\rockstar games\\", "Rockstar"}, {L"\\ea games\\", "EA"},
    {L"\\gog galaxy\\games\\", "GOG"}, {L"\\gog games\\", "GOG"}, {L"\\xboxgames\\", "Xbox"},
    {L"\\battle.net\\", "Battle.net"}, {L"\\blizzard", "Blizzard"}, {L"\\roblox\\", "Roblox"},
    {L"\\minecraft", "Minecraft"}, {L"\\games\\", "Games folder"},
};

struct EnumCtx {
  DWORD excludePid;
  std::vector<WindowInfo>* out;
};

BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
  auto* ctx = reinterpret_cast<EnumCtx*>(lp);
  if (!IsWindowVisible(h) || isCloaked(h)) return TRUE;
  if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
  LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  if (ex & WS_EX_TOOLWINDOW) return TRUE;

  WindowInfo w;
  w.hwnd = h;
  GetWindowThreadProcessId(h, &w.pid);
  if (w.pid == ctx->excludePid) return TRUE;

  wchar_t title[512];
  int n = GetWindowTextW(h, title, int(std::size(title)));
  if (n <= 0) return TRUE;
  w.title.assign(title, size_t(n));
  wchar_t cls[256];
  n = GetClassNameW(h, cls, int(std::size(cls)));
  w.className.assign(cls, size_t(std::max(0, n)));
  if (w.className == L"Progman" || w.className == L"Shell_TrayWnd") return TRUE;

  w.minimized = IsIconic(h) != FALSE;
  w.rect = windowVisualRect(h);
  int ww = w.rect.right - w.rect.left, wh = w.rect.bottom - w.rect.top;
  if (!w.minimized && (ww < 64 || wh < 64)) return TRUE;

  w.exePath = processPath(w.pid);
  auto slash = w.exePath.find_last_of(L"\\/");
  w.exeName = slash == std::wstring::npos ? w.exePath : w.exePath.substr(slash + 1);
  std::wstring exeLower = lower(w.exeName), pathLower = lower(w.exePath);

  bool nonGame = false;
  for (auto* ng : kNonGames)
    if (exeLower == ng) nonGame = true;

  if (!nonGame) {
    for (auto& e : kEngineClasses)
      if (w.className.rfind(e.className, 0) == 0) {
        w.likelyGame = true;
        w.gameHint = e.hint;
        break;
      }
    if (!w.likelyGame)
      for (auto& g : kGamePaths)
        if (pathLower.find(g.fragment) != std::wstring::npos) {
          w.likelyGame = true;
          w.gameHint = g.hint;
          break;
        }
  }
  if (!w.minimized) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
      w.coversMonitor = w.rect.left <= mi.rcMonitor.left && w.rect.top <= mi.rcMonitor.top &&
                        w.rect.right >= mi.rcMonitor.right && w.rect.bottom >= mi.rcMonitor.bottom;
      if (w.coversMonitor && !nonGame && !w.likelyGame) {
        w.likelyGame = true;
        w.gameHint = "Borderless fullscreen";
      }
    }
  }
  ctx->out->push_back(std::move(w));
  return TRUE;
}

}  // namespace

RECT windowVisualRect(HWND hwnd) {
  RECT r{};
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
  return r;
}

RECT windowClientRect(HWND hwnd) {
  RECT r{};
  if (GetClientRect(hwnd, &r)) MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
  return r;
}

bool isTargetForeground(HWND target) {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  if (fg == target || GetAncestor(fg, GA_ROOTOWNER) == target) return true;
  DWORD a = 0, b = 0;
  GetWindowThreadProcessId(fg, &a);
  GetWindowThreadProcessId(target, &b);
  return a != 0 && a == b;
}

std::vector<WindowInfo> enumerateCaptureTargets(DWORD excludePid, bool includeAll) {
  std::vector<WindowInfo> out;
  EnumCtx ctx{excludePid, &out};
  EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));
  if (!includeAll) {
    // Hide well-known non-game apps unless the user asks for all windows.
    std::erase_if(out, [](const WindowInfo& w) {
      std::wstring e = lower(w.exeName);
      for (auto* ng : kNonGames)
        if (e == ng) return true;
      return false;
    });
  }
  std::stable_sort(out.begin(), out.end(), [](const WindowInfo& a, const WindowInfo& b) {
    if (a.likelyGame != b.likelyGame) return a.likelyGame;
    if (a.minimized != b.minimized) return !a.minimized;
    return false;
  });
  return out;
}

namespace {
// GDI device name ("\\.\DISPLAY1") -> monitor's EDID friendly name, via the DisplayConfig API.
std::wstring monitorFriendlyName(const std::wstring& gdiName) {
  UINT32 nPaths = 0, nModes = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) return {};
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) != ERROR_SUCCESS)
    return {};
  for (UINT32 i = 0; i < nPaths; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = paths[i].sourceInfo.adapterId;
    src.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || gdiName != src.viewGdiDeviceName) continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
    tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tgt.header.size = sizeof(tgt);
    tgt.header.adapterId = paths[i].targetInfo.adapterId;
    tgt.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) return tgt.monitorFriendlyDeviceName;
  }
  return {};
}
}  // namespace

std::vector<MonitorInfo> enumerateMonitors() {
  std::vector<MonitorInfo> out;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* v = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(m, &mi)) return TRUE;
        MonitorInfo info;
        info.handle = m;
        info.rect = mi.rcMonitor;
        info.name = mi.szDevice;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) info.refreshHz = int(dm.dmDisplayFrequency);
        v->push_back(info);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  for (auto& m : out) m.friendlyName = monitorFriendlyName(m.name);
  std::stable_sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) { return a.primary && !b.primary; });
  return out;
}

}  // namespace bl
