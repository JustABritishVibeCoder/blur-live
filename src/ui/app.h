#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <string>
#include <vector>

#include "capture/window_list.h"
#include "core/settings.h"
#include "filters/presets.h"
#include "pipeline/pipeline.h"
#include "platform/gpu_monitor.h"
#include "ui/icon_cache.h"

namespace bl::ui {

enum class Page { Dashboard, Sources, Filters, Blur, Output, Performance, Settings, Help, Count };

// The control-panel window. Runs on the main thread with its own D3D11 device; all capture and
// processing happens on the pipeline thread, so the UI can never stall the blur.
class App {
 public:
  // Command-line options (also handy for automated testing):
  //   --page <dashboard|sources|filters|blur|output|performance|settings|help>
  //   --capture "<window title substring>"   start capturing that window on launch
  //   --output <overlay|window|monitor>      output mode for this launch
  //   --preset "<name>"                        apply a preset at launch
  void parseCommandLine(const wchar_t* cmdLine);
  int run(HINSTANCE instance);

 private:
  static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handleMessage(UINT msg, WPARAM w, LPARAM l);
  bool createMainWindow(HINSTANCE instance);
  bool createDevice();
  void createRenderTarget();
  void destroyDevice();
  void registerHotkeys();

  void tick();          // background work every loop iteration (also when minimized)
  void drawFrame();
  void drawSidebar();
  void drawHeader();
  void drawBanners();
  void pageDashboard();
  void pageSources();
  void pageFilters();  // app_filters.cpp
  void drawPresetBar();
  void drawFilterToolbar();
  void drawPreviewPanel(float width);
  void drawFilterCard(const FilterDef& def);
  void drawMotionBlurFilterCard();
  void drawEffectsQuickCard();  // dashboard
  void applyPreset(const std::string& name);
  bool presetModified() const;
  void openPreviewTexture();
  void lockPreview();
  void unlockPreview();
  void pageBlur();
  void pageOutput();
  void pagePerformance();
  void pageSettings();
  void pageHelp();
  void drawStartStopButton(float width);

  void persistSettings();
  void refreshWindows();
  void startSelected();
  void toggleCapture();
  const WindowInfo* selectedWindow() const;

  HWND m_hwnd = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
  UINT m_resizeW = 0, m_resizeH = 0;
  bool m_occluded = false;
  float m_dpi = 1.0f;
  bool m_dpiChanged = false;
  bool m_quit = false;
  HANDLE m_timer = nullptr;

  Pipeline m_pipeline;
  std::string m_pipelineError;
  PipelineSnapshot m_snap;
  Settings m_settings;
  Settings m_pushedSettings;
  bool m_saveDirty = false;
  double m_lastSettingsChange = 0;

  std::vector<WindowInfo> m_windows;
  HWND m_selected = nullptr;
  double m_lastEnum = -100;
  char m_search[128] = {};
  std::vector<MonitorInfo> m_monitors;
  double m_lastMonitorEnum = -100;

  GpuMonitor m_gpuMonitor;
  double m_lastGpuSample = 0;
  IconCache m_icons;

  Page m_page = Page::Dashboard;
  std::string m_dismissedMessage;
  bool m_confirmReset = false;
  std::wstring m_autoCapture;  // --capture

  // Filters page
  PresetStore m_presets;
  char m_filterSearch[64] = {};
  std::array<bool, kFilterCount> m_filterExpanded{};
  std::array<bool, kFilterCategoryCount> m_categoryOpen{true, true, true, true, true, true};
  bool m_showAdvanced = false;
  int m_selectiveBand = 0;
  char m_presetNameBuf[96] = {};
  std::string m_presetMessage;
  bool m_presetMessageIsError = false;
  bool m_confirmDeletePreset = false;

  // Live preview: shared texture from the pipeline device, opened on the UI device.
  HANDLE m_previewHandle = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> m_previewTex;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_previewSrv;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_previewMutex;
  bool m_previewLocked = false;
  int m_outputOverride = -1;   // --output
  std::string m_startPreset;   // --preset
  OutputMode m_fileOutputMode = OutputMode::Overlay;
};

}  // namespace bl::ui
