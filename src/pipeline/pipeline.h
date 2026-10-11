#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "core/settings.h"

namespace bl {

struct CaptureTarget {
  HWND hwnd = nullptr;
  std::wstring title;
  std::wstring exe;
};

enum class PipelineState { Idle, Starting, Running, Error };
const char* pipelineStateName(PipelineState s);

struct GraphRing {
  static constexpr int kSize = 240;
  float values[kSize]{};
  int head = 0;
  int count = 0;
  void push(float v) {
    values[head] = v;
    head = (head + 1) % kSize;
    if (count < kSize) ++count;
  }
  float at(int i) const { return values[(head - count + i + kSize * 2) % kSize]; }  // 0 = oldest
};

struct PipelineSnapshot {
  PipelineState state = PipelineState::Idle;
  bool deviceReady = false;
  std::string adapterName;
  std::wstring targetTitle, targetExe;
  HWND targetHwnd = nullptr;

  uint32_t srcW = 0, srcH = 0;
  uint32_t outW = 0, outH = 0;
  OutputMode outputMode = OutputMode::Overlay;
  bool outputVisible = false;
  bool tearing = false;

  float captureFps = 0;         // frames delivered by WGC per second
  float presentFps = 0;         // processed frames presented per second
  float captureIntervalMs = 0;  // median interval between captured frames
  float wakeLatencyMs = 0;      // WGC FrameArrived -> picked up by the processing thread
  float cpuFrameMs = 0;         // CPU time to process + present one frame
  float gpuMs = 0;              // GPU time of the blur engine for one frame (timestamp queries)
  float gpuFilterMs = 0;        // GPU time of the filter suite for one frame (0 when bypassed)
  bool filtersActive = false;   // the filter chain processed the last frame
  bool effectsEnabled = true;   // master switch
  float pacingMedianMs = 0, pacingStdMs = 0, pacingP99Ms = 0;  // present-to-present intervals
  float pipelineLatencyMs = 0;  // FrameArrived -> Present() returned (measured)
  float displayLatencyMs = 0;   // estimated: our frame on screen - original frame's target display time
  bool displayLatencyValid = false;
  float shutterMs = 0;
  int segments = 0;
  int samples = 0;
  float coverage = 0;
  float meanBlockError = 0;

  uint64_t framesCaptured = 0, framesPresented = 0, framesDropped = 0;
  uint64_t sceneCuts = 0, discontinuities = 0;
  bool blackFrames = false;
  bool waitingForFrames = false;

  uint64_t vramUsageMB = 0, vramBudgetMB = 0;

  std::string message;  // last status / error text for the UI
  bool messageIsError = false;

  GraphRing graphInterval, graphGpu, graphLatency, graphPresent;

  // Live preview (shared D3D11 texture with keyed mutex, opened by the UI device).
  HANDLE previewHandle = nullptr;
  uint32_t previewWidth = 0, previewHeight = 0;
  uint64_t previewSeq = 0;
  bool lutLoaded = false;
  std::string lutMessage;
};

// Owns the processing thread: D3D11 device, capture session, blur engine and output window.
// All public methods are thread-safe and non-blocking (commands are queued).
class Pipeline {
 public:
  Pipeline();
  ~Pipeline();
  bool start(std::string* error);  // spins up the thread and creates the GPU device (blocks until ready)
  void shutdown();

  void startCapture(const CaptureTarget& target);
  void stopCapture();
  void setSettings(const Settings& s);
  void setPreviewWanted(bool wanted) { m_previewWanted = wanted; }  // UI shows the live preview
  PipelineSnapshot snapshot() const;

 private:
  struct Command {
    enum Type { Start, Stop } type;
    CaptureTarget target;
  };
  struct Impl;
  void threadMain();

  std::thread m_thread;
  std::atomic<bool> m_quit{false};
  std::atomic<bool> m_previewWanted{false};
  HANDLE m_cmdEvent = nullptr;
  HANDLE m_readyEvent = nullptr;
  std::string m_initError;
  bool m_initOk = false;

  mutable std::mutex m_mutex;  // guards everything below
  std::deque<Command> m_commands;
  Settings m_settings;
  uint64_t m_settingsVersion = 0;
  PipelineSnapshot m_snap;
};

}  // namespace bl
