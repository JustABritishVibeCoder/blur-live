#include <windows.h>

#include "core/log.h"
#include "core/util.h"
#include "ui/app.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // Single instance: bring the existing window forward instead of starting a second capture app.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\BlurLive.SingleInstance");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND existing = FindWindowW(L"BlurLiveMain", nullptr)) {
      ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(existing);
    }
    CloseHandle(mutex);
    return 0;
  }

  bl::logInit(bl::appDataDir() + L"\\blurlive.log");
  int rc = 0;
  {
    bl::ui::App app;
    app.parseCommandLine(GetCommandLineW());  // full command line (argv[0] = exe path)
    rc = app.run(instance);
  }
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  return rc;
}
