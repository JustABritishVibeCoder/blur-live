#include "pipeline/pipeline.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

#include "capture/wgc_capture.h"
#include "capture/window_list.h"
#include "core/log.h"
#include "core/util.h"
#include "engine/blur_engine.h"
#include "filters/filter_chain.h"
#include "present/output_window.h"

using Microsoft::WRL::ComPtr;

namespace bl {

const char* pipelineStateName(PipelineState s) {
  switch (s) {
    case PipelineState::Idle: return "Idle";
    case PipelineState::Starting: return "Starting";
    case PipelineState::Running: return "Capturing";
    case PipelineState::Error: return "Error";
  }
  return "?";
}

// Everything owned by the processing thread.
struct Pipeline::Impl {
  Pipeline& owner;
  explicit Impl(Pipeline& o) : owner(o) {}

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGIAdapter3> adapter;
  std::string adapterName;
  BlurEngine engine;
  WgcCapture capture;
  OutputWindow output;
  FilterChain filters;

  Settings settings;
  uint64_t settingsVersion = ~0ull;
  CaptureTarget target;
  PipelineState state = PipelineState::Idle;
  std::string message;
  bool messageIsError = false;

  // Frame statistics
  double lastFrameTime = 0;      // capture timestamp of the previous frame
  double lastFrameArrival = 0;   // QPC time we processed the previous frame
  bool settled = true;
  std::vector<double> intervals;  // recent capture intervals (s)
  double medianInterval = 1.0 / 60.0;
  uint64_t framesCaptured = 0, framesPresented = 0, framesDropped = 0, sceneCuts = 0, discontinuities = 0;
  uint64_t fpsCapCount = 0, fpsPresCount = 0;
  double fpsWindowStart = 0;
  float captureFps = 0, presentFps = 0;
  float wakeLatencyMs = 0, cpuFrameMs = 0, pipelineLatencyMs = 0, displayLatencyMs = 0;
  bool displayLatencyValid = false;
  EngineFrameInfo lastInfo;
  EngineGpuStats gpu;
  double blackSince = 0;
  bool blackFrames = false;
  double lastHousekeeping = 0, lastSlowHousekeeping = 0, lastOverlayUpdate = 0;
  uint64_t vramUsage = 0, vramBudget = 0;
  bool lastSceneCutCounted = false;

  // Client-area crop inside the captured frame (title bar / borders removed).
  struct Crop {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool valid = false;
  } crop;

  void updateCrop() {
    crop = {};
    if (!target.hwnd || !IsWindow(target.hwnd)) return;
    RECT v = windowVisualRect(target.hwnd), c = windowClientRect(target.hwnd);
    LONG x = c.left - v.left, y = c.top - v.top, w = c.right - c.left, h = c.bottom - c.top;
    if (x < 0 || y < 0 || w < 16 || h < 16) return;
    crop = {uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), true};
  }

  // Graph samples are pushed here and copied into the snapshot.
  GraphRing gInterval, gGpu, gLatency, gPresent;

  // Filters, frame pacing and live preview
  float gpuFilterMs = 0;
  bool filtersRan = false;
  double lastPresentTime = 0;
  float pacingMedianMs = 0, pacingStdMs = 0, pacingP99Ms = 0;
  double lastPreview = 0;
  uint64_t previewSeq = 0;

  // Engine output -> filter chain (skipped entirely when no effect is active) -> frame to present.
  ID3D11ShaderResourceView* runFilters() {
    filtersRan = false;
    if (!settings.effectsEnabled) return engine.outputSRV();  // master off: untouched capture
    FilterFrameInput in{engine.outputSRV(), engine.width(), engine.height(), engine.frameHistorySRV(),
                        engine.newestSlice(), settings.compareEnabled, settings.compareSplit};
    ID3D11ShaderResourceView* out = filters.process(in);
    filtersRan = out != engine.outputSRV();
    return out;
  }

  void updatePreview(ID3D11ShaderResourceView* frame, double now) {
    if (!owner.m_previewWanted.load() || now - lastPreview < 1.0 / 30.0) return;
    if (filters.updatePreview(frame, engine.width(), engine.height())) {
      ++previewSeq;
      lastPreview = now;
    }
  }

  bool createDevice(std::string* error) {
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> a1;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f6))))
      f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a1));
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(a1.Get(), a1 ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   fls, 2, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
      if (error) *error = "Could not create a Direct3D 11 hardware device: " + hrToString(hr);
      return false;
    }
    // Keep at most one frame queued in the driver: lowest latency.
    ComPtr<IDXGIDevice1> dxgi1;
    if (SUCCEEDED(device.As(&dxgi1))) dxgi1->SetMaximumFrameLatency(1);
    ComPtr<IDXGIDevice> dxgi;
    device.As(&dxgi);
    ComPtr<IDXGIAdapter> a;
    dxgi->GetAdapter(&a);
    a.As(&adapter);
    DXGI_ADAPTER_DESC desc{};
    a->GetDesc(&desc);
    adapterName = toUtf8(desc.Description);
    // Raise GPU scheduling priority a little so blur work is not starved by the game (no admin needed
    // for values up to 7).
    dxgi->SetGPUThreadPriority(7);
    std::string err;
    if (!engine.init(device.Get(), ctx.Get(), &err)) {
      if (error) *error = "Blur engine initialisation failed: " + err;
      return false;
    }
    if (!filters.init(device.Get(), ctx.Get(), &err)) {
      if (error) *error = "Filter suite initialisation failed: " + err;
      return false;
    }
    filters.setSettings(settings.filters);
    logInfo("GPU: {} (feature level {:X})", adapterName, int(device->GetFeatureLevel()));
    return true;
  }

  void setMessage(std::string m, bool isError) {
    if (isError) logError("{}", m);
    else logInfo("{}", m);
    message = std::move(m);
    messageIsError = isError;
  }

  EngineParams buildParams() const {
    EngineParams p;
    p.enabled = settings.effectsEnabled && settings.blurEnabled;  // master switch bypasses blur too
    p.mode = settings.mode;
    double shutter = settings.shutterFollowsCapture ? settings.blurAmount * medianInterval
                                                    : settings.blurAmount / std::max(1.0f, settings.referenceFps);
    p.shutterSeconds = std::clamp(shutter, 0.0, 0.25);
    p.sampleBudget = qualitySamples(settings.quality);
    p.blendFrames = settings.blendFrames;
    p.weighting = settings.weighting;
    p.gamma = settings.gamma;
    p.sceneCutAbs = settings.sceneCutThreshold;
    p.sceneCutRel = 0.5f;
    p.confidenceLow = settings.confidenceLow;
    p.confidenceHigh = settings.confidenceHigh;
    p.maxGapSeconds = settings.maxFrameGapMs / 1000.0;
    return p;
  }

  void resetCounters() {
    lastFrameTime = 0;
    lastFrameArrival = qpcSeconds();
    settled = true;
    intervals.clear();
    medianInterval = 1.0 / 60.0;
    framesCaptured = framesPresented = framesDropped = sceneCuts = discontinuities = 0;
    fpsCapCount = fpsPresCount = 0;
    fpsWindowStart = qpcSeconds();
    captureFps = presentFps = 0;
    wakeLatencyMs = cpuFrameMs = pipelineLatencyMs = displayLatencyMs = 0;
    displayLatencyValid = false;
    lastInfo = {};
    blackSince = 0;
    blackFrames = false;
    gInterval = {};
    gGpu = {};
    gLatency = {};
    gPresent = {};
    lastPresentTime = 0;
    pacingMedianMs = pacingStdMs = pacingP99Ms = 0;
    gpuFilterMs = 0;
  }

  OutputOptions outputOptions() const {
    OutputOptions o;
    o.mode = settings.outputMode;
    o.target = target.hwnd;
    o.monitorIndex = settings.outputMonitor;
    o.vsync = settings.vsync;
    o.autoHide = settings.autoHideOverlay;
    o.excludeFromCapture = settings.excludeOutputFromCapture;
    o.cropToClient = settings.cropToClient;
    RECT r = settings.cropToClient ? windowClientRect(target.hwnd) : windowVisualRect(target.hwnd);
    o.initialWidth = uint32_t(std::max<LONG>(320, r.right - r.left));
    o.initialHeight = uint32_t(std::max<LONG>(200, r.bottom - r.top));
    return o;
  }

  void startCapture(const CaptureTarget& t) {
    stopCapture(nullptr);
    target = t;
    state = PipelineState::Starting;
    if (!device) {
      state = PipelineState::Error;
      setMessage("No GPU device is available.", true);
      return;
    }
    if (!IsWindow(t.hwnd)) {
      state = PipelineState::Error;
      setMessage("The selected window no longer exists. Refresh the window list and pick it again.", true);
      return;
    }
    if (IsIconic(t.hwnd))
      logWarn("Target window is minimized; frames will start when it is restored.");

    CaptureOptions co;
    co.captureCursor = settings.captureCursor;
    co.hideBorder = settings.hideCaptureBorder;
    co.bufferCount = settings.framePoolBuffers;
    std::string err;
    if (!capture.startWindow(t.hwnd, device.Get(), co, &err)) {
      state = PipelineState::Error;
      std::string hint;
      if (err.find("E_ACCESSDENIED") != std::string::npos)
        hint = " The game may be running as administrator (start Blur Live as administrator too) or it blocks capture.";
      setMessage("Could not start capture: " + err + hint, true);
      return;
    }
    engine.resetHistory();
    resetCounters();
    updateCrop();
    if (!output.create(device.Get(), ctx.Get(), outputOptions(), &err)) {
      capture.stop();
      state = PipelineState::Error;
      setMessage("Could not create the output window: " + err, true);
      return;
    }
    state = PipelineState::Running;
    setMessage("Capturing \"" + toUtf8(t.title) + "\"", false);
  }

  void stopCapture(const char* reason, bool isError = false) {
    const bool wasActive = capture.active() || output.valid();
    capture.stop();
    output.destroy();
    engine.resetHistory();
    if (wasActive || reason) {
      state = isError ? PipelineState::Error : PipelineState::Idle;
      if (reason) setMessage(reason, isError);
    }
  }

  void recoverDevice(const std::string& why) {
    logError("GPU device problem: {}", why);
    capture.stop();
    output.destroy();
    engine.shutdown();
    filters.shutdown();
    ctx.Reset();
    device.Reset();
    adapter.Reset();
    std::string err;
    if (!createDevice(&err)) {
      state = PipelineState::Error;
      setMessage("The GPU device was lost and could not be recreated: " + err, true);
      return;
    }
    state = PipelineState::Error;
    setMessage("The GPU device was reset (" + why + "). Capture stopped; press Start to resume.", true);
  }

  void applySettings(const Settings& s) {
    const Settings old = settings;
    settings = s;
    if (!(old.filters == s.filters)) filters.setSettings(s.filters);  // live: no capture restart
    if (state != PipelineState::Running) return;
    const bool outputChanged = old.outputMode != s.outputMode || old.outputMonitor != s.outputMonitor ||
                               old.excludeOutputFromCapture != s.excludeOutputFromCapture;
    const bool captureChanged = old.captureCursor != s.captureCursor || old.hideCaptureBorder != s.hideCaptureBorder ||
                                old.framePoolBuffers != s.framePoolBuffers;
    if (captureChanged) {
      CaptureTarget t = target;
      startCapture(t);
      return;
    }
    if (outputChanged) {
      std::string err;
      output.destroy();
      if (!output.create(device.Get(), ctx.Get(), outputOptions(), &err)) {
        stopCapture(("Could not recreate the output window: " + err).c_str(), true);
        return;
      }
      settled = false;
    }
    output.setVsync(s.vsync);
    output.setAutoHide(s.autoHideOverlay);
    output.setCropToClient(s.cropToClient);
    if (old.mode != s.mode) engine.resetHistory();
  }

  void processFrame() {
    CapturedFrame f;
    if (!capture.acquireLatest(f)) return;
    const double t0 = qpcSeconds();

    ++framesCaptured;
    ++fpsCapCount;
    framesDropped += uint64_t(f.dropped);
    if (lastFrameTime > 0) {
      double dt = f.time - lastFrameTime;
      if (dt > 0 && dt < 1.0) {
        intervals.push_back(dt);
        if (intervals.size() > 31) intervals.erase(intervals.begin());
        std::vector<double> tmp = intervals;
        std::nth_element(tmp.begin(), tmp.begin() + std::ptrdiff_t(tmp.size() / 2), tmp.end());
        medianInterval = tmp[tmp.size() / 2];
        gInterval.push(float(dt * 1000.0));
      }
    }
    lastFrameTime = f.time;
    wakeLatencyMs = float(f.wakeLatencyMs);

    EngineFrameInfo info;
    const bool useCrop = settings.cropToClient && crop.valid && crop.x + crop.w <= f.width && crop.y + crop.h <= f.height;
    const bool ok = useCrop ? engine.submitRegion(f.texture.Get(), crop.x, crop.y, crop.w, crop.h, f.time, buildParams(), &info)
                            : engine.submitFrame(f.texture.Get(), f.width, f.height, f.time, buildParams(), &info);
    if (!ok) {
      HRESULT reason = device->GetDeviceRemovedReason();
      if (FAILED(reason)) {
        recoverDevice(hrToString(reason));
        return;
      }
      logWarn("Frame {}x{} could not be processed", f.width, f.height);
      return;
    }
    lastInfo = info;
    if (info.discontinuity && framesCaptured > 1) ++discontinuities;

    ID3D11ShaderResourceView* frame = runFilters();
    HRESULT hr = output.present(frame, engine.width(), engine.height(), f.time);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
      recoverDevice(hrToString(device->GetDeviceRemovedReason()));
      return;
    }
    if (hr == S_OK || hr == DXGI_STATUS_OCCLUDED) {
      ++framesPresented;
      ++fpsPresCount;
    }
    const double t1 = qpcSeconds();
    cpuFrameMs = float((t1 - t0) * 1000.0);
    pipelineLatencyMs = float((t1 - f.arrivalTime) * 1000.0);
    gLatency.push(pipelineLatencyMs);
    displayLatencyValid = output.displayLatencyMs(displayLatencyMs);
    if (lastPresentTime > 0) gPresent.push(float((t1 - lastPresentTime) * 1000.0));
    lastPresentTime = t1;
    updatePreview(frame, t1);
    lastFrameArrival = t1;
    settled = false;

    gpu = engine.pollGpuStats();
    gpuFilterMs = filtersRan ? filters.pollGpuMs() : 0.0f;
    if (gpu.timingValid) gGpu.push(gpu.gpuMs);
    if (gpu.motionValid) {
      if (gpu.sceneCut && !lastSceneCutCounted) ++sceneCuts;
      lastSceneCutCounted = gpu.sceneCut;
      if (gpu.meanLuma < 0.004f) {
        if (blackSince == 0) blackSince = t1;
        blackFrames = t1 - blackSince > 2.0;
      } else {
        blackSince = 0;
        blackFrames = false;
      }
    }
  }

  void idleWork() {
    // When the source stops producing frames (paused game, static menu), drop the motion trail and
    // show the newest frame sharp, so stale blur does not linger on screen.
    const double now = qpcSeconds();
    double settleAfter = std::max(0.05, 2.0 * buildParams().shutterSeconds + medianInterval);
    if (!settled && engine.hasOutput() && now - lastFrameArrival > settleAfter) {
      engine.renderNewestUnblurred();
      ID3D11ShaderResourceView* frame = runFilters();
      output.present(frame, engine.width(), engine.height());
      updatePreview(frame, now);
      settled = true;
    }
  }

  void housekeeping() {
    const double now = qpcSeconds();
    if (state == PipelineState::Running) {
      if (capture.targetClosed() || !IsWindow(target.hwnd)) {
        stopCapture("Capture stopped: the window was closed.");
        return;
      }
      if (output.closedByUser()) {
        stopCapture("Capture stopped: the output window was closed.");
        return;
      }
      if (now - lastOverlayUpdate > 0.016) {
        updateCrop();
        output.update();
        lastOverlayUpdate = now;
      }
    }
    if (now - fpsWindowStart >= 0.5 && gPresent.count > 8) {
      // Frame pacing over the last 240 presents: median, standard deviation, 99th percentile.
      std::vector<float> v(size_t(gPresent.count));
      for (int i = 0; i < gPresent.count; ++i) v[size_t(i)] = gPresent.at(i);
      double mean = 0, var = 0;
      for (float x : v) mean += x;
      mean /= double(v.size());
      for (float x : v) var += (x - mean) * (x - mean);
      pacingStdMs = float(std::sqrt(var / double(v.size())));
      std::sort(v.begin(), v.end());
      pacingMedianMs = v[v.size() / 2];
      pacingP99Ms = v[std::min(v.size() - 1, v.size() * 99 / 100)];
    }
    if (now - fpsWindowStart >= 0.5) {
      double span = now - fpsWindowStart;
      captureFps = float(double(fpsCapCount) / span);
      presentFps = float(double(fpsPresCount) / span);
      fpsCapCount = fpsPresCount = 0;
      fpsWindowStart = now;
    }
    if (now - lastSlowHousekeeping > 1.0 && adapter) {
      DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
      if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
        vramUsage = mi.CurrentUsage / (1024 * 1024);
        vramBudget = mi.Budget / (1024 * 1024);
      }
      lastSlowHousekeeping = now;
    }
  }

  void publish() {
    std::lock_guard lock(owner.m_mutex);
    PipelineSnapshot& s = owner.m_snap;
    s.state = state;
    s.deviceReady = device != nullptr;
    s.adapterName = adapterName;
    s.targetTitle = target.title;
    s.targetExe = target.exe;
    s.targetHwnd = target.hwnd;
    s.srcW = engine.width();
    s.srcH = engine.height();
    s.outW = output.width();
    s.outH = output.height();
    s.outputMode = settings.outputMode;
    s.outputVisible = output.valid() && output.isVisible();
    s.tearing = output.tearingSupported();
    s.captureFps = captureFps;
    s.presentFps = presentFps;
    s.captureIntervalMs = float(medianInterval * 1000.0);
    s.wakeLatencyMs = wakeLatencyMs;
    s.cpuFrameMs = cpuFrameMs;
    s.gpuMs = gpu.gpuMs;
    s.gpuFilterMs = gpuFilterMs;
    s.filtersActive = filtersRan;
    s.effectsEnabled = settings.effectsEnabled;
    s.pacingMedianMs = pacingMedianMs;
    s.pacingStdMs = pacingStdMs;
    s.pacingP99Ms = pacingP99Ms;
    s.graphPresent = gPresent;
    s.previewHandle = filters.previewHandle();
    s.previewWidth = filters.previewWidth();
    s.previewHeight = filters.previewHeight();
    s.previewSeq = previewSeq;
    s.lutLoaded = filters.lutLoaded();
    s.lutMessage = filters.lutMessage();
    s.pipelineLatencyMs = pipelineLatencyMs;
    s.displayLatencyMs = displayLatencyMs;
    s.displayLatencyValid = displayLatencyValid;
    s.shutterMs = float(buildParams().shutterSeconds * 1000.0);
    s.segments = lastInfo.segments;
    s.samples = lastInfo.samples;
    s.coverage = lastInfo.coverage;
    s.meanBlockError = gpu.meanBlockError;
    s.framesCaptured = framesCaptured;
    s.framesPresented = framesPresented;
    s.framesDropped = framesDropped;
    s.sceneCuts = sceneCuts;
    s.discontinuities = discontinuities;
    s.blackFrames = blackFrames;
    s.waitingForFrames = state == PipelineState::Running && qpcSeconds() - lastFrameArrival > 1.0;
    s.vramUsageMB = vramUsage;
    s.vramBudgetMB = vramBudget;
    s.message = message;
    s.messageIsError = messageIsError;
    s.graphInterval = gInterval;
    s.graphGpu = gGpu;
    s.graphLatency = gLatency;
  }
};

Pipeline::Pipeline() {
  m_cmdEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

Pipeline::~Pipeline() {
  shutdown();
  CloseHandle(m_cmdEvent);
  CloseHandle(m_readyEvent);
}

bool Pipeline::start(std::string* error) {
  if (m_thread.joinable()) return m_initOk;
  m_quit = false;
  m_thread = std::thread([this] { threadMain(); });
  WaitForSingleObject(m_readyEvent, INFINITE);
  if (!m_initOk && error) *error = m_initError;
  return m_initOk;
}

void Pipeline::shutdown() {
  if (!m_thread.joinable()) return;
  m_quit = true;
  SetEvent(m_cmdEvent);
  m_thread.join();
}

void Pipeline::startCapture(const CaptureTarget& target) {
  {
    std::lock_guard lock(m_mutex);
    m_commands.push_back({Command::Start, target});
  }
  SetEvent(m_cmdEvent);
}

void Pipeline::stopCapture() {
  {
    std::lock_guard lock(m_mutex);
    m_commands.push_back({Command::Stop, {}});
  }
  SetEvent(m_cmdEvent);
}

void Pipeline::setSettings(const Settings& s) {
  {
    std::lock_guard lock(m_mutex);
    m_settings = s;
    ++m_settingsVersion;
  }
  SetEvent(m_cmdEvent);
}

PipelineSnapshot Pipeline::snapshot() const {
  std::lock_guard lock(m_mutex);
  return m_snap;
}

void Pipeline::threadMain() {
  SetThreadDescription(GetCurrentThread(), L"BlurLive pipeline");
  // Above-normal priority keeps frame turnaround tight while a game saturates the CPU.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  auto impl = std::make_unique<Impl>(*this);
  {
    std::lock_guard lock(m_mutex);
    impl->settings = m_settings;
    impl->settingsVersion = m_settingsVersion;
  }
  m_initOk = impl->createDevice(&m_initError);
  impl->publish();
  SetEvent(m_readyEvent);

  while (!m_quit) {
    HANDLE handles[2] = {m_cmdEvent, impl->capture.frameEvent()};
    const bool running = impl->state == PipelineState::Running;
    DWORD n = running ? 2 : 1;
    MsgWaitForMultipleObjectsEx(n, handles, running ? 4 : 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    std::deque<Command> cmds;
    Settings newSettings;
    bool settingsChanged = false;
    {
      std::lock_guard lock(m_mutex);
      cmds.swap(m_commands);
      if (m_settingsVersion != impl->settingsVersion) {
        newSettings = m_settings;
        impl->settingsVersion = m_settingsVersion;
        settingsChanged = true;
      }
    }
    if (settingsChanged) impl->applySettings(newSettings);
    for (auto& c : cmds) {
      if (c.type == Command::Start) impl->startCapture(c.target);
      else impl->stopCapture("Capture stopped.");
    }

    if (impl->state == PipelineState::Running && impl->device) {
      impl->processFrame();
      if (impl->state == PipelineState::Running) impl->idleWork();
    }
    impl->housekeeping();
    impl->publish();
  }

  impl->stopCapture(nullptr);
  impl->engine.shutdown();
  impl->filters.shutdown();
  impl.reset();
  if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace bl
