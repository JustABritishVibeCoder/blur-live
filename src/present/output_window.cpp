#include "present/output_window.h"

#include <dxgi1_5.h>

#include <algorithm>

#include "capture/window_list.h"
#include "core/log.h"
#include "core/util.h"
#include "present_ps.h"
#include "present_vs.h"

using Microsoft::WRL::ComPtr;

namespace bl {

namespace {
const wchar_t* kClassName = L"BlurLiveOutput";
}  // namespace

OutputWindow::~OutputWindow() { destroy(); }

LRESULT CALLBACK OutputWindow::wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  auto* self = reinterpret_cast<OutputWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (self) {
    switch (msg) {
      case WM_NCHITTEST:
        if (self->m_opt.mode == OutputMode::Overlay) return HTTRANSPARENT;
        break;
      case WM_MOUSEACTIVATE:
        if (self->m_opt.mode != OutputMode::Window) return MA_NOACTIVATE;
        break;
      case WM_SIZE:
        self->m_resizePending = true;
        break;
      case WM_KEYDOWN:
        if (w == VK_ESCAPE && self->m_opt.mode == OutputMode::Window) {
          self->m_closedByUser = true;
          return 0;
        }
        break;
      case WM_CLOSE:
        // Only the normal window can be closed by the user; the overlay / monitor windows are owned
        // by the capture session (a stray WM_CLOSE from another tool must not stop capture).
        if (self->m_opt.mode == OutputMode::Window) self->m_closedByUser = true;
        return 0;
      case WM_ERASEBKGND:
        return 1;
      default: break;
    }
  }
  return DefWindowProcW(h, msg, w, l);
}

bool OutputWindow::create(ID3D11Device* device, ID3D11DeviceContext* ctx, const OutputOptions& opt,
                          std::string* error) {
  destroy();
  m_dev = device;
  m_ctx = ctx;
  m_opt = opt;
  m_closedByUser = false;

  static bool classRegistered = false;
  if (!classRegistered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      if (error) *error = "RegisterClassEx failed";
      return false;
    }
    classRegistered = true;
  }

  DWORD style = 0, exStyle = 0;
  RECT r{};
  switch (opt.mode) {
    case OutputMode::Overlay:
      style = WS_POPUP;
      exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                WS_EX_NOREDIRECTIONBITMAP;
      r = desiredOverlayRect();
      break;
    case OutputMode::Monitor: {
      style = WS_POPUP;
      exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
      auto mons = enumerateMonitors();
      if (mons.empty()) {
        if (error) *error = "No monitors found.";
        return false;
      }
      r = mons[size_t(std::clamp(opt.monitorIndex, 0, int(mons.size()) - 1))].rect;
      break;
    }
    case OutputMode::Window: {
      style = WS_OVERLAPPEDWINDOW;
      RECT work{};
      SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
      int maxW = (work.right - work.left) * 8 / 10, maxH = (work.bottom - work.top) * 8 / 10;
      double s = std::min({1.0, double(maxW) / std::max(1u, opt.initialWidth), double(maxH) / std::max(1u, opt.initialHeight)});
      r = {0, 0, LONG(opt.initialWidth * s), LONG(opt.initialHeight * s)};
      AdjustWindowRectEx(&r, style, FALSE, exStyle);
      OffsetRect(&r, work.left + 40 - r.left, work.top + 40 - r.top);
      break;
    }
  }
  int w = std::max<LONG>(64, r.right - r.left), h = std::max<LONG>(64, r.bottom - r.top);
  m_hwnd = CreateWindowExW(exStyle, kClassName, L"Blur Live Output", style, r.left, r.top, w, h, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
  if (!m_hwnd) {
    if (error) *error = "CreateWindowEx failed: " + hrToString(HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  m_lastRect = r;
  if (opt.mode == OutputMode::Overlay) SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
  if (opt.excludeFromCapture) SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE);

  RECT cr;
  GetClientRect(m_hwnd, &cr);
  if (!createSwapChain(uint32_t(std::max<LONG>(1, cr.right)), uint32_t(std::max<LONG>(1, cr.bottom)), error)) {
    destroy();
    return false;
  }

  if (FAILED(m_dev->CreateVertexShader(g_present_vs, sizeof(g_present_vs), nullptr, &m_vs)) ||
      FAILED(m_dev->CreatePixelShader(g_present_ps, sizeof(g_present_ps), nullptr, &m_ps))) {
    if (error) *error = "Failed to create presentation shaders.";
    destroy();
    return false;
  }
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  m_dev->CreateSamplerState(&sd, &m_sampler);

  if (opt.mode == OutputMode::Overlay) {
    m_visible = false;
    update();  // shows it if the game is in the foreground
  } else {
    ShowWindow(m_hwnd, opt.mode == OutputMode::Window ? SW_SHOW : SW_SHOWNOACTIVATE);
    m_visible = true;
  }
  logInfo("Output window created: {} {}x{} (tearing {})", outputModeName(opt.mode), m_bbW, m_bbH,
          m_tearing ? "available" : "unavailable");
  return true;
}

bool OutputWindow::createSwapChain(uint32_t w, uint32_t h, std::string* error) {
  ComPtr<IDXGIDevice> dxgiDev;
  m_dev.As(&dxgiDev);
  ComPtr<IDXGIAdapter> adapter;
  dxgiDev->GetAdapter(&adapter);
  ComPtr<IDXGIFactory2> factory;
  adapter->GetParent(IID_PPV_ARGS(&factory));

  ComPtr<IDXGIFactory5> f5;
  BOOL tearing = FALSE;
  if (SUCCEEDED(factory.As(&f5)))
    f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
  // Composition swap chains are always composed by DWM; tearing only applies to HWND swap chains.
  m_tearing = tearing && m_opt.mode != OutputMode::Overlay;

  DXGI_SWAP_CHAIN_DESC1 d{};
  d.Width = w;
  d.Height = h;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  d.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

  HRESULT hr;
  if (m_opt.mode == OutputMode::Overlay) {
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForComposition(m_dev.Get(), &d, nullptr, &m_swap);
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&m_dcomp));
    if (SUCCEEDED(hr)) hr = m_dcomp->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
    if (SUCCEEDED(hr)) hr = m_dcomp->CreateVisual(&m_dcompVisual);
    if (SUCCEEDED(hr)) hr = m_dcompVisual->SetContent(m_swap.Get());
    if (SUCCEEDED(hr)) hr = m_dcompTarget->SetRoot(m_dcompVisual.Get());
    if (SUCCEEDED(hr)) hr = m_dcomp->Commit();
  } else {
    d.Scaling = DXGI_SCALING_NONE;
    hr = factory->CreateSwapChainForHwnd(m_dev.Get(), m_hwnd, &d, nullptr, nullptr, &m_swap);
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
  }
  if (FAILED(hr)) {
    if (error) *error = "Swap chain creation failed: " + hrToString(hr);
    return false;
  }
  ComPtr<ID3D11Texture2D> bb;
  m_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
  m_dev->CreateRenderTargetView(bb.Get(), nullptr, &m_rtv);
  m_bbW = w;
  m_bbH = h;
  return true;
}

bool OutputWindow::resizeBuffers(uint32_t w, uint32_t h) {
  if (!m_swap || w == 0 || h == 0 || (w == m_bbW && h == m_bbH)) return true;
  m_rtv.Reset();
  m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  m_ctx->Flush();
  HRESULT hr = m_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr)) {
    logError("ResizeBuffers failed: {}", hrToString(hr));
    return false;
  }
  ComPtr<ID3D11Texture2D> bb;
  m_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
  m_dev->CreateRenderTargetView(bb.Get(), nullptr, &m_rtv);
  m_bbW = w;
  m_bbH = h;
  return true;
}

void OutputWindow::destroy() {
  if (m_ctx) m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  m_rtv.Reset();
  m_dcompVisual.Reset();
  m_dcompTarget.Reset();
  m_dcomp.Reset();
  m_swap.Reset();
  m_vs.Reset();
  m_ps.Reset();
  m_sampler.Reset();
  if (m_hwnd) {
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
  }
  if (m_ctx) m_ctx->Flush();
  m_ctx.Reset();
  m_dev.Reset();
  m_visible = false;
  m_bbW = m_bbH = 0;
}

RECT OutputWindow::desiredOverlayRect() const {
  if (m_opt.target && IsWindow(m_opt.target))
    return m_opt.cropToClient ? windowClientRect(m_opt.target) : windowVisualRect(m_opt.target);
  return RECT{0, 0, 640, 360};
}

void OutputWindow::update() {
  if (!m_hwnd) return;
  if (m_opt.mode != OutputMode::Overlay) return;

  const bool alive = m_opt.target && IsWindow(m_opt.target);
  bool show = alive && !IsIconic(m_opt.target);
  if (show && m_opt.autoHide) show = isTargetForeground(m_opt.target) || GetForegroundWindow() == m_hwnd;

  if (alive) {
    RECT r = desiredOverlayRect();
    if (!EqualRect(&r, &m_lastRect) && r.right > r.left && r.bottom > r.top) {
      SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      m_lastRect = r;
      m_resizePending = true;
    }
  }
  if (show != m_visible) {
    ShowWindow(m_hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    m_visible = show;
  } else if (show) {
    // Games sometimes put themselves top-most; re-assert our z-order a few times per second.
    int64_t now = GetTickCount64();
    if (now - m_lastTopmostTick > 500) {
      SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      m_lastTopmostTick = now;
    }
  }
}

HRESULT OutputWindow::present(ID3D11ShaderResourceView* srv, uint32_t srcW, uint32_t srcH, double sourceDisplayTime) {
  if (!m_swap || !srv || srcW == 0 || srcH == 0) return E_INVALIDARG;
  if (m_resizePending) {
    m_resizePending = false;
    RECT cr;
    GetClientRect(m_hwnd, &cr);
    resizeBuffers(uint32_t(std::max<LONG>(1, cr.right)), uint32_t(std::max<LONG>(1, cr.bottom)));
  }
  if (!m_rtv) return E_FAIL;
  if (m_opt.mode == OutputMode::Overlay && !m_visible) return S_FALSE;  // hidden: skip the present

  // Letterbox (exact 1:1 in overlay mode where the back buffer matches the captured window).
  float s = std::min(float(m_bbW) / float(srcW), float(m_bbH) / float(srcH));
  float vw = float(srcW) * s, vh = float(srcH) * s;
  D3D11_VIEWPORT vp{(float(m_bbW) - vw) * 0.5f, (float(m_bbH) - vh) * 0.5f, vw, vh, 0.0f, 1.0f};

  ID3D11RenderTargetView* rtv = m_rtv.Get();
  m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
  if (vw < float(m_bbW) - 0.5f || vh < float(m_bbH) - 0.5f) {
    const float black[4] = {0, 0, 0, 1};
    m_ctx->ClearRenderTargetView(rtv, black);
  }
  m_ctx->RSSetViewports(1, &vp);
  m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_ctx->IASetInputLayout(nullptr);
  m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
  m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
  m_ctx->PSSetShaderResources(0, 1, &srv);
  ID3D11SamplerState* samp = m_sampler.Get();
  m_ctx->PSSetSamplers(0, 1, &samp);
  m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  m_ctx->RSSetState(nullptr);
  m_ctx->Draw(3, 0);
  ID3D11ShaderResourceView* nullSrv = nullptr;
  m_ctx->PSSetShaderResources(0, 1, &nullSrv);

  UINT interval = m_opt.vsync ? 1 : 0;
  UINT flags = (!m_opt.vsync && m_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
  HRESULT hr = m_swap->Present(interval, flags);
  if (FAILED(hr) || sourceDisplayTime <= 0) return hr;

  // ---- Display latency estimate ----
  const double freq = qpcFrequency();
  if (m_opt.mode == OutputMode::Overlay && m_dcomp) {
    // DWM composes our visual at its next frame; nextEstimatedFrameTime is when that frame is shown.
    DCOMPOSITION_FRAME_STATISTICS fs{};
    if (SUCCEEDED(m_dcomp->GetFrameStatistics(&fs)) && fs.timeFrequency.QuadPart > 0) {
      double next = double(fs.nextEstimatedFrameTime.QuadPart) / double(fs.timeFrequency.QuadPart);
      m_displayLatencyMs = float((next - sourceDisplayTime) * 1000.0);
      m_displayLatencyValid = true;
    }
  } else {
    // Flip-model swap chains report when a given Present() reached the screen.
    UINT count = 0;
    if (SUCCEEDED(m_swap->GetLastPresentCount(&count))) {
      m_pending[m_pendingHead] = {count, sourceDisplayTime};
      m_pendingHead = (m_pendingHead + 1) % 8;
    }
    DXGI_FRAME_STATISTICS st{};
    if (SUCCEEDED(m_swap->GetFrameStatistics(&st)) && st.SyncQPCTime.QuadPart > 0) {
      for (auto& p : m_pending) {
        if (p.presentCount == st.PresentCount && p.sourceTime > 0) {
          m_displayLatencyMs = float((double(st.SyncQPCTime.QuadPart) / freq - p.sourceTime) * 1000.0);
          m_displayLatencyValid = true;
          p.sourceTime = 0;
        }
      }
    }
  }
  return hr;
}

}  // namespace bl
