#pragma once
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "core/settings.h"

namespace bl {

struct OutputOptions {
  OutputMode mode = OutputMode::Overlay;
  HWND target = nullptr;        // captured window (overlay follows it)
  int monitorIndex = 0;         // for OutputMode::Monitor
  bool vsync = false;
  bool autoHide = true;         // overlay: only visible while the target is in the foreground
  bool excludeFromCapture = false;
  bool cropToClient = true;     // overlay covers the client area only (matches a cropped capture)
  uint32_t initialWidth = 1280;  // window mode: initial client size
  uint32_t initialHeight = 720;
};

// Presents processed frames. Must be created, used and destroyed on one thread, which must pump
// window messages.
//
//  * Overlay: borderless, top-most, click-through (WS_EX_LAYERED|WS_EX_TRANSPARENT), never
//    activated, positioned exactly over the game window. Presented through DirectComposition so the
//    window has no redirection surface. The game keeps keyboard/mouse focus.
//  * Window: a normal resizable window with a flip-model swap chain (tearing allowed).
//  * Monitor: borderless fullscreen-sized window on a chosen monitor (eligible for independent flip).
class OutputWindow {
 public:
  ~OutputWindow();
  bool create(ID3D11Device* device, ID3D11DeviceContext* ctx, const OutputOptions& opt, std::string* error);
  void destroy();
  bool valid() const { return m_hwnd != nullptr; }

  // Draws `srv` (srcW x srcH) letterboxed into the back buffer and presents.
  // sourceDisplayTime: when the original (unprocessed) frame was due on screen (QPC seconds), used
  // to estimate the display latency we add.
  HRESULT present(ID3D11ShaderResourceView* srv, uint32_t srcW, uint32_t srcH, double sourceDisplayTime = 0);

  // Latest estimate of (our frame's display time - original frame's display time), in ms.
  // From DirectComposition frame statistics (overlay) or DXGI frame statistics (window/monitor).
  bool displayLatencyMs(float& ms) const {
    ms = m_displayLatencyMs;
    return m_displayLatencyValid;
  }

  // Overlay tracking (position / visibility). Call regularly (~every frame is fine; it is cheap).
  void update();
  void setVsync(bool v) { m_opt.vsync = v; }
  void setAutoHide(bool v) { m_opt.autoHide = v; }
  void setCropToClient(bool v) { m_opt.cropToClient = v; }

  bool closedByUser() const { return m_closedByUser; }
  bool isVisible() const { return m_visible; }
  HWND hwnd() const { return m_hwnd; }
  OutputMode mode() const { return m_opt.mode; }
  uint32_t width() const { return m_bbW; }
  uint32_t height() const { return m_bbH; }
  bool tearingSupported() const { return m_tearing; }

 private:
  static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
  bool createSwapChain(uint32_t w, uint32_t h, std::string* error);
  bool resizeBuffers(uint32_t w, uint32_t h);
  RECT desiredOverlayRect() const;

  Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
  Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcomp;
  Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
  Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcompVisual;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

  OutputOptions m_opt;
  HWND m_hwnd = nullptr;
  uint32_t m_bbW = 0, m_bbH = 0;
  bool m_tearing = false;
  bool m_visible = false;
  bool m_closedByUser = false;
  bool m_resizePending = false;
  RECT m_lastRect{};
  int64_t m_lastTopmostTick = 0;

  struct PendingPresent {
    UINT presentCount = 0;
    double sourceTime = 0;
  };
  PendingPresent m_pending[8]{};
  int m_pendingHead = 0;
  float m_displayLatencyMs = 0;
  bool m_displayLatencyValid = false;
};

}  // namespace bl
