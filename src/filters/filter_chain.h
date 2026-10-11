#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "filters/filter_registry.h"

namespace bl {

struct FilterFrameInput {
  ID3D11ShaderResourceView* source = nullptr;  // engine output (display-encoded RGBA8)
  uint32_t width = 0, height = 0;
  ID3D11ShaderResourceView* originalArray = nullptr;  // raw captured frames (compare view)
  int originalSlice = -1;
  bool compare = false;
  float compareSplit = 0.5f;
};

// GPU filter suite that runs after the motion-blur engine. Not thread-safe: use on the thread that
// owns the immediate context (the pipeline thread).
//
// Passes (only the ones needed by enabled filters run):
//   denoise (full res) -> luma pyramid 1/2..1/16 -> bloom chain 1/2..1/16 -> uber pass (full res)
// With no filter enabled and compare off, process() returns the source untouched: zero GPU work.
class FilterChain {
 public:
  bool init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::string* error);
  void shutdown();

  // Cheap to call every frame; reloads the LUT only when its path changes.
  void setSettings(const FilterSettings& fs);
  bool needsProcessing(bool compare) const;
  ID3D11ShaderResourceView* process(const FilterFrameInput& in);
  ID3D11ShaderResourceView* outputSRV() const { return m_out.srv.Get(); }

  // GPU time of the last measured filter pass (non-blocking; 0 if nothing ran).
  float pollGpuMs();
  float measureLastGpuMsBlocking();  // tests only

  // LUT status for the UI: title / error of the current .cube file.
  bool lutLoaded() const { return m_lutSize > 0; }
  const std::string& lutMessage() const { return m_lutMessage; }

  // Live preview for the control panel: a small shared texture (keyed mutex) updated at most
  // ~30 times per second. Returns false if the copy was skipped (UI busy, not requested).
  bool updatePreview(ID3D11ShaderResourceView* frame, uint32_t w, uint32_t h);
  HANDLE previewHandle() const { return m_previewHandle; }
  uint32_t previewWidth() const { return m_previewW; }
  uint32_t previewHeight() const { return m_previewH; }

  bool readbackOutput(std::vector<uint8_t>& rgba);  // tests only (stalls)

 private:
  using Tex = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
  using SRV = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
  using UAV = Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
  struct Target {
    Tex tex;
    SRV srv;
    UAV uav;
    uint32_t w = 0, h = 0;
  };
  bool makeTarget(Target& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt);
  bool ensureResources(uint32_t w, uint32_t h);
  void loadLut();
  void packConstants(const FilterFrameInput& in);
  template <class T>
  void updateCB(ID3D11Buffer* cb, const T& data);
  void dispatch(ID3D11ComputeShader* cs, ID3D11Buffer* cb, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                ID3D11UnorderedAccessView* uav, uint32_t w, uint32_t h);
  bool on(FilterId id) const { return m_fs[id].enabled; }

  Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csUber, m_csDenoise, m_csPyrColor, m_csPyr, m_csBloom;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbUber, m_cbSmall;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linear;

  FilterSettings m_fs = defaultFilterSettings();
  std::string m_loadedLutPath;
  std::string m_lutMessage;
  Tex m_lutTex;
  SRV m_lutSrv;
  int m_lutSize = 0;

  uint32_t m_w = 0, m_h = 0;
  Target m_out, m_denoised;
  std::array<Target, 4> m_pyr;      // luma 1/2 .. 1/16
  std::array<Target, 4> m_bloomDown;  // 1/2 .. 1/16
  std::array<Target, 3> m_bloomUp;    // 1/2 .. 1/8
  uint32_t m_frame = 0;

  // GPU timing
  struct Timing {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false;
  };
  std::array<Timing, 4> m_timing;
  int m_timingWrite = 0;
  float m_gpuMs = 0;

  // Preview
  Tex m_preview;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_previewRtv;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_previewMutex;
  HANDLE m_previewHandle = nullptr;
  uint32_t m_previewW = 0, m_previewH = 0;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
};

}  // namespace bl
