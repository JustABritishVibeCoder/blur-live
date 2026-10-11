// Integration tests that exercise real Windows Graphics Capture, the output window and the full
// pipeline against a test window rendered by this process. Included from tests.cpp.
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <thread>

// A top-level window rendering animated content with its own D3D11 device and flip swap chain,
// running on its own thread (like a game would).
class TestWindow {
 public:
  enum class Pattern { MovingBar, SolidGreen };

  bool open(int x, int y, int w, int h, Pattern pattern, const wchar_t* title = L"BlurLive Test Window") {
    m_pattern = pattern;
    m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread = std::thread([=, this] { run(x, y, w, h, title); });
    WaitForSingleObject(m_ready, 5000);
    CloseHandle(m_ready);
    return m_hwnd != nullptr;
  }
  void close() {
    if (!m_thread.joinable()) return;
    m_quit = true;
    if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    m_thread.join();
    m_hwnd = nullptr;
  }
  ~TestWindow() { close(); }
  HWND hwnd() const { return m_hwnd; }
  uint64_t framesRendered() const { return m_frames; }

 private:
  static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
      DestroyWindow(h);
      return 0;
    }
    if (m == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(h, m, w, l);
  }

  void run(int x, int y, int w, int h, const wchar_t* title) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BlurLiveTestWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                      D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    ComPtr<IDXGIDevice> dxgi;
    dev.As(&dxgi);
    ComPtr<IDXGIAdapter> ad;
    dxgi->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> fac;
    ad->GetParent(IID_PPV_ARGS(&fac));
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    fac->CreateSwapChainForHwnd(dev.Get(), hwnd, &d, nullptr, nullptr, &sc);
    ComPtr<ID3D11Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    ComPtr<ID3D11DeviceContext1> ctx1;
    ctx.As(&ctx1);
    m_hwnd = hwnd;
    SetEvent(m_ready);

    int frame = 0;
    MSG msg;
    while (!m_quit) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) m_quit = true;
        DispatchMessageW(&msg);
      }
      if (m_quit) break;
      if (m_pattern == Pattern::SolidGreen) {
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), green);
      } else {
        const float bg[4] = {0.15f, 0.15f, 0.2f, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), bg);
        const float white[4] = {1, 1, 1, 1};
        int bx = (frame * 8) % (w + 80) - 80;
        D3D11_RECT rects[2] = {{bx, h / 4, bx + 80, h * 3 / 4}, {(frame * 3) % w, 10, (frame * 3) % w + 20, 30}};
        ctx1->ClearView(rtv.Get(), white, rects, 2);
      }
      ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      sc->Present(1, 0);
      ++frame;
      ++m_frames;
    }
    rtv.Reset();
    sc.Reset();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
  }

  Pattern m_pattern = Pattern::MovingBar;
  std::thread m_thread;
  std::atomic<bool> m_quit{false};
  std::atomic<HWND> m_hwnd{nullptr};
  std::atomic<uint64_t> m_frames{0};
  HANDLE m_ready = nullptr;
};

static void pumpFor(double seconds) {
  double end = qpcSeconds() + seconds;
  while (qpcSeconds() < end) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    Sleep(5);
  }
}

#define REQUIRE_WGC()                                                   \
  REQUIRE_GPU();                                                        \
  if (!WgcCapture::isSupported()) {                                     \
    std::printf("    SKIP: Windows Graphics Capture not supported\n");  \
    return;                                                             \
  }

static double g_lastLeadMs = 0;  // SystemRelativeTime - arrival (DWM target-time lead)

static int captureFor(WgcCapture& cap, double seconds, CapturedFrame& last, double& latencySum, int& dropped) {
  int frames = 0;
  double end = qpcSeconds() + seconds;
  while (qpcSeconds() < end) {
    { HANDLE ev = cap.frameEvent(); MsgWaitForMultipleObjects(1, &ev, FALSE, 20, QS_ALLINPUT); }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    CapturedFrame f;
    if (cap.acquireLatest(f)) {
      ++frames;
      latencySum += f.wakeLatencyMs;
      g_lastLeadMs = (f.time - f.arrivalTime) * 1000.0;
      dropped += f.dropped;
      last = f;
    }
  }
  return frames;
}

TEST(capture_window_frames_and_restart) {
  REQUIRE_WGC();
  TestWindow tw;
  CHECK(tw.open(120, 120, 800, 450, TestWindow::Pattern::MovingBar));
  pumpFor(0.3);
  for (int round = 0; round < 3; ++round) {
    WgcCapture cap;
    std::string err;
    CaptureOptions opt;
    bool ok = cap.startWindow(tw.hwnd(), gpu().dev.Get(), opt, &err);
    CHECK(ok, "%s", err.c_str());
    if (!ok) return;
    CapturedFrame last;
    double lat = 0;
    int dropped = 0;
    uint64_t rendered0 = tw.framesRendered();
    int frames = captureFor(cap, 1.5, last, lat, dropped);
    uint64_t rendered = tw.framesRendered() - rendered0;
    std::printf("    round %d: %d frames captured in 1.5 s (%.0f fps; window rendered %.0f fps), %ux%u, "
                "FrameArrived->acquire %.3f ms, SystemRelativeTime lead over arrival %.2f ms, dropped %d\n",
                round, frames, frames / 1.5, rendered / 1.5, last.width, last.height, frames ? lat / frames : 0.0,
                g_lastLeadMs, dropped);
    CHECK(frames >= 30, "frames=%d", frames);
    CHECK(last.width >= 790 && last.width <= 830 && last.height >= 440 && last.height <= 500, "%ux%u", last.width,
          last.height);
    cap.stop();
    CHECK(!cap.active());
  }
}

// Diagnostic: distribution of WGC SystemRelativeTime deltas between consecutive acquired frames.
TEST(capture_timestamp_deltas) {
  REQUIRE_WGC();
  TestWindow tw;
  CHECK(tw.open(120, 120, 800, 450, TestWindow::Pattern::MovingBar));
  pumpFor(0.3);
  WgcCapture cap;
  std::string err;
  CHECK(cap.startWindow(tw.hwnd(), gpu().dev.Get(), CaptureOptions{}, &err), "%s", err.c_str());
  double last = 0;
  int frames = 0, zero = 0, negative = 0, big = 0;
  double minDt = 1e9, maxDt = 0;
  double end = qpcSeconds() + 2.0;
  while (qpcSeconds() < end) {
    HANDLE ev = cap.frameEvent();
    MsgWaitForMultipleObjects(1, &ev, FALSE, 20, QS_ALLINPUT);
    CapturedFrame f;
    if (!cap.acquireLatest(f)) continue;
    if (last > 0) {
      double dt = f.time - last;
      if (dt == 0) ++zero;
      else if (dt < 0) ++negative;
      else if (dt > 0.1) ++big;
      if (dt > 0) {
        minDt = std::min(minDt, dt);
        maxDt = std::max(maxDt, dt);
      }
    }
    last = f.time;
    ++frames;
  }
  std::printf("    %d frames: %d zero deltas, %d negative, %d > 100 ms, positive dt range %.3f .. %.3f ms\n", frames, zero,
              negative, big, minDt * 1000, maxDt * 1000);
  CHECK(frames > 100);
}

TEST(capture_detects_closed_window) {
  REQUIRE_WGC();
  TestWindow tw;
  CHECK(tw.open(140, 140, 640, 360, TestWindow::Pattern::MovingBar));
  pumpFor(0.2);
  WgcCapture cap;
  std::string err;
  CHECK(cap.startWindow(tw.hwnd(), gpu().dev.Get(), CaptureOptions{}, &err), "%s", err.c_str());
  CapturedFrame f;
  double lat = 0;
  int dropped = 0;
  captureFor(cap, 0.5, f, lat, dropped);
  tw.close();
  double end = qpcSeconds() + 2.0;
  while (!cap.targetClosed() && qpcSeconds() < end) {
    { HANDLE ev = cap.frameEvent(); MsgWaitForMultipleObjects(1, &ev, FALSE, 20, QS_ALLINPUT); }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
  }
  CHECK(cap.targetClosed());
  cap.stop();
  // Starting on a dead window must fail cleanly.
  CHECK(!cap.startWindow(tw.hwnd(), gpu().dev.Get(), CaptureOptions{}, &err));
}

TEST(window_enumeration) {
  TestWindow tw;
  CHECK(tw.open(160, 160, 500, 300, TestWindow::Pattern::MovingBar, L"BlurLive Enum Probe"));
  pumpFor(0.2);
  auto all = enumerateCaptureTargets(0, true);
  bool found = false;
  for (auto& w : all)
    if (w.hwnd == tw.hwnd()) {
      found = true;
      CHECK(w.title == L"BlurLive Enum Probe");
      CHECK(w.exeName == L"BlurLiveTests.exe");
    }
  CHECK(found);
  auto mine = enumerateCaptureTargets(GetCurrentProcessId(), true);
  for (auto& w : mine) CHECK(w.pid != GetCurrentProcessId());
  std::printf("    %zu capturable windows, %zu monitors\n", all.size(), enumerateMonitors().size());
  CHECK(!enumerateMonitors().empty());
}

// Overlay correctness: the overlay must be visible on screen (checked by capturing the monitor) and
// must be transparent to hit-testing so the game underneath receives the mouse.
TEST(overlay_visible_and_click_through) {
  REQUIRE_WGC();
  TestWindow tw;
  CHECK(tw.open(200, 200, 640, 360, TestWindow::Pattern::SolidGreen, L"BlurLive Overlay Target"));
  pumpFor(0.3);
  RECT vr = windowClientRect(tw.hwnd());  // overlay aligns to the client area by default
  POINT center{(vr.left + vr.right) / 2, (vr.top + vr.bottom) / 2};
  HWND under = WindowFromPoint(center);
  if (under != tw.hwnd() && GetAncestor(under, GA_ROOT) != tw.hwnd()) {
    std::printf("    SKIP: another window covers the test area\n");
    return;
  }

  OutputWindow out;
  OutputOptions oo;
  oo.mode = OutputMode::Overlay;
  oo.target = tw.hwnd();
  oo.autoHide = false;
  std::string err;
  CHECK(out.create(gpu().dev.Get(), gpu().ctx.Get(), oo, &err), "%s", err.c_str());
  if (!out.valid()) return;
  CHECK(out.width() == UINT(vr.right - vr.left) && out.height() == UINT(vr.bottom - vr.top), "%ux%u", out.width(),
        out.height());

  // Magenta source frame.
  auto magenta = makeTexture(std::vector<uint32_t>(64 * 64, 0xFFFF00FFu), 64, 64);
  ComPtr<ID3D11ShaderResourceView> srv;
  gpu().dev->CreateShaderResourceView(magenta.Get(), nullptr, &srv);
  for (int i = 0; i < 10; ++i) {
    HRESULT hr = out.present(srv.Get(), 64, 64);
    CHECK(SUCCEEDED(hr), "present hr=%s", hrToString(hr).c_str());
    pumpFor(0.02);
  }
  CHECK(out.isVisible());

  // Hit-testing passes through the overlay to the window below.
  HWND hit = WindowFromPoint(center);
  CHECK(hit == tw.hwnd() || GetAncestor(hit, GA_ROOT) == tw.hwnd(), "WindowFromPoint returned the overlay");

  // What the user actually sees: capture the monitor and sample the centre pixel.
  WgcCapture mon;
  HMONITOR hm = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
  bool ok = mon.startMonitor(hm, gpu().dev.Get(), CaptureOptions{}, &err);
  CHECK(ok, "%s", err.c_str());
  if (ok) {
    CapturedFrame f;
    double lat = 0;
    int dropped = 0;
    for (int i = 0; i < 20 && !f.texture; ++i) {
      out.present(srv.Get(), 64, 64);
      captureFor(mon, 0.05, f, lat, dropped);
    }
    CHECK(f.texture != nullptr);
    if (f.texture) {
      MONITORINFO mi{sizeof(mi)};
      GetMonitorInfoW(hm, &mi);
      D3D11_TEXTURE2D_DESC d;
      f.texture->GetDesc(&d);
      d.BindFlags = 0;
      d.MiscFlags = 0;
      d.Usage = D3D11_USAGE_STAGING;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Texture2D> stg;
      gpu().dev->CreateTexture2D(&d, nullptr, &stg);
      gpu().ctx->CopyResource(stg.Get(), f.texture.Get());
      D3D11_MAPPED_SUBRESOURCE m;
      gpu().ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m);
      int px = center.x - mi.rcMonitor.left, py = center.y - mi.rcMonitor.top;
      uint32_t c = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m.pData) + size_t(py) * m.RowPitch + size_t(px) * 4);
      gpu().ctx->Unmap(stg.Get(), 0);
      int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
      std::printf("    on-screen pixel at overlay centre: R=%d G=%d B=%d (magenta = overlay, green = game)\n", r, g, b);
      CHECK(r > 200 && g < 60 && b > 200, "overlay content not visible on screen");
    }
    mon.stop();
  }
  out.destroy();
}

static bool waitFor(const std::function<bool()>& pred, double seconds) {
  double end = qpcSeconds() + seconds;
  while (qpcSeconds() < end) {
    if (pred()) return true;
    pumpFor(0.02);
  }
  return pred();
}

static void runPipelineScenario(OutputMode mode) {
  TestWindow tw;
  CHECK(tw.open(220, 180, 960, 540, TestWindow::Pattern::MovingBar, L"BlurLive Pipeline Target"));
  pumpFor(0.3);

  Pipeline p;
  std::string err;
  CHECK(p.start(&err), "%s", err.c_str());
  Settings s;
  s.outputMode = mode;
  s.autoHideOverlay = false;
  p.setSettings(s);

  CaptureTarget t{tw.hwnd(), L"BlurLive Pipeline Target", L"BlurLiveTests.exe"};
  for (int round = 0; round < 2; ++round) {
    p.startCapture(t);
    bool running = waitFor([&] { return p.snapshot().state == PipelineState::Running; }, 4.0) &&
                   waitFor([&] { return p.snapshot().framesPresented > 60; }, 4.0);
    pumpFor(1.0);
    auto snap = p.snapshot();
    std::printf("    [%s round %d] state=%s src=%ux%u capture %.0f fps, presented %.0f fps, GPU %.3f ms, CPU %.3f ms, "
                "wake %.3f ms, arrival->present %.2f ms, est. added display latency %s%.2f ms, shutter %.1f ms over %d "
                "segments/%d samples, "
                "dropped %llu\n",
                outputModeName(mode), round, pipelineStateName(snap.state), snap.srcW, snap.srcH, snap.captureFps,
                snap.presentFps, snap.gpuMs, snap.cpuFrameMs, snap.wakeLatencyMs, snap.pipelineLatencyMs,
                snap.displayLatencyValid ? "" : "(n/a) ", snap.displayLatencyMs,
                snap.shutterMs, snap.segments, snap.samples, (unsigned long long)snap.framesDropped);
    CHECK(running && snap.state == PipelineState::Running, "msg=%s", snap.message.c_str());
    CHECK(snap.framesPresented > 60);
    CHECK(snap.gpuMs > 0.0f);
    CHECK(snap.segments >= 1);
    // Live settings change while running.
    s.blurAmount = round == 0 ? 0.5f : 1.5f;
    s.quality = round == 0 ? 3 : 0;
    p.setSettings(s);
    pumpFor(0.3);
    CHECK(p.snapshot().state == PipelineState::Running);

    // Filters switched on live: no capture restart, the chain runs and is timed separately.
    uint64_t presentedBefore = p.snapshot().framesPresented;
    Settings fs = s;
    for (FilterId id : {FilterId::Sharpen, FilterId::Vibrance, FilterId::ShadowsHighlights, FilterId::Bloom})
      fs.filters[id].enabled = true;
    p.setSettings(fs);
    pumpFor(0.5);
    auto withFx = p.snapshot();
    std::printf("    [%s round %d] filters live: active=%d, filter GPU %.3f ms, engine GPU %.3f ms, %.0f fps out, "
                "pacing median %.2f ms sd %.2f ms p99 %.2f ms\n",
                outputModeName(mode), round, int(withFx.filtersActive), withFx.gpuFilterMs, withFx.gpuMs,
                withFx.presentFps, withFx.pacingMedianMs, withFx.pacingStdMs, withFx.pacingP99Ms);
    CHECK(withFx.state == PipelineState::Running && withFx.framesPresented > presentedBefore);
    CHECK(withFx.filtersActive && withFx.gpuFilterMs > 0.0f);
    // Master switch off: capture keeps running, the filter chain is bypassed.
    fs.effectsEnabled = false;
    p.setSettings(fs);
    pumpFor(0.3);
    auto off = p.snapshot();
    CHECK(off.state == PipelineState::Running && !off.filtersActive && !off.effectsEnabled);
    p.setSettings(s);
    pumpFor(0.1);
    p.stopCapture();
    CHECK(waitFor([&] { return p.snapshot().state == PipelineState::Idle; }, 2.0));
  }

  // Target closes while capturing: pipeline must stop cleanly with a message, not crash.
  p.startCapture(t);
  CHECK(waitFor([&] { return p.snapshot().state == PipelineState::Running; }, 4.0));
  CHECK(waitFor([&] { return p.snapshot().framesPresented > 10; }, 4.0));
  tw.close();
  CHECK(waitFor([&] { return p.snapshot().state == PipelineState::Idle; }, 3.0));
  std::printf("    after target closed: state=%s message=\"%s\"\n", pipelineStateName(p.snapshot().state),
              p.snapshot().message.c_str());
  // Starting on a dead window reports an error.
  p.startCapture(t);
  CHECK(waitFor([&] { return p.snapshot().state == PipelineState::Error; }, 2.0));
  p.shutdown();
}

TEST(pipeline_window_mode) {
  REQUIRE_WGC();
  runPipelineScenario(OutputMode::Window);
}

TEST(pipeline_overlay_mode) {
  REQUIRE_WGC();
  runPipelineScenario(OutputMode::Overlay);
}
