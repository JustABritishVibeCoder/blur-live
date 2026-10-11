#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/segment_planner.h"
#include "core/settings.h"
#include "core/weighting.h"

namespace bl {

struct EngineParams {
  bool enabled = true;
  BlurMode mode = BlurMode::MotionFlow;
  double shutterSeconds = 1.0 / 60.0;
  int sampleBudget = 48;
  int blendFrames = 4;
  WeightingParams weighting;
  float gamma = 1.0f;
  float sceneCutAbs = 0.06f;
  float sceneCutRel = 0.50f;
  float confidenceLow = 0.035f;
  float confidenceHigh = 0.09f;
  double maxGapSeconds = 0.1;
};

// Results that the GPU produced for an earlier frame (read back without stalling).
struct EngineGpuStats {
  bool timingValid = false;
  float gpuMs = 0;               // GPU time of the whole engine pass for one frame
  bool motionValid = false;
  float meanBlockError = 0;      // mean best-match error (0..1 luma)
  float meanZeroError = 0;       // mean error without motion compensation
  float meanLuma = 0;
  bool sceneCut = false;
};

struct EngineFrameInfo {
  int segments = 0;
  int samples = 0;
  float coverage = 0;
  bool discontinuity = false;
  bool historyReset = false;
};

// GPU motion-blur engine. Not thread-safe: use from the thread that owns the immediate context.
class BlurEngine {
 public:
  static constexpr int kSlots = 16;        // history ring size
  static constexpr int kMaxSegments = 12;  // frame pairs that a single shutter window may span
  static constexpr int kLevels = 4;        // luma pyramid levels (1/2 .. 1/16 resolution)

  bool init(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error);
  void shutdown();
  void resetHistory();

  // Ingests a captured frame (copied into the history ring) and renders the blurred output.
  // srcW/srcH may be smaller than the texture (WGC pools can be larger than the content).
  bool submitFrame(ID3D11Texture2D* source, uint32_t srcW, uint32_t srcH, double timeSeconds,
                   const EngineParams& params, EngineFrameInfo* info = nullptr) {
    return submitRegion(source, 0, 0, srcW, srcH, timeSeconds, params, info);
  }
  // Same, using only the region [srcX, srcX+srcW) x [srcY, srcY+srcH) of the source (crop).
  bool submitRegion(ID3D11Texture2D* source, uint32_t srcX, uint32_t srcY, uint32_t srcW, uint32_t srcH,
                    double timeSeconds, const EngineParams& params, EngineFrameInfo* info = nullptr);

  // Re-renders the newest frame without blur (used when the source goes idle).
  bool renderNewestUnblurred();

  ID3D11ShaderResourceView* outputSRV() const { return m_outSrv.Get(); }
  // Raw captured frames (Texture2DArray) and the slice holding the newest one: used by the filter
  // chain's compare view to show the untouched capture next to the processed image.
  ID3D11ShaderResourceView* frameHistorySRV() const { return m_colorRingSrv.Get(); }
  int newestSlice() const { return m_newest; }
  ID3D11Texture2D* outputTexture() const { return m_out.Get(); }
  uint32_t width() const { return m_w; }
  uint32_t height() const { return m_h; }
  bool hasOutput() const { return m_hasOutput; }

  EngineGpuStats pollGpuStats();

  // ---- Testing / diagnostics (these stall the GPU) ----
  bool readbackOutput(std::vector<uint8_t>& rgba);
  bool readbackNewestFlow(std::vector<float>& xyzw, uint32_t& gridW, uint32_t& gridH);
  float measureLastGpuMsBlocking();

 private:
  using SRV = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
  using UAV = Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
  using Tex = Microsoft::WRL::ComPtr<ID3D11Texture2D>;

  struct Level {
    uint32_t w = 0, h = 0, gw = 0, gh = 0;
    Tex luma;
    SRV lumaSrv;
    UAV lumaUav;
    Tex flowA, flowB;
    SRV flowASrv, flowBSrv;
    UAV flowAUav, flowBUav;
  };

  struct SlotInfo {
    double time = 0;
    bool discontinuityBefore = true;
    bool hasLuma = false;  // luma pyramid computed for this slot (needed as the next frame's reference)
  };

  bool createShaders(std::string* error);
  bool createSizeResources(uint32_t w, uint32_t h);
  void releaseSizeResources();
  void runMotionEstimation(int prevSlot, int curSlot);
  void runSynthesis(const EngineParams& params, EngineFrameInfo* info);
  void collectHistory(std::vector<HistoryFrame>& out) const;
  template <class T>
  void updateCB(ID3D11Buffer* cb, const T& data);
  void dispatch(ID3D11ComputeShader* cs, ID3D11Buffer* cb, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                ID3D11UnorderedAccessView* uav, uint32_t gx, uint32_t gy);

  Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;

  Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csLuma, m_csDown, m_csMe, m_csMedian, m_csStats, m_csSynth, m_csBlend;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPyramid, m_cbMe, m_cbMedian, m_cbStats, m_cbSynth, m_cbBlend;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearClamp;

  uint32_t m_w = 0, m_h = 0;
  Tex m_colorRing;
  SRV m_colorRingSrv;
  std::array<Level, kLevels> m_levels;
  Tex m_flowRing;
  SRV m_flowRingSrv;
  Tex m_out;
  SRV m_outSrv;
  UAV m_outUav;

  Microsoft::WRL::ComPtr<ID3D11Buffer> m_statsTemp, m_statsAll;
  UAV m_statsTempUav;
  SRV m_statsAllSrv;
  static constexpr int kReadbackRing = 4;
  std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, kReadbackRing> m_statsStaging;
  std::array<bool, kReadbackRing> m_statsPending{};
  std::array<float, kReadbackRing> m_statsSceneAbs{};
  int m_statsWrite = 0;

  struct TimingQuery {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false;
  };
  std::array<TimingQuery, kReadbackRing> m_timing;
  int m_timingWrite = 0;
  EngineGpuStats m_lastStats;

  std::array<SlotInfo, kSlots> m_slots;
  int m_newest = -1;
  int m_count = 0;
  bool m_hasOutput = false;
  EngineParams m_lastParams;
  float m_sceneRel = 0.5f;
};

}  // namespace bl
