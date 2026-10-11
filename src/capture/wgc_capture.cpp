#include "capture/wgc_capture.h"

#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "core/log.h"
#include "core/util.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace bl {

struct WgcCapture::Impl {
  wgc::GraphicsCaptureItem item{nullptr};
  wgc::Direct3D11CaptureFramePool pool{nullptr};
  wgc::GraphicsCaptureSession session{nullptr};
  wgd3d::IDirect3DDevice rtDevice{nullptr};
  wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker;
  wgc::GraphicsCaptureItem::Closed_revoker closedRevoker;
  wgc::Direct3D11CaptureFrame held{nullptr};
  winrt::Windows::Graphics::SizeInt32 poolSize{};
  bool recreatePending = false;
  int buffers = 2;
  HANDLE event = nullptr;
  std::atomic<int64_t> lastArrival{0};  // QPC ticks of the latest FrameArrived
  std::atomic<bool> closed{false};
  bool running = false;
};

namespace {

wgd3d::IDirect3DDevice makeWinrtDevice(ID3D11Device* device) {
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
  winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
  winrt::com_ptr<::IInspectable> insp;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()));
  return insp.as<wgd3d::IDirect3DDevice>();
}

std::string describe(const winrt::hresult_error& e) {
  return hrToString(e.code()) + " " + toUtf8(e.message());
}

}  // namespace

WgcCapture::WgcCapture() : m(std::make_unique<Impl>()) {
  m->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

WgcCapture::~WgcCapture() {
  stop();
  if (m->event) CloseHandle(m->event);
}

bool WgcCapture::isSupported() {
  try {
    return wgc::GraphicsCaptureSession::IsSupported();
  } catch (...) {
    return false;
  }
}

bool WgcCapture::borderlessSupported() {
  try {
    return ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
  } catch (...) {
    return false;
  }
}

bool WgcCapture::minUpdateIntervalSupported() {
  try {
    return ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval");
  } catch (...) {
    return false;
  }
}

static bool startCommon(WgcCapture::Impl& m, ID3D11Device* device, const CaptureOptions& opt, std::string* error) {
  try {
    m.rtDevice = makeWinrtDevice(device);
    m.poolSize = m.item.Size();
    m.buffers = opt.bufferCount;
    m.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        m.rtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, opt.bufferCount, m.poolSize);
    m.session = m.pool.CreateCaptureSession(m.item);

    HANDLE ev = m.event;
    auto* arrival = &m.lastArrival;
    m.frameRevoker = m.pool.FrameArrived(winrt::auto_revoke, [ev, arrival](auto&&, auto&&) {
      arrival->store(qpcTicks());
      SetEvent(ev);
    });
    m.closedRevoker = m.item.Closed(winrt::auto_revoke, [&m, ev](auto&&, auto&&) {
      m.closed = true;
      SetEvent(ev);
    });

    try {
      m.session.IsCursorCaptureEnabled(opt.captureCursor);
    } catch (...) {
    }
    if (opt.hideBorder && WgcCapture::borderlessSupported()) {
      try {
        // Unpackaged desktop apps are granted border-less capture without a prompt on Windows 11.
        auto status = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
        (void)status;
        m.session.IsBorderRequired(false);
      } catch (const winrt::hresult_error& e) {
        logWarn("Border-less capture not available: {}", describe(e));
      }
    }
    if (WgcCapture::minUpdateIntervalSupported()) {
      try {
        // Ask for every frame the window produces (default throttling can cap high refresh rates).
        m.session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{10000});  // 1 ms
      } catch (const winrt::hresult_error& e) {
        logWarn("MinUpdateInterval not applied: {}", describe(e));
      }
    }
    m.closed = false;
    m.session.StartCapture();
    m.running = true;
    logInfo("Capture started ({}x{}, {} buffers)", m.poolSize.Width, m.poolSize.Height, opt.bufferCount);
    return true;
  } catch (const winrt::hresult_error& e) {
    if (error) *error = describe(e);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
  }
  return false;
}

bool WgcCapture::startWindow(HWND target, ID3D11Device* device, const CaptureOptions& opt, std::string* error) {
  stop();
  if (!isSupported()) {
    if (error) *error = "Windows Graphics Capture is not supported on this system (Windows 10 1903+ required).";
    return false;
  }
  if (!IsWindow(target)) {
    if (error) *error = "The selected window no longer exists.";
    return false;
  }
  try {
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                  winrt::put_abi(m->item)));
  } catch (const winrt::hresult_error& e) {
    if (error) *error = "Windows refused to create a capture item for this window: " + describe(e);
    return false;
  }
  if (!startCommon(*m, device, opt, error)) {
    stop();
    return false;
  }
  return true;
}

bool WgcCapture::startMonitor(HMONITOR monitor, ID3D11Device* device, const CaptureOptions& opt, std::string* error) {
  stop();
  if (!isSupported()) {
    if (error) *error = "Windows Graphics Capture is not supported on this system.";
    return false;
  }
  try {
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                   winrt::put_abi(m->item)));
  } catch (const winrt::hresult_error& e) {
    if (error) *error = describe(e);
    return false;
  }
  if (!startCommon(*m, device, opt, error)) {
    stop();
    return false;
  }
  return true;
}

void WgcCapture::stop() {
  m->frameRevoker.revoke();
  m->closedRevoker.revoke();
  m->held = nullptr;
  try {
    if (m->session) m->session.Close();
  } catch (...) {
  }
  try {
    if (m->pool) m->pool.Close();
  } catch (...) {
  }
  m->session = nullptr;
  m->pool = nullptr;
  m->item = nullptr;
  m->rtDevice = nullptr;
  if (m->running) logInfo("Capture stopped");
  m->running = false;
  m->recreatePending = false;
}

bool WgcCapture::active() const { return m->running; }
bool WgcCapture::targetClosed() const { return m->closed; }
HANDLE WgcCapture::frameEvent() const { return m->event; }

bool WgcCapture::acquireLatest(CapturedFrame& out) {
  if (!m->running) return false;
  try {
    // Release the frame we handed out last time before touching the pool.
    m->held = nullptr;
    if (m->recreatePending) {
      m->pool.Recreate(m->rtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, m->buffers, m->poolSize);
      m->recreatePending = false;
      logInfo("Capture pool resized to {}x{}", m->poolSize.Width, m->poolSize.Height);
    }

    wgc::Direct3D11CaptureFrame latest{nullptr};
    int dropped = 0;
    for (;;) {
      auto f = m->pool.TryGetNextFrame();
      if (!f) break;
      if (latest) {
        ++dropped;
        latest.Close();
      }
      latest = f;
    }
    if (!latest) return false;

    auto cs = latest.ContentSize();
    if (cs.Width != m->poolSize.Width || cs.Height != m->poolSize.Height) {
      m->poolSize = cs;
      m->recreatePending = true;  // applied on the next acquire, after this frame is released
    }

    auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&tex)));
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);

    const int64_t t100ns = latest.SystemRelativeTime().count();
    out.texture = tex;
    out.width = std::min<uint32_t>(uint32_t(std::max(cs.Width, 0)), d.Width);
    out.height = std::min<uint32_t>(uint32_t(std::max(cs.Height, 0)), d.Height);
    out.time = qpcSecondsFrom100ns(t100ns);
    out.arrivalTime = double(m->lastArrival.load()) / qpcFrequency();
    out.wakeLatencyMs = std::max(0.0, (qpcSeconds() - out.arrivalTime) * 1000.0);
    out.dropped = dropped;
    m->held = latest;
    return out.width > 0 && out.height > 0;
  } catch (const winrt::hresult_error& e) {
    logError("Frame acquire failed: {}", describe(e));
    return false;
  }
}

}  // namespace bl
