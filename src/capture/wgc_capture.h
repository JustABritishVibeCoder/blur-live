#pragma once
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bl {

struct CaptureOptions {
  bool captureCursor = false;
  bool hideBorder = true;   // Windows 11: request a border-less capture (no yellow outline)
  int bufferCount = 2;      // frame pool size
};

struct CapturedFrame {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;  // valid until the next acquireLatest()/stop()
  uint32_t width = 0;     // content size (may be smaller than the texture)
  uint32_t height = 0;
  // WGC SystemRelativeTime in QPC seconds. Measured on Windows 11 this is DWM's target display time
  // for the composition that produced the frame (it lies slightly in the future when the frame
  // arrives), so it is a stable presentation timestamp: ideal for motion timing.
  double time = 0;
  double arrivalTime = 0; // QPC seconds when WGC signalled the frame (FrameArrived)
  double wakeLatencyMs = 0;  // FrameArrived -> acquired by the processing thread
  int dropped = 0;        // older frames skipped in favour of this one
};

// Windows Graphics Capture session for one window (or monitor). Frames arrive on a WGC thread and
// only signal an event; the owner thread pulls the newest frame, so frames never pile up.
class WgcCapture {
 public:
  WgcCapture();
  ~WgcCapture();
  WgcCapture(const WgcCapture&) = delete;
  WgcCapture& operator=(const WgcCapture&) = delete;

  static bool isSupported();
  static bool borderlessSupported();
  static bool minUpdateIntervalSupported();

  bool startWindow(HWND target, ID3D11Device* device, const CaptureOptions& opt, std::string* error);
  bool startMonitor(HMONITOR monitor, ID3D11Device* device, const CaptureOptions& opt, std::string* error);
  void stop();

  bool active() const;
  bool targetClosed() const;  // the captured item went away (window closed)
  HANDLE frameEvent() const;  // auto-reset, signalled when a new frame (or close) arrives
  bool acquireLatest(CapturedFrame& out);

  struct Impl;  // opaque (keeps WinRT headers out of this header)

 private:
  std::unique_ptr<Impl> m;
};

}  // namespace bl
