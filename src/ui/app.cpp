#include "ui/app.h"

#include <dwmapi.h>
#include <dxgi1_6.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

#include "capture/wgc_capture.h"
#include "core/log.h"
#include "core/util.h"
#include "ui/theme.h"
#include "ui/widgets.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace bl::ui {

namespace {
constexpr int kHotkeyBlur = 1;
constexpr int kHotkeyCapture = 2;
constexpr int kHotkeyEffects = 3;
constexpr int kHotkeyCompare = 4;

const char* kPageNames[] = {"Dashboard", "Sources",     "Filters",  "Motion Blur",
                            "Output",    "Performance", "Settings", "Help"};
const char* kPageSubtitles[] = {
    "Live status and the controls you need while playing",
    "Pick the game or window to capture",
    "Independent real-time filters, presets and a live preview",
    "How the blur looks: algorithm, shutter, weighting and quality",
    "Where the processed gameplay is shown",
    "Measured timings straight from the pipeline",
    "Capture, robustness and application options",
    "How Blur Live works, its limits, and how to fix common problems",
};
static_assert(std::size(kPageNames) == size_t(Page::Count));
const char* kPageIcons[] = {icon::home,    icon::game,  icon::sliders,  icon::blur,
                            icon::monitor, icon::speed, icon::settings, icon::help};

std::string fmt(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof(b), f, v);
  return b;
}

std::wstring lowerW(std::wstring s) {
  for (auto& c : s) c = wchar_t(std::towlower(c));
  return s;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Window / device plumbing
// ---------------------------------------------------------------------------------------------

LRESULT CALLBACK App::wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
  auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    self = static_cast<App*>(cs->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    // Must be set before CreateWindowEx returns: messages sent during creation (WM_NCCALCSIZE, which
    // sizes the title bar and borders) are forwarded to DefWindowProc with m_hwnd.
    self->m_hwnd = h;
  }
  if (self) return self->handleMessage(msg, w, l);
  return DefWindowProcW(h, msg, w, l);
}

LRESULT App::handleMessage(UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_SIZE:
      if (w != SIZE_MINIMIZED) {
        m_resizeW = LOWORD(l);
        m_resizeH = HIWORD(l);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
      mmi->ptMinTrackSize = {LONG(980 * m_dpi), LONG(660 * m_dpi)};
      return 0;
    }
    case WM_DPICHANGED: {
      m_dpi = float(HIWORD(w)) / 96.0f;
      m_dpiChanged = true;
      auto* r = reinterpret_cast<RECT*>(l);
      SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_HOTKEY:
      if (w == kHotkeyBlur) {
        m_settings.blurEnabled = !m_settings.blurEnabled;
        logInfo("Hotkey: motion blur {}", m_settings.blurEnabled ? "on" : "off");
      } else if (w == kHotkeyEffects) {
        m_settings.effectsEnabled = !m_settings.effectsEnabled;
        logInfo("Hotkey: all effects {}", m_settings.effectsEnabled ? "on" : "off");
      } else if (w == kHotkeyCompare) {
        m_settings.compareEnabled = !m_settings.compareEnabled;
      } else if (w == kHotkeyCapture) {
        toggleCapture();
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((w & 0xfff0) == SC_KEYMENU) return 0;  // no ALT menu beep
      break;
    case WM_CLOSE:
      m_quit = true;
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default: break;
  }
  return DefWindowProcW(m_hwnd, msg, w, l);
}

bool App::createMainWindow(HINSTANCE instance) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"BlurLiveMain";
  wc.hbrBackground = CreateSolidBrush(RGB(11, 10, 16));
  RegisterClassExW(&wc);

  POINT origin{0, 0};
  HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  UINT dx = 96, dy = 96;
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
    if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) fn(mon, 0, &dx, &dy);
  }
  m_dpi = float(dx) / 96.0f;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  int w = std::min(int(1340 * m_dpi), int(mi.rcWork.right - mi.rcWork.left) - 40);
  int h = std::min(int(880 * m_dpi), int(mi.rcWork.bottom - mi.rcWork.top) - 40);
  int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
  int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
  m_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Blur Live", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                           instance, this);
  if (!m_hwnd) return false;
  m_dpi = float(GetDpiForWindow(m_hwnd)) / 96.0f;

  BOOL dark = TRUE;
  DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
  COLORREF caption = RGB(16, 14, 23);
  DwmSetWindowAttribute(m_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
  return true;
}

bool App::createDevice() {
  D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  // Same adapter as the pipeline (high-performance GPU) so the live-preview texture can be shared.
  ComPtr<IDXGIFactory6> f6;
  ComPtr<IDXGIAdapter1> adapter;
  if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f6))))
    f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2, D3D11_SDK_VERSION, &m_dev, nullptr, &m_ctx);
  if (FAILED(hr))  // UI still works on WARP if no hardware device is usable
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2,
                           D3D11_SDK_VERSION, &m_dev, nullptr, &m_ctx);
  if (FAILED(hr)) return false;
  ComPtr<IDXGIDevice> dxgi;
  m_dev.As(&dxgi);
  ComPtr<IDXGIAdapter> ad;
  dxgi->GetAdapter(&ad);
  ComPtr<IDXGIFactory2> fac;
  ad->GetParent(IID_PPV_ARGS(&fac));
  DXGI_SWAP_CHAIN_DESC1 d{};
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  if (FAILED(fac->CreateSwapChainForHwnd(m_dev.Get(), m_hwnd, &d, nullptr, nullptr, &m_swap))) return false;
  fac->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
  createRenderTarget();
  return true;
}

void App::createRenderTarget() {
  ComPtr<ID3D11Texture2D> bb;
  m_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
  m_dev->CreateRenderTargetView(bb.Get(), nullptr, &m_rtv);
}

void App::destroyDevice() {
  m_rtv.Reset();
  m_swap.Reset();
  m_ctx.Reset();
  m_dev.Reset();
}

void App::registerHotkeys() {
  UnregisterHotKey(m_hwnd, kHotkeyBlur);
  UnregisterHotKey(m_hwnd, kHotkeyCapture);
  UnregisterHotKey(m_hwnd, kHotkeyEffects);
  UnregisterHotKey(m_hwnd, kHotkeyCompare);
  if (!m_settings.hotkeysEnabled) return;
  if (!RegisterHotKey(m_hwnd, kHotkeyBlur, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B'))
    logWarn("Hotkey Ctrl+Alt+B is already used by another application");
  if (!RegisterHotKey(m_hwnd, kHotkeyEffects, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F'))
    logWarn("Hotkey Ctrl+Alt+F is already used by another application");
  if (!RegisterHotKey(m_hwnd, kHotkeyCompare, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C'))
    logWarn("Hotkey Ctrl+Alt+C is already used by another application");
  if (!RegisterHotKey(m_hwnd, kHotkeyCapture, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S'))
    logWarn("Hotkey Ctrl+Alt+S is already used by another application");
}

// ---------------------------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------------------------

void App::parseCommandLine(const wchar_t* cmdLine) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"", &argc);
  if (!argv) return;
  static const wchar_t* kPageArgs[] = {L"dashboard", L"sources",     L"filters",  L"blur",
                                       L"output",    L"performance", L"settings", L"help"};
  static_assert(std::size(kPageArgs) == size_t(Page::Count));
  for (int i = 0; i + 1 < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--page") {
      std::wstring v = lowerW(argv[i + 1]);
      for (int p = 0; p < int(Page::Count); ++p)
        if (v == kPageArgs[p]) m_page = Page(p);
    } else if (a == L"--capture") {
      m_autoCapture = argv[i + 1];
    } else if (a == L"--preset") {
      m_startPreset = toUtf8(argv[i + 1]);
    } else if (a == L"--output") {
      std::wstring v = lowerW(argv[i + 1]);
      m_outputOverride = v == L"window" ? 1 : v == L"monitor" ? 2 : v == L"overlay" ? 0 : -1;
    }
  }
  LocalFree(argv);
}

int App::run(HINSTANCE instance) {
  loadSettings(m_settings, defaultSettingsPath());
  m_fileOutputMode = m_settings.outputMode;
  if (m_outputOverride >= 0) m_settings.outputMode = OutputMode(m_outputOverride);
  sanitize(m_settings);
  m_pushedSettings = m_settings;

  if (!createMainWindow(instance)) {
    MessageBoxW(nullptr, L"Could not create the main window.", L"Blur Live", MB_ICONERROR);
    return 1;
  }
  if (!createDevice()) {
    MessageBoxW(m_hwnd, L"Direct3D 11 is not available on this system.", L"Blur Live", MB_ICONERROR);
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplWin32_Init(m_hwnd);
  ImGui_ImplDX11_Init(m_dev.Get(), m_ctx.Get());
  applyTheme(m_dpi);
  m_icons.init(m_dev.Get());
  m_gpuMonitor.init();

  m_pipeline.setSettings(m_settings);
  if (!m_pipeline.start(&m_pipelineError)) logError("Pipeline unavailable: {}", m_pipelineError);
  if (!WgcCapture::isSupported())
    m_pipelineError = "Windows Graphics Capture is not supported on this version of Windows (needs Windows 10 1903+).";
  registerHotkeys();

  ShowWindow(m_hwnd, SW_SHOWDEFAULT);
  UpdateWindow(m_hwnd);
  m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  logInfo("Blur Live {} started", BLURLIVE_VERSION);
  if (!m_startPreset.empty()) {
    if (m_presets.find(m_startPreset)) applyPreset(m_startPreset);
    else logWarn("--preset: no preset named \"{}\"", m_startPreset);
  }
  if (!m_autoCapture.empty()) {
    refreshWindows();
    for (auto& w : enumerateCaptureTargets(GetCurrentProcessId(), true))
      if (w.title.find(m_autoCapture) != std::wstring::npos) {
        m_windows.push_back(w);
        m_selected = w.hwnd;
        // A scripted --capture must not replace the game the user picked last time.
        const std::string keepExe = m_settings.lastTargetExe, keepTitle = m_settings.lastTargetTitle;
        startSelected();
        m_settings.lastTargetExe = keepExe;
        m_settings.lastTargetTitle = keepTitle;
        break;
      }
    if (!m_selected) logWarn("--capture: no window title contains \"{}\"", toUtf8(m_autoCapture));
  }

  double nextFrame = qpcSeconds();
  while (!m_quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) m_quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (m_quit) break;

    tick();

    if (m_dpiChanged) {
      m_dpiChanged = false;
      applyTheme(m_dpi);
      ImGui_ImplDX11_InvalidateDeviceObjects();
      ImGui_ImplDX11_CreateDeviceObjects();
    }
    if (m_resizeW && m_resizeH) {
      m_rtv.Reset();
      m_swap->ResizeBuffers(0, m_resizeW, m_resizeH, DXGI_FORMAT_UNKNOWN, 0);
      m_resizeW = m_resizeH = 0;
      createRenderTarget();
    }

    // The control panel does not need to run at 280 Hz while a game is running: cap it.
    const bool foreground = GetForegroundWindow() == m_hwnd;
    const bool hidden = IsIconic(m_hwnd) || m_occluded;
    const double period = hidden ? 0.1 : foreground ? 1.0 / 120.0 : 1.0 / 30.0;
    double now = qpcSeconds();
    if (now < nextFrame && m_timer) {
      LARGE_INTEGER due;
      due.QuadPart = -LONGLONG((nextFrame - now) * 1e7);
      SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0);
      MsgWaitForMultipleObjects(1, &m_timer, FALSE, INFINITE, QS_ALLINPUT);
      continue;  // handle any messages that woke us, then render
    }
    nextFrame = std::max(nextFrame + period, now);

    if (hidden) {
      if (m_occluded && m_swap->Present(0, DXGI_PRESENT_TEST) != DXGI_STATUS_OCCLUDED) m_occluded = false;
      continue;
    }

    m_icons.beginFrame();
    openPreviewTexture();
    lockPreview();  // held only while this frame's UI draws the preview (released right after rendering)
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    drawFrame();
    ImGui::Render();
    const float clear[4] = {11 / 255.0f, 10 / 255.0f, 16 / 255.0f, 1.0f};
    m_ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_ctx->ClearRenderTargetView(m_rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    unlockPreview();
    HRESULT hr = m_swap->Present(1, 0);
    m_occluded = hr == DXGI_STATUS_OCCLUDED;
  }

  // Shutdown: stop the pipeline first (releases the game capture and overlay immediately).
  m_pipeline.shutdown();
  unlockPreview();
  m_previewSrv.Reset();
  m_previewMutex.Reset();
  m_previewTex.Reset();
  if (m_saveDirty || !(m_settings == m_pushedSettings)) persistSettings();
  UnregisterHotKey(m_hwnd, kHotkeyBlur);
  UnregisterHotKey(m_hwnd, kHotkeyCapture);
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  m_icons.clear();
  destroyDevice();
  if (m_timer) CloseHandle(m_timer);
  DestroyWindow(m_hwnd);
  logInfo("Blur Live exited");
  return 0;
}

void App::tick() {
  const double now = qpcSeconds();
  m_snap = m_pipeline.snapshot();
  // The preview copy only runs while it is actually visible.
  m_pipeline.setPreviewWanted(m_settings.livePreview && m_page == Page::Filters && !IsIconic(m_hwnd) && !m_occluded);

  sanitize(m_settings);
  if (!(m_settings == m_pushedSettings)) {
    const bool hotkeysChanged = m_settings.hotkeysEnabled != m_pushedSettings.hotkeysEnabled;
    m_pipeline.setSettings(m_settings);
    m_pushedSettings = m_settings;
    m_saveDirty = true;
    m_lastSettingsChange = now;
    if (hotkeysChanged) registerHotkeys();
  }
  if (m_saveDirty && now - m_lastSettingsChange > 1.0) {
    persistSettings();
    m_saveDirty = false;
  }
  if (now - m_lastGpuSample > 1.0) {
    m_gpuMonitor.sample();
    m_lastGpuSample = now;
  }
  const bool listVisible = m_page == Page::Sources || m_page == Page::Dashboard;
  if (now - m_lastEnum > (listVisible ? 2.0 : 10.0)) refreshWindows();
  if (now - m_lastMonitorEnum > 5.0) {
    m_monitors = enumerateMonitors();
    m_lastMonitorEnum = now;
  }
}

void App::persistSettings() {
  Settings toSave = m_settings;
  // A --output override applies to this launch only, unless the user picked a mode in the UI.
  if (m_outputOverride >= 0 && toSave.outputMode == OutputMode(m_outputOverride)) toSave.outputMode = m_fileOutputMode;
  if (!saveSettings(toSave, defaultSettingsPath())) logWarn("Could not save settings");
}

void App::refreshWindows() {
  m_windows = enumerateCaptureTargets(GetCurrentProcessId(), m_settings.showAllWindows);
  m_lastEnum = qpcSeconds();
  bool selectedAlive = false;
  for (auto& w : m_windows)
    if (w.hwnd == m_selected) selectedAlive = true;
  if (!selectedAlive) {
    m_selected = nullptr;
    // Re-select the game used last time.
    for (auto& w : m_windows)
      if (!m_settings.lastTargetExe.empty() && toUtf8(w.exeName) == m_settings.lastTargetExe) {
        m_selected = w.hwnd;
        break;
      }
  }
}

const WindowInfo* App::selectedWindow() const {
  for (auto& w : m_windows)
    if (w.hwnd == m_selected) return &w;
  return nullptr;
}

void App::startSelected() {
  const WindowInfo* w = selectedWindow();
  if (!w) {
    m_page = Page::Sources;
    return;
  }
  m_settings.lastTargetExe = toUtf8(w->exeName);
  m_settings.lastTargetTitle = toUtf8(w->title);
  m_dismissedMessage.clear();
  m_pipeline.startCapture({w->hwnd, w->title, w->exeName});
}

void App::toggleCapture() {
  if (m_snap.state == PipelineState::Running || m_snap.state == PipelineState::Starting) {
    m_pipeline.stopCapture();
  } else {
    if (!selectedWindow()) refreshWindows();
    startSelected();
  }
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

void App::drawFrame() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse);
  const float sidebarW = S(244);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + sidebarW, vp->Pos.y + vp->Size.y), col::sidebar);
  dl->AddLine(ImVec2(vp->Pos.x + sidebarW, vp->Pos.y), ImVec2(vp->Pos.x + sidebarW, vp->Pos.y + vp->Size.y),
              col::border);

  ImGui::SetCursorPos(ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(18)));
  ImGui::BeginChild("##sidebar", ImVec2(sidebarW, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  drawSidebar();
  ImGui::EndChild();
  ImGui::PopStyleVar();

  ImGui::SameLine(0, 0);
  ImGui::BeginChild("##main", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  drawHeader();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(30), S(6)));
  ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(10)));
  drawBanners();
  switch (m_page) {
    case Page::Dashboard: pageDashboard(); break;
    case Page::Sources: pageSources(); break;
    case Page::Filters: pageFilters(); break;
    case Page::Blur: pageBlur(); break;
    case Page::Output: pageOutput(); break;
    case Page::Performance: pagePerformance(); break;
    case Page::Settings: pageSettings(); break;
    case Page::Help: pageHelp(); break;
    default: break;
  }
  ImGui::Dummy(ImVec2(0, S(20)));
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::End();
}

void App::drawSidebar() {
  ImVec2 p = ImGui::GetCursorScreenPos();
  DrawLogo(ImVec2(p.x + S(2), p.y), S(40));
  ImGui::SetCursorScreenPos(ImVec2(p.x + S(52), p.y - S(1)));
  ImGui::BeginGroup();
  ImGui::PushFont(fonts().hero);
  ImGui::TextUnformatted("Blur Live");
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(8));
  ImGui::PushFont(fonts().caption);
  ImGui::PushStyleColor(ImGuiCol_Text, col::textFaint);
  ImGui::Text("Motion blur & filters  v%s", BLURLIVE_VERSION);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(64)));

  SectionLabel("MENU");
  ImGui::Dummy(ImVec2(0, S(2)));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float w = ImGui::GetContentRegionAvail().x;
  for (int i = 0; i < int(Page::Count); ++i) {
    ImGui::PushID(i);
    ImVec2 ip = ImGui::GetCursorScreenPos();
    float h = S(40);
    bool clicked = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool active = int(m_page) == i;
    if (active) {
      dl->AddRectFilled(ip, ImVec2(ip.x + w, ip.y + h), IM_COL32(139, 92, 246, 38), S(10));
      dl->AddRectFilled(ImVec2(ip.x, ip.y + S(10)), ImVec2(ip.x + S(3), ip.y + h - S(10)), col::accentHi, S(2));
    } else if (hovered) {
      dl->AddRectFilled(ip, ImVec2(ip.x + w, ip.y + h), IM_COL32(255, 255, 255, 10), S(10));
    }
    ImU32 tc = active ? col::text : hovered ? col::text : col::textDim;
    ImGui::PushFont(fonts().icon);
    dl->AddText(ImVec2(ip.x + S(14), ip.y + (h - ImGui::GetFontSize()) * 0.5f), active ? col::accentHi : tc,
                kPageIcons[i]);
    ImGui::PopFont();
    ImGui::PushFont(active ? fonts().semibold : fonts().regular);
    dl->AddText(ImVec2(ip.x + S(46), ip.y + (h - ImGui::GetFontSize()) * 0.5f), tc, kPageNames[i]);
    ImGui::PopFont();
    if (clicked) m_page = Page(i);
    ImGui::PopID();
  }

  // Bottom status card.
  float cardH = S(118);
  float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - S(18);
  ImVec2 cp(p.x, std::max(ImGui::GetCursorScreenPos().y + S(10), bottom - cardH));
  dl->AddRectFilled(cp, ImVec2(cp.x + w, cp.y + cardH), col::card, S(12));
  dl->AddRect(cp, ImVec2(cp.x + w, cp.y + cardH), col::border, S(12));
  const bool running = m_snap.state == PipelineState::Running;
  ImU32 sc = running ? col::success : m_snap.state == PipelineState::Error ? col::danger : col::textFaint;
  dl->AddCircleFilled(ImVec2(cp.x + S(18), cp.y + S(22)), S(4.5f), sc);
  ImGui::PushFont(fonts().semibold);
  dl->AddText(ImVec2(cp.x + S(30), cp.y + S(12)), col::text, running ? "Capturing" : pipelineStateName(m_snap.state));
  ImGui::PopFont();
  ImGui::PushFont(fonts().caption);
  std::string l1 = running ? fmt("%.0f fps in", m_snap.captureFps) + fmt("  ·  %.0f fps out", m_snap.presentFps)
                           : std::string("No capture running");
  std::string l2 = running ? fmt("GPU %.2f ms", m_snap.gpuMs) + fmt("  ·  shutter %.1f ms", m_snap.shutterMs)
                           : std::string(m_settings.blurEnabled ? "Blur enabled" : "Blur disabled");
  dl->AddText(ImVec2(cp.x + S(14), cp.y + S(40)), col::textDim, l1.c_str());
  dl->AddText(ImVec2(cp.x + S(14), cp.y + S(60)), col::textDim, l2.c_str());
  dl->AddText(ImVec2(cp.x + S(14), cp.y + S(86)), col::textFaint,
              m_settings.hotkeysEnabled ? "Ctrl+Alt+ B blur  F effects  C compare" : "Hotkeys disabled");
  ImGui::PopFont();
}

void App::drawStartStopButton(float width) {
  const bool running = m_snap.state == PipelineState::Running || m_snap.state == PipelineState::Starting;
  if (running) {
    std::string label = std::string(icon::stop) + "  Stop capture";
    if (DangerButton(label.c_str(), ImVec2(width, S(42)))) m_pipeline.stopCapture();
  } else {
    std::string label = std::string(icon::play) + "  Start capture";
    bool can = m_snap.deviceReady && WgcCapture::isSupported();
    if (PrimaryButton(label.c_str(), ImVec2(width, S(42)), can)) startSelected();
    if (ImGui::IsItemHovered() && !selectedWindow()) ImGui::SetTooltip("Choose a game on the Sources page first");
  }
}

void App::drawHeader() {
  ImGui::SetCursorPos(ImVec2(S(30), S(22)));
  ImGui::BeginGroup();
  ImGui::PushFont(fonts().title);
  ImGui::TextUnformatted(kPageNames[int(m_page)]);
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));
  ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
  ImGui::TextUnformatted(kPageSubtitles[int(m_page)]);
  ImGui::PopStyleColor();
  ImGui::EndGroup();

  float right = ImGui::GetWindowWidth() - S(30);
  float btnW = S(172);
  ImGui::SetCursorPos(ImVec2(right - btnW, S(26)));
  drawStartStopButton(btnW);
  const char* stateText = m_snap.state == PipelineState::Running ? "Capturing"
                          : m_snap.state == PipelineState::Starting ? "Starting"
                          : m_snap.state == PipelineState::Error    ? "Error"
                                                                    : "Idle";
  ImU32 sc = m_snap.state == PipelineState::Running ? col::success
             : m_snap.state == PipelineState::Error ? col::danger
                                                    : col::textDim;
  ImGui::PushFont(fonts().semibold);
  float pillW = ImGui::CalcTextSize(stateText).x + S(44);
  ImGui::PopFont();
  ImGui::SetCursorPos(ImVec2(right - btnW - S(14) - pillW, S(32)));
  StatusPill(stateText, sc);
  ImGui::SetCursorPos(ImVec2(0, S(96)));
}

void App::drawBanners() {
  if (!m_pipelineError.empty()) {
    Banner(icon::error, col::danger, m_pipelineError.c_str());
    ImGui::Dummy(ImVec2(0, S(2)));
  }
  if (m_snap.messageIsError && !m_snap.message.empty() && m_snap.message != m_dismissedMessage) {
    float y = ImGui::GetCursorPosY();
    Banner(icon::error, col::danger, m_snap.message.c_str());
    float after = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionAvail().x + S(30) - S(36), y + S(8)));
    if (IconButton(icon::close, "Dismiss")) m_dismissedMessage = m_snap.message;
    ImGui::SetCursorPosY(after + S(2));
  }
  if (m_snap.state == PipelineState::Running) {
    if (m_snap.blackFrames)
      Banner(icon::warn, col::warning,
             "The captured frames are completely black. The game may block capture (protected content or anti-cheat), "
             "or it may be in exclusive fullscreen. Switch the game to borderless / windowed mode.");
    else if (m_snap.waitingForFrames)
      Banner(icon::info, col::info,
             "No new frames for over a second. Windows only delivers frames when the window changes: the game may be "
             "minimized, paused, or showing a static screen.");
    if (m_snap.outputMode == OutputMode::Overlay && !m_snap.outputVisible && !m_snap.waitingForFrames &&
        m_settings.autoHideOverlay)
      Banner(icon::info, col::info,
             "The overlay is hidden while the game is not the active window. Click into the game to see the processed "
             "output (auto-hide can be turned off on the Output page).");
  }
}

// ---------------------------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------------------------

static void twoColumns(float& colW, bool& stacked) {
  float avail = ImGui::GetContentRegionAvail().x;
  stacked = avail < S(820);
  colW = stacked ? avail : (avail - S(16)) * 0.5f;
}

void App::pageDashboard() {
  float colW;
  bool stacked;
  twoColumns(colW, stacked);
  const bool running = m_snap.state == PipelineState::Running;

  // ---- Source card ----
  BeginCard("##src", colW);
  CardHeader(icon::game, "Capture source", running ? "Live" : "Select a game, then start capture");
  const WindowInfo* sel = selectedWindow();
  std::string title, sub;
  ImTextureID tex{};
  if (running) {
    title = toUtf8(m_snap.targetTitle);
    sub = toUtf8(m_snap.targetExe) + fmt("  ·  %.0f", m_snap.srcW) + fmt(" x %.0f", m_snap.srcH);
    for (auto& w : m_windows)
      if (w.hwnd == m_snap.targetHwnd) tex = m_icons.get(w.hwnd, w.exePath);
  } else if (sel) {
    title = toUtf8(sel->title);
    sub = toUtf8(sel->exeName) + fmt("  ·  %.0f", sel->rect.right - sel->rect.left) +
          fmt(" x %.0f", sel->rect.bottom - sel->rect.top);
    tex = m_icons.get(sel->hwnd, sel->exePath);
  }
  if (!title.empty()) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float isz = S(44);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + isz, p.y + isz), col::rgba(36, 31, 52), S(10));
    if (tex) dl->AddImage(tex, ImVec2(p.x + S(6), p.y + S(6)), ImVec2(p.x + isz - S(6), p.y + isz - S(6)));
    ImGui::SetCursorScreenPos(ImVec2(p.x + isz + S(12), p.y));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW - S(110));
    ImGui::PushFont(fonts().hero);
    ImGui::TextWrapped("%s", title.c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));
    ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
    ImGui::TextUnformatted(sub.c_str());
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, S(4)));
  } else {
    Hint("No game selected yet. Open Sources and pick the window you want to blur. Borderless or windowed games work "
         "best.");
    ImGui::Dummy(ImVec2(0, S(4)));
  }
  float bw = (ImGui::GetContentRegionAvail().x - S(10)) * 0.5f;
  drawStartStopButton(bw);
  ImGui::SameLine();
  std::string ch = std::string(icon::game) + "  Change source";
  if (SecondaryButton(ch.c_str(), ImVec2(bw, S(42)))) m_page = Page::Sources;
  if (running) {
    ImGui::Dummy(ImVec2(0, S(2)));
    std::string out = std::string("Output: ") + outputModeName(m_snap.outputMode) +
                      fmt("  ·  %.0f", m_snap.outW) + fmt(" x %.0f", m_snap.outH) +
                      (m_snap.outputMode == OutputMode::Overlay ? (m_snap.outputVisible ? "  ·  visible" : "  ·  hidden")
                                                                : "");
    Hint(out.c_str());
  }
  EndCard();
  if (!stacked) ImGui::SameLine(0, S(16));

  // ---- Live stats card ----
  BeginCard("##live", colW);
  CardHeader(icon::speed, "Live performance", running ? "Measured every frame" : "Start capture to see live numbers");
  float tw = (ImGui::GetContentRegionAvail().x - S(10)) * 0.5f;
  auto num = [&](float v, const char* f) { return running ? fmt(f, v) : std::string("--"); };
  StatTile("Capture rate", num(m_snap.captureFps, "%.0f").c_str(), "fps", col::accent, tw,
           "Frames per second delivered by Windows Graphics Capture. Windows only sends a frame when the game presents "
           "a new one, so this follows the game's own frame rate.");
  ImGui::SameLine();
  StatTile("Output rate", num(m_snap.presentFps, "%.0f").c_str(), "fps", col::info, tw,
           "Processed frames presented per second.");
  StatTile("GPU time / frame", num(m_snap.gpuMs + m_snap.gpuFilterMs, "%.2f").c_str(), "ms", col::success, tw,
           "Total GPU time of motion blur + filters for one frame, measured with Direct3D timestamp queries. This "
           "is shader time, not end-to-end latency.");
  ImGui::SameLine();
  std::string lat = running && m_snap.displayLatencyValid ? fmt("%.1f", m_snap.displayLatencyMs) : std::string("--");
  StatTile("Added latency (est.)", lat.c_str(), "ms", col::warning, tw,
           "Estimated delay between when the original game frame was due on screen and when the processed frame is "
           "shown, from DirectComposition / DXGI frame statistics. Includes Windows composition; typically about one "
           "display refresh.");
  EndCard();

  // ---- Blur controls ----
  BeginCard("##blur", 0);
  CardHeader(icon::blur, "Motion blur",
             m_settings.mode == BlurMode::MotionFlow ? "Motion-compensated shutter (Blur-style)" : "Classic frame blending");
  ToggleRow("Enable motion blur",
            m_settings.hotkeysEnabled ? "Toggle in-game with Ctrl+Alt+B. When off, frames pass through untouched."
                                      : "When off, frames pass through untouched.",
            &m_settings.blurEnabled);
  ImGui::Dummy(ImVec2(0, S(4)));
  if (m_settings.mode == BlurMode::MotionFlow) {
    float shutterMs = m_settings.shutterFollowsCapture
                          ? m_settings.blurAmount * (m_snap.captureIntervalMs > 0 ? m_snap.captureIntervalMs : 16.67f)
                          : m_settings.blurAmount * 1000.0f / m_settings.referenceFps;
    std::string desc = fmt("Shutter %.1f ms", shutterMs) +
                       (m_settings.shutterFollowsCapture ? fmt("  ·  %.0f%% of each captured frame", m_settings.blurAmount * 100)
                                                         : fmt("  ·  %.0f deg shutter at ", m_settings.blurAmount * 360) +
                                                               fmt("%.0f fps", m_settings.referenceFps));
    if (LabeledSlider("Blur strength", &m_settings.blurAmount, 0.0f, 2.0f, "%.0f%%", desc.c_str(), 100.0f))
      m_settings.profile = Profile::Custom;
  } else {
    if (LabeledSliderInt("Frames blended", &m_settings.blendFrames, 2, 16, "%.0f frames",
                         "Classic mode averages this many captured frames."))
      m_settings.profile = Profile::Custom;
  }
  ImGui::Dummy(ImVec2(0, S(4)));
  SectionLabel("PROFILE");
  static const char* kProfiles[] = {"Subtle", "Cinematic", "Strong", "Responsive", "Custom"};
  int prof = int(m_settings.profile);
  if (SegmentedControl("##prof", &prof, kProfiles, 5)) applyProfile(m_settings, Profile(prof));
  Hint(profileDescription(m_settings.profile));
  ImGui::Dummy(ImVec2(0, S(2)));
  SectionLabel("QUALITY");
  static const char* kQ[] = {"Low", "Medium", "High", "Ultra"};
  SegmentedControl("##q", &m_settings.quality, kQ, 4);
  Hint((fmt("%.0f temporal samples per shutter window", qualitySamples(m_settings.quality)) +
        "  ·  Blur's default (1200 fps interpolation, 60 fps output) is equivalent to 20.")
           .c_str());
  EndCard();

  drawEffectsQuickCard();

  // ---- Graphs ----
  BeginCard("##graphs", 0);
  CardHeader(icon::sliders, "Frame timing", "Last 240 frames");
  float gw = (ImGui::GetContentRegionAvail().x - S(10)) * 0.5f;
  ImGui::BeginGroup();
  SectionLabel("CAPTURE INTERVAL");
  Sparkline("##gi", m_snap.graphInterval, ImVec2(gw, S(110)), col::accentHi);
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  SectionLabel("GPU TIME PER FRAME");
  Sparkline("##gg", m_snap.graphGpu, ImVec2(gw, S(110)), col::success);
  ImGui::EndGroup();
  EndCard();
}

void App::pageSources() {
  float avail = ImGui::GetContentRegionAvail().x;
  const float lineY = ImGui::GetCursorPosY();
  ImGui::SetNextItemWidth(avail - S(250));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(11)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(10));
  std::string hint = std::string(icon::search) + "  Search by title or process";
  ImGui::InputTextWithHint("##search", hint.c_str(), m_search, sizeof(m_search));
  ImGui::PopStyleVar(2);
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(9));
  if (ToggleSwitch("##all", &m_settings.showAllWindows)) refreshWindows();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
  ImGui::TextUnformatted("All windows");
  ImGui::SameLine(avail - S(44));
  ImGui::SetCursorPosY(lineY + S(3));
  if (IconButton(icon::refresh, "Refresh the window list")) refreshWindows();
  ImGui::Dummy(ImVec2(0, S(2)));

  std::wstring filter = lowerW(toWide(m_search));
  int shown = 0;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float w = ImGui::GetContentRegionAvail().x;
  for (auto& win : m_windows) {
    if (!filter.empty() && lowerW(win.title).find(filter) == std::wstring::npos &&
        lowerW(win.exeName).find(filter) == std::wstring::npos)
      continue;
    ++shown;
    ImGui::PushID(win.hwnd);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = S(66);
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool dbl = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    bool selected = win.hwnd == m_selected;
    bool capturing = m_snap.state == PipelineState::Running && m_snap.targetHwnd == win.hwnd;
    if (clicked) m_selected = win.hwnd;
    if (dbl) {
      m_selected = win.hwnd;
      startSelected();
    }
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      selected ? IM_COL32(139, 92, 246, 34) : hovered ? col::cardHover : col::card, S(12));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), selected ? col::accent : col::border, S(12), 0, selected ? S(1.5f) : 1.0f);
    float isz = S(40);
    ImVec2 ip(p.x + S(14), p.y + (h - isz) * 0.5f);
    dl->AddRectFilled(ip, ImVec2(ip.x + isz, ip.y + isz), col::rgba(36, 31, 52), S(9));
    if (ImTextureID tex = m_icons.get(win.hwnd, win.exePath))
      dl->AddImage(tex, ImVec2(ip.x + S(5), ip.y + S(5)), ImVec2(ip.x + isz - S(5), ip.y + isz - S(5)));
    float tx = ip.x + isz + S(14);
    float badgeX = p.x + w - S(14);
    auto badge = [&](const char* text, ImU32 c) {
      ImGui::PushFont(fonts().caption);
      ImVec2 ts = ImGui::CalcTextSize(text);
      badgeX -= ts.x + S(16);
      ImVec2 bp(badgeX, p.y + (h - ts.y - S(6)) * 0.5f);
      dl->AddRectFilled(bp, ImVec2(bp.x + ts.x + S(14), bp.y + ts.y + S(6)), (c & 0x00FFFFFF) | 0x30000000, S(8));
      dl->AddText(ImVec2(bp.x + S(7), bp.y + S(3)), c, text);
      ImGui::PopFont();
      badgeX -= S(6);
    };
    if (capturing) badge("CAPTURING", col::success);
    if (win.minimized) badge("MINIMIZED", col::warning);
    if (win.likelyGame) badge(win.gameHint.empty() ? "GAME" : win.gameHint.c_str(), col::accentHi);
    std::string t = toUtf8(win.title);
    ImGui::PushFont(fonts().semibold);
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(badgeX - S(8), p.y + h), true);
    dl->AddText(ImVec2(tx, p.y + S(13)), col::text, t.c_str());
    ImGui::PopFont();
    std::string s2 = toUtf8(win.exeName) + fmt("   %.0f", win.rect.right - win.rect.left) +
                     fmt(" x %.0f", win.rect.bottom - win.rect.top) + (win.coversMonitor ? "  ·  fullscreen-size" : "");
    ImGui::PushFont(fonts().caption);
    dl->AddText(ImVec2(tx, p.y + S(37)), col::textDim, s2.c_str());
    ImGui::PopFont();
    dl->PopClipRect();
    ImGui::PopID();
  }
  if (shown == 0) {
    ImGui::Dummy(ImVec2(0, S(20)));
    Hint(m_windows.empty() ? "No capturable windows found. Start your game (borderless or windowed works best), then "
                             "press refresh. Turn on \"All windows\" to include regular apps."
                           : "Nothing matches your search.");
  }
  ImGui::Dummy(ImVec2(0, S(6)));
  if (const WindowInfo* sel = selectedWindow()) {
    std::string l = "Selected: " + toUtf8(sel->title);
    Hint(l.c_str());
  }
  Hint("Tip: double-click a window to start capturing it right away. Games running as administrator can only be "
       "captured if Blur Live also runs as administrator.");
}

void App::pageBlur() {
  float colW;
  bool stacked;
  twoColumns(colW, stacked);

  BeginCard("##algo", 0);
  CardHeader(icon::bolt, "Algorithm", "Choose how motion is turned into blur");
  float rw = stacked ? ImGui::GetContentRegionAvail().x : (ImGui::GetContentRegionAvail().x - S(12)) * 0.5f;
  if (RadioCard("flow", icon::bolt, "Motion-compensated",
                "Estimates per-block motion between frames on the GPU and integrates the shutter along the motion "
                "paths, the real-time counterpart of Blur's interpolate-then-blend pipeline. Continuous streaks, no "
                "ghost copies. Areas where motion can't be estimated (occlusions, cuts) stay sharp.",
                m_settings.mode == BlurMode::MotionFlow, rw, "Recommended")) {
    m_settings.mode = BlurMode::MotionFlow;
    m_settings.profile = Profile::Custom;
  }
  if (!stacked) ImGui::SameLine(0, S(12));
  if (RadioCard("blend", icon::video, "Frame blending",
                "Averages the last N captured frames, like Blur with interpolation turned off. Cheapest, but moving "
                "objects turn into discrete ghost copies, and at equal capture/display rates it adds visible trailing.",
                m_settings.mode == BlurMode::FrameBlend, rw)) {
    m_settings.mode = BlurMode::FrameBlend;
    m_settings.profile = Profile::Custom;
  }
  EndCard();

  BeginCard("##shutter", colW);
  CardHeader(icon::sliders, m_settings.mode == BlurMode::MotionFlow ? "Shutter" : "History",
             m_settings.mode == BlurMode::MotionFlow ? "How long each output frame 'exposes'" : "How many frames are averaged");
  bool changed = false;
  if (m_settings.mode == BlurMode::MotionFlow) {
    changed |= LabeledSlider("Blur strength (blur amount)", &m_settings.blurAmount, 0.0f, 2.0f, "%.0f%%",
                             "100% = a full 360 degree shutter at the reference frame rate (Blur's blur amount = 1).",
                             100.0f);
    ImGui::Dummy(ImVec2(0, S(4)));
    if (m_settings.shutterFollowsCapture) ImGui::BeginDisabled();
    changed |= LabeledSlider("Reference frame rate", &m_settings.referenceFps, 24.0f, 240.0f, "%.0f fps",
                             "Blur's 'output fps'. The shutter lasts blur amount / reference fps, independent of how fast "
                             "the game runs, so 60 gives the classic cinematic look even at 280 fps.");
    if (m_settings.shutterFollowsCapture) ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, S(4)));
    changed |= ToggleRow("Follow the capture rate",
                         "Shutter = blur amount x the measured interval between captured frames. Blur gets subtler as "
                         "the game's frame rate rises.",
                         &m_settings.shutterFollowsCapture);
    float shutterMs = m_settings.shutterFollowsCapture
                          ? m_settings.blurAmount * (m_snap.captureIntervalMs > 0 ? m_snap.captureIntervalMs : 16.67f)
                          : m_settings.blurAmount * 1000.0f / m_settings.referenceFps;
    ImGui::Dummy(ImVec2(0, S(2)));
    Hint((fmt("Current shutter: %.1f ms", shutterMs) +
          (m_snap.state == PipelineState::Running
               ? fmt("  ·  spans %.0f frame pair(s)", m_snap.segments) + fmt(", %.0f samples", m_snap.samples)
               : std::string()))
             .c_str());
  } else {
    changed |= LabeledSliderInt("Frames blended", &m_settings.blendFrames, 2, 16, "%.0f frames",
                                "More frames = longer trails. Frames separated by a pause or a scene cut are never mixed.");
  }
  EndCard();
  if (!stacked) ImGui::SameLine(0, S(16));

  BeginCard("##weight", colW);
  CardHeader(icon::chip, "Weighting", "How much each moment of the shutter contributes");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##wcombo", weightingName(m_settings.weighting.type))) {
    for (int i = 0; i < int(Weighting::Count); ++i) {
      bool sel = int(m_settings.weighting.type) == i;
      if (ImGui::Selectable(weightingName(Weighting(i)), sel)) {
        m_settings.weighting.type = Weighting(i);
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  Hint(weightingDescription(m_settings.weighting.type));
  WeightCurve(buildWeightLut(m_settings.weighting), ImVec2(ImGui::GetContentRegionAvail().x, S(110)));
  if (m_settings.weighting.type == Weighting::Gaussian || m_settings.weighting.type == Weighting::GaussianSym) {
    changed |= LabeledSlider("Standard deviation", &m_settings.weighting.gaussStdDev, 0.1f, 4.0f, "%.2f");
    if (m_settings.weighting.type == Weighting::Gaussian)
      changed |= LabeledSlider("Mean", &m_settings.weighting.gaussMean, -1.0f, 3.0f, "%.2f",
                               "Position of the peak inside the bound (Blur default 2 = newest).");
    changed |= LabeledSlider("Bound (upper)", &m_settings.weighting.gaussBoundHi, 0.5f, 4.0f, "%.2f");
  }
  if (m_settings.weighting.type == Weighting::Custom) {
    static char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", m_settings.weighting.custom.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##custom", "e.g. 1,2,3,2,1", buf, sizeof(buf))) {
      m_settings.weighting.custom = buf;
      changed = true;
    }
    std::vector<double> tmp;
    if (!parseCustomWeights(m_settings.weighting.custom, tmp))
      Banner(icon::warn, col::warning, "Enter numbers separated by commas. Using equal weights until then.");
  }
  EndCard();

  BeginCard("##quality", 0);
  CardHeader(icon::speed, "Quality", "Samples per shutter and blending space");
  static const char* kQ[] = {"Low  (12)", "Medium  (24)", "High  (48)", "Ultra  (96)"};
  SegmentedControl("##q2", &m_settings.quality, kQ, 4);
  Hint("Temporal samples along the motion path per shutter window. More samples = smoother streaks on very fast motion. "
       "Measured on an RTX 5070 at 1080p: Medium ~0.8 ms, High ~1.2 ms, Ultra ~2.0 ms GPU per frame.");
  ImGui::Dummy(ImVec2(0, S(4)));
  changed |= LabeledSlider("Blending gamma", &m_settings.gamma, 1.0f, 3.0f, "%.2f",
                           "1.0 blends displayed values (Blur's default). 2.2 blends in approximately linear light, "
                           "which keeps bright highlights and lights streaking brightly.");
  EndCard();
  if (changed) m_settings.profile = Profile::Custom;
}

void App::pageOutput() {
  BeginCard("##mode", 0);
  CardHeader(icon::monitor, "Presentation", "Where the processed frames appear");
  float w = ImGui::GetContentRegionAvail().x;
  struct M {
    OutputMode m;
    const char* glyph;
    const char* title;
    const char* desc;
    const char* badge;
  } modes[] = {
      {OutputMode::Overlay, icon::game, "Game overlay",
       "A borderless, top-most, click-through window placed exactly over the game. You keep playing normally: the game "
       "keeps keyboard and mouse focus and receives every click. Windows composes the overlay, which typically adds "
       "about one display refresh of latency (about 3.6 ms at 280 Hz).",
       "Recommended"},
      {OutputMode::Window, icon::window, "Window",
       "A normal resizable window showing the processed output. Useful for testing, recording, or putting on another "
       "screen manually. Press Esc in the window to close it.",
       nullptr},
      {OutputMode::Monitor, icon::monitor, "Fullscreen on a monitor",
       "A borderless fullscreen window on the monitor you choose. Ideal for a second display: play on one screen, "
       "watch the blurred version on the other. Eligible for tearing / independent flip when V-Sync is off.",
       nullptr},
  };
  for (auto& m : modes)
    if (RadioCard(m.title, m.glyph, m.title, m.desc, m_settings.outputMode == m.m, w, m.badge))
      m_settings.outputMode = m.m;
  EndCard();

  float colW;
  bool stacked;
  twoColumns(colW, stacked);
  BeginCard("##opts", colW);
  CardHeader(icon::settings, "Options", nullptr);
  SectionLabel("MONITOR (fullscreen mode)");
  if (m_settings.outputMode != OutputMode::Monitor) ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(-1);
  auto monLabel = [&](int i) {
    if (i < 0 || i >= int(m_monitors.size())) return std::string("Monitor ") + std::to_string(i + 1);
    const MonitorInfo& m = m_monitors[size_t(i)];
    std::string name = m.friendlyName.empty() ? toUtf8(m.name) : toUtf8(m.friendlyName);
    return "Monitor " + std::to_string(i + 1) + "  " + name + fmt("  (%.0f", m.rect.right - m.rect.left) +
           fmt(" x %.0f", m.rect.bottom - m.rect.top) + fmt(" @ %.0f Hz)", m.refreshHz) + (m.primary ? "  primary" : "");
  };
  if (ImGui::BeginCombo("##mon", monLabel(m_settings.outputMonitor).c_str())) {
    for (int i = 0; i < int(m_monitors.size()); ++i)
      if (ImGui::Selectable(monLabel(i).c_str(), i == m_settings.outputMonitor)) m_settings.outputMonitor = i;
    ImGui::EndCombo();
  }
  if (m_settings.outputMode != OutputMode::Monitor) ImGui::EndDisabled();
  ImGui::Dummy(ImVec2(0, S(6)));
  ToggleRow("V-Sync", "Off = present immediately (lowest latency; tearing allowed in window/monitor modes). The overlay "
                      "is always composed by Windows, so it never tears.",
            &m_settings.vsync);
  ImGui::Dummy(ImVec2(0, S(4)));
  ToggleRow("Hide overlay when the game is not focused",
            "Prevents a frozen game image from covering your desktop when you Alt+Tab.", &m_settings.autoHideOverlay);
  ImGui::Dummy(ImVec2(0, S(4)));
  ToggleRow("Hide output from screen capture",
            "Excludes the output window from OBS / screenshots (WDA_EXCLUDEFROMCAPTURE). Leave off if you want to "
            "record the blurred gameplay with display capture.",
            &m_settings.excludeOutputFromCapture);
  EndCard();
  if (!stacked) ImGui::SameLine(0, S(16));
  BeginCard("##how", colW);
  CardHeader(icon::info, "What is and isn't possible", nullptr);
  Hint("An external app cannot change the pixels a game sends to the display. Blur Live captures the game window, "
       "processes it on the GPU and shows the result in its own window on top. The overlay is opaque and covers the "
       "game exactly; it is not a transparent filter.");
  ImGui::Dummy(ImVec2(0, S(2)));
  Hint("Latency budget: the game frame is composed by Windows, delivered to Blur Live (WGC), processed (about 1 ms), "
       "then composed again with the overlay. Expect roughly one extra display refresh. The Dashboard shows the "
       "measured estimate.");
  ImGui::Dummy(ImVec2(0, S(2)));
  Hint("Exclusive-fullscreen games cannot be overlaid or reliably captured; use borderless windowed. Anti-cheat "
       "protected games may block capture (black frames). Blur Live never injects into or reads from game processes.");
  EndCard();
}

void App::pagePerformance() {
  const bool running = m_snap.state == PipelineState::Running;
  BeginCard("##perf", 0);
  CardHeader(icon::speed, "Pipeline", running ? "Live measurements" : "Start capture to collect measurements");
  float avail = ImGui::GetContentRegionAvail().x;
  int perRow = avail > S(900) ? 4 : 2;
  float tw = (avail - S(10) * float(perRow - 1)) / float(perRow);
  auto v = [&](float x, const char* f) { return running ? fmt(f, x) : std::string("--"); };
  struct T {
    const char* label;
    std::string value;
    const char* unit;
    ImU32 c;
    const char* tip;
  } tiles[] = {
      {"Capture rate", v(m_snap.captureFps, "%.0f"), "fps", col::accent, "Frames delivered by Windows Graphics Capture."},
      {"Output rate", v(m_snap.presentFps, "%.0f"), "fps", col::info, "Processed frames presented."},
      {"Frame interval", v(m_snap.captureIntervalMs, "%.2f"), "ms", col::accentHi, "Median time between captured frames."},
      {"Motion blur GPU", v(m_snap.gpuMs, "%.3f"), "ms", col::success, "Blur engine GPU time per frame (timestamp queries)."},
      {"Filters GPU", v(m_snap.gpuFilterMs, "%.3f"), "ms", col::success,
       "Filter-suite GPU time per frame (timestamp queries). 0 when no filter is enabled: the chain is skipped."},
      {"Total GPU / frame", v(m_snap.gpuMs + m_snap.gpuFilterMs, "%.3f"), "ms", col::success,
       "Shader execution time only. This is not end-to-end latency: see arrival-to-present and added latency."},
      {"CPU / frame", v(m_snap.cpuFrameMs, "%.3f"), "ms", col::success, "CPU time to submit processing and Present."},
      {"Arrival to present", v(m_snap.pipelineLatencyMs, "%.2f"), "ms", col::warning,
       "From Windows signalling the captured frame to our Present() returning. Measured."},
      {"Added latency (est.)", running && m_snap.displayLatencyValid ? fmt("%.2f", m_snap.displayLatencyMs) : "--", "ms",
       col::warning, "Estimated from compositor frame statistics: when our frame is shown minus when the original was due."},
      {"Dropped frames", running ? std::to_string(m_snap.framesDropped) : "--", "", col::danger,
       "Captured frames skipped because a newer frame was already waiting (processing fell behind)."},
      {"Frame pacing (median)", v(m_snap.pacingMedianMs, "%.2f"), "ms", col::info,
       "Median interval between our presents over the last 240 frames."},
      {"Pacing jitter (sd)", v(m_snap.pacingStdMs, "%.2f"), "ms", col::info,
       "Standard deviation of present intervals. Low = even frame delivery."},
      {"Worst 1% interval", v(m_snap.pacingP99Ms, "%.2f"), "ms", col::info,
       "99th percentile present interval: spikes show up here."},
  };
  int i = 0;
  for (auto& t : tiles) {
    if (i % perRow != 0) ImGui::SameLine();
    StatTile(t.label, t.value.c_str(), t.unit, t.c, tw, t.tip);
    ++i;
  }
  EndCard();

  float colW;
  bool stacked;
  twoColumns(colW, stacked);
  BeginCard("##gpu", colW);
  CardHeader(icon::chip, "GPU", m_snap.adapterName.empty() ? "No device" : m_snap.adapterName.c_str());
  auto row = [&](const char* k, const std::string& val) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
    ImGui::TextUnformatted(k);
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::TextUnformatted(val.c_str());
  };
  row("GPU 3D utilisation (system)", m_gpuMonitor.valid() ? fmt("%.0f %%", m_gpuMonitor.total3D()) : "unavailable");
  row("Blur Live GPU utilisation", m_gpuMonitor.valid() ? fmt("%.1f %%", m_gpuMonitor.process3D()) : "unavailable");
  row("Video memory in use (this app)", fmt("%.0f MB", double(m_snap.vramUsageMB)) + fmt(" of %.0f MB budget", double(m_snap.vramBudgetMB)));
  row("Processing resolution", running ? fmt("%.0f", m_snap.srcW) + fmt(" x %.0f", m_snap.srcH) : "--");
  row("Output size", running ? fmt("%.0f", m_snap.outW) + fmt(" x %.0f", m_snap.outH) : "--");
  row("Output tearing support", m_snap.tearing ? "yes" : "no (composed)");
  EndCard();
  if (!stacked) ImGui::SameLine(0, S(16));
  BeginCard("##eng", colW);
  CardHeader(icon::blur, "Engine", nullptr);
  row("Shutter", fmt("%.2f ms", m_snap.shutterMs));
  row("Frame pairs in shutter", running ? std::to_string(m_snap.segments) : "--");
  row("Temporal samples", running ? std::to_string(m_snap.samples) : "--");
  row("History coverage", running ? fmt("%.0f %%", m_snap.coverage * 100) : "--");
  row("Mean motion residual", running ? fmt("%.4f", m_snap.meanBlockError) : "--");
  row("Scene cuts detected", std::to_string(m_snap.sceneCuts));
  row("History resets (pauses)", std::to_string(m_snap.discontinuities));
  row("Frames captured / presented", std::to_string(m_snap.framesCaptured) + " / " + std::to_string(m_snap.framesPresented));
  EndCard();

  BeginCard("##graphs2", 0);
  CardHeader(icon::sliders, "Timelines", "Last 240 frames");
  float gw = ImGui::GetContentRegionAvail().x;
  SectionLabel("CAPTURE INTERVAL (ms)");
  Sparkline("##p1", m_snap.graphInterval, ImVec2(gw, S(120)), col::accentHi);
  SectionLabel("GPU TIME PER FRAME (ms)");
  Sparkline("##p2", m_snap.graphGpu, ImVec2(gw, S(120)), col::success);
  SectionLabel("FRAME ARRIVAL TO PRESENT (ms)");
  Sparkline("##p3", m_snap.graphLatency, ImVec2(gw, S(120)), col::warning);
  SectionLabel("PRESENT-TO-PRESENT INTERVAL / FRAME PACING (ms)");
  Sparkline("##p4", m_snap.graphPresent, ImVec2(gw, S(120)), col::info);
  EndCard();
}

void App::pageSettings() {
  float colW;
  bool stacked;
  twoColumns(colW, stacked);
  BeginCard("##cap", colW);
  CardHeader(icon::video, "Capture", "Applied on the next start (restarts a running capture)");
  ToggleRow("Capture the mouse cursor", "Most games draw their own cursor or crosshair; leave off.", &m_settings.captureCursor);
  ImGui::Dummy(ImVec2(0, S(4)));
  ToggleRow("Hide the yellow capture border", "Windows 11 only. Requests border-less capture.", &m_settings.hideCaptureBorder);
  ImGui::Dummy(ImVec2(0, S(4)));
  ToggleRow("Crop title bar and borders", "Process only the game's client area. Recommended for windowed games.",
            &m_settings.cropToClient);
  ImGui::Dummy(ImVec2(0, S(4)));
  LabeledSliderInt("Capture buffers", &m_settings.framePoolBuffers, 1, 4, "%.0f",
                   "2 is lowest latency. 3 can help if frames are dropped under heavy load.");
  EndCard();
  if (!stacked) ImGui::SameLine(0, S(16));
  BeginCard("##rob", colW);
  CardHeader(icon::warn, "Robustness", "When to stop blurring");
  LabeledSlider("Scene-cut threshold", &m_settings.sceneCutThreshold, 0.02f, 0.25f, "%.3f",
                "Mean motion error above which two frames are treated as different shots (no blur across them).");
  LabeledSlider("Motion trust: start fading", &m_settings.confidenceLow, 0.0f, 0.2f, "%.3f",
                "Per-block error where the estimated motion starts being distrusted.");
  LabeledSlider("Motion trust: fully sharp", &m_settings.confidenceHigh, 0.01f, 0.4f, "%.3f",
                "Per-block error above which that area is shown sharp instead of blurred (prevents smearing).");
  LabeledSlider("Pause detection", &m_settings.maxFrameGapMs, 20.0f, 500.0f, "%.0f ms",
                "A gap between frames longer than this resets the history, so stale frames are never blended in.");
  EndCard();

  BeginCard("##app", 0);
  CardHeader(icon::settings, "Application", nullptr);
  ToggleRow("Global hotkeys", "Ctrl+Alt+B toggles blur, Ctrl+Alt+S starts/stops capture, even while in game.",
            &m_settings.hotkeysEnabled);
  ImGui::Dummy(ImVec2(0, S(6)));
  std::string f1 = std::string(icon::folder) + "  Open settings folder";
  if (SecondaryButton(f1.c_str())) ShellExecuteW(nullptr, L"open", appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  ImGui::SameLine();
  std::string f2 = std::string(icon::info) + "  Open log file";
  if (SecondaryButton(f2.c_str()))
    ShellExecuteW(nullptr, L"open", (appDataDir() + L"\\blurlive.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  ImGui::SameLine();
  std::string f3 = std::string(icon::refresh) + "  Restore defaults";
  if (SecondaryButton(f3.c_str())) m_confirmReset = true;
  if (m_confirmReset) {
    ImGui::SameLine();
    if (DangerButton("Confirm reset")) {
      Settings d;
      d.lastTargetExe = m_settings.lastTargetExe;
      d.lastTargetTitle = m_settings.lastTargetTitle;
      m_settings = d;
      m_confirmReset = false;
    }
    ImGui::SameLine();
    if (SecondaryButton("Cancel")) m_confirmReset = false;
  }
  EndCard();

  BeginCard("##log", 0);
  CardHeader(icon::info, "Log", "Most recent events");
  ImGui::PushStyleColor(ImGuiCol_ChildBg, col::rgba(18, 16, 27));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
  ImGui::BeginChild("##logview", ImVec2(0, S(240)), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushFont(fonts().caption);
  for (auto& e : logSnapshot(300)) {
    ImU32 c = e.level == LogLevel::Error ? col::danger : e.level == LogLevel::Warn ? col::warning : col::textDim;
    ImGui::PushStyleColor(ImGuiCol_Text, col::textFaint);
    ImGui::TextUnformatted(e.timestamp.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", e.text.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  EndCard();
}

void App::pageHelp() {
  auto section = [&](const char* id, const char* glyph, const char* title, std::initializer_list<const char*> paras) {
    BeginCard(id, 0);
    CardHeader(glyph, title, nullptr);
    for (const char* p : paras) {
      ImGui::PushTextWrapPos(0);
      ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
      ImGui::TextWrapped("%s", p);
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0, S(2)));
    }
    EndCard();
  };
  section("##h1", icon::play, "Quick start",
          {"1. Run your game in borderless windowed (or windowed) mode.",
           "2. Open Sources and select the game window, then press Start capture.",
           "3. Click back into the game. The overlay sits exactly over it; you keep playing normally.",
           "4. Adjust blur strength and profile live on the Dashboard. Ctrl+Alt+B toggles blur, Ctrl+Alt+S stops."});
  section("##h2", icon::blur, "How the blur works",
          {"f0e/blur renders motion blur offline: it interpolates footage to a very high frame rate (1200 fps by "
           "default, using SVP motion vectors or RIFE) and then averages all frames inside each output frame's "
           "shutter. That's why it looks like real camera blur instead of ghosting.",
           "Blur Live does the same in real time on the GPU. For each new frame it builds a luma pyramid, estimates "
           "block motion coarse to fine (with a vector median filter and half-pixel refinement), then integrates the "
           "shutter window by warping between neighbouring frames along the motion vectors, using up to 96 samples.",
           "Differences from offline Blur: the window is causal (it can only use past frames, so blur trails behind "
           "objects instead of being centred on them), the motion model is block-based rather than SVP/RIFE quality, "
           "and areas where motion can't be estimated fall back to sharp instead of being interpolated."});
  section("##h3", icon::warn, "Limitations",
          {"Exclusive fullscreen: capture may be black or stop and the overlay can't appear above it. Use borderless.",
           "Anti-cheat: some protected games block Windows Graphics Capture (black frames). Blur Live never injects, "
           "hooks or reads game memory, and it does not try to bypass protection.",
           "Latency: external capture plus presentation adds roughly one display refresh plus about 1 ms processing.",
           "HDR: frames are captured as 8-bit SDR; HDR games are tone-mapped by Windows and the output is SDR.",
           "Frame delivery: Windows only sends frames when the window changes, so the output rate follows the game.",
           "Elevated games (run as administrator) need Blur Live to run as administrator too."});
  section("##h4", icon::help, "Troubleshooting",
          {"Black output: switch the game to borderless windowed; check that the game isn't DRM/anti-cheat protected; "
           "make sure it isn't minimized.",
           "Overlay not visible: the overlay hides when the game isn't the active window (Output > auto-hide). Click "
           "into the game.",
           "Mouse/keyboard not reaching the game: the overlay is click-through and never takes focus; if a game misbehaves, "
           "use Window or Monitor output instead.",
           "Stutter or dropped frames: lower Quality, or check GPU utilisation on the Performance page. The game itself "
           "must leave some GPU headroom.",
           "Windows says an Application Control policy blocked BlurLive.exe: Smart App Control blocks unsigned "
           "executables it has no reputation for, and the verdict can change from one build to the next. Turn Smart "
           "App Control off, or sign the executable with a trusted code-signing certificate.",
           "Logs are written to %APPDATA%\\BlurLive\\blurlive.log."});
  section("##hf", icon::sliders, "Filters",
          {"Every filter is independent: turning one off removes its effect completely and keeps its settings. "
           "Everything starts off; saturation is never enabled unless you turn it on.",
           "The master switch (Ctrl+Alt+F) bypasses every effect including motion blur, while capture keeps running. "
           "Compare (Ctrl+Alt+C) shows the untouched capture left of the split line and the processed image right.",
           "Performance: colour, tone, grading, sharpening and lens effects run in a single GPU pass; denoise, bloom "
           "and the local-contrast pyramid add small extra passes only while enabled. With no filter enabled the "
           "chain is skipped entirely (zero cost).",
           "Presets store every filter plus the motion-blur look. Built-in presets are templates: tweak them and use "
           "\"Save as\" to keep your version. Presets live in %APPDATA%\\BlurLive\\presets and can be imported/exported.",
           "Filters change how the image looks; they don't give a guaranteed competitive advantage."});
  section("##h5", icon::info, "Credits and licences",
          {"Visual reference: f0e/blur (GPL-3.0). Blur Live is an independent real-time reimplementation of the "
           "technique and contains no Blur source code. Weighting functions follow Blur's documented options (themselves "
           "credited to smoothie-rs and HFR-Resampler).",
           "Dear ImGui by Omar Cornut (MIT licence). Uses Windows Graphics Capture, Direct3D 11, DXGI and DirectComposition."});
}

}  // namespace bl::ui
