#include "engine/blur_engine.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "blend_cs.h"
#include "core/log.h"
#include "core/util.h"
#include "downsample_cs.h"
#include "luma_cs.h"
#include "me_cs.h"
#include "median_cs.h"
#include "stats_cs.h"
#include "synth_cs.h"

using Microsoft::WRL::ComPtr;

namespace bl {

namespace {

// ---- Constant buffer layouts (must match the HLSL cbuffers exactly) ----
struct PyramidCB {
  uint32_t srcW, srcH, dstW, dstH, srcSlice, dstSlice, pad[2];
};
static_assert(sizeof(PyramidCB) == 32);

struct MeCB {
  int32_t levelW, levelH, gridW, gridH, coarseW, coarseH;
  uint32_t curSlice, prevSlice;
  int32_t radius, coarsest;
  float levelScale, lambda;
  int32_t subpixel;
  float pad[3];
};
static_assert(sizeof(MeCB) == 64);

struct MedianCB {
  int32_t gw, gh, pad[2];
};
static_assert(sizeof(MedianCB) == 16);

struct StatsCB {
  int32_t gw, gh, lumaW, lumaH;
  uint32_t slice, pad[3];
};
static_assert(sizeof(StatsCB) == 32);

struct SegmentGPU {
  uint32_t prevSlice, curSlice, samples, pad;
  float u0, u1, tau0, tau1;
};
struct SynthCB {
  uint32_t w, h;
  float invW, invH;
  float flowUVScaleX, flowUVScaleY;
  uint32_t segCount, newestSlice;
  float gamma, invGamma, confLo, confHi;
  float sceneAbs, sceneRel;
  uint32_t enabled, pad0;
  SegmentGPU seg[BlurEngine::kMaxSegments];
  float lut[kWeightLutSize];
};
static_assert(sizeof(SynthCB) == 704);

struct BlendCB {
  uint32_t w, h, count, enabled;
  float gamma, invGamma, sceneAbs, sceneRel;
  uint32_t slices[16];
  float weights[16];
};
static_assert(sizeof(BlendCB) == 160);

constexpr uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// Motion search parameters per pyramid level (index 0 = 1/2 resolution).
constexpr int kCoarseRadius = 8;   // +-8 px at 1/16 res = +-128 px full res per frame
constexpr int kRefineRadius[BlurEngine::kLevels] = {1, 2, 2, kCoarseRadius};
constexpr float kLambda = 0.0015f;

}  // namespace

bool BlurEngine::init(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error) {
  m_dev = device;
  m_ctx = context;
  if (!createShaders(error)) return false;

  auto makeCB = [&](UINT size, ComPtr<ID3D11Buffer>& out) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(m_dev->CreateBuffer(&d, nullptr, &out));
  };
  bool ok = makeCB(sizeof(PyramidCB), m_cbPyramid) && makeCB(sizeof(MeCB), m_cbMe) &&
            makeCB(sizeof(MedianCB), m_cbMedian) && makeCB(sizeof(StatsCB), m_cbStats) &&
            makeCB(sizeof(SynthCB), m_cbSynth) && makeCB(sizeof(BlendCB), m_cbBlend);
  if (!ok) {
    if (error) *error = "Failed to create constant buffers.";
    return false;
  }

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(m_dev->CreateSamplerState(&sd, &m_linearClamp))) {
    if (error) *error = "Failed to create sampler.";
    return false;
  }

  // Per-frame statistics buffers (raw, 16 bytes per slot).
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 16;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(m_dev->CreateBuffer(&bd, nullptr, &m_statsTemp))) return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = 4;
  ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(m_dev->CreateUnorderedAccessView(m_statsTemp.Get(), &ud, &m_statsTempUav))) return false;

  std::vector<uint32_t> zeros(kSlots * 4, 0);
  D3D11_SUBRESOURCE_DATA init{zeros.data(), 0, 0};
  bd.ByteWidth = 16 * kSlots;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(m_dev->CreateBuffer(&bd, &init, &m_statsAll))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = DXGI_FORMAT_R32_TYPELESS;
  sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  sv.BufferEx.NumElements = 4 * kSlots;
  sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  if (FAILED(m_dev->CreateShaderResourceView(m_statsAll.Get(), &sv, &m_statsAllSrv))) return false;

  D3D11_BUFFER_DESC stg{};
  stg.ByteWidth = 16;
  stg.Usage = D3D11_USAGE_STAGING;
  stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto& b : m_statsStaging)
    if (FAILED(m_dev->CreateBuffer(&stg, nullptr, &b))) return false;

  D3D11_QUERY_DESC qd{};
  for (auto& t : m_timing) {
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (FAILED(m_dev->CreateQuery(&qd, &t.disjoint))) return false;
    qd.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(m_dev->CreateQuery(&qd, &t.begin)) || FAILED(m_dev->CreateQuery(&qd, &t.end))) return false;
  }
  return true;
}

bool BlurEngine::createShaders(std::string* error) {
  struct Item {
    const BYTE* code;
    size_t size;
    ComPtr<ID3D11ComputeShader>* out;
    const char* name;
  } items[] = {
      {g_luma_cs, sizeof(g_luma_cs), &m_csLuma, "luma"},
      {g_downsample_cs, sizeof(g_downsample_cs), &m_csDown, "downsample"},
      {g_me_cs, sizeof(g_me_cs), &m_csMe, "motion estimation"},
      {g_median_cs, sizeof(g_median_cs), &m_csMedian, "median"},
      {g_stats_cs, sizeof(g_stats_cs), &m_csStats, "stats"},
      {g_synth_cs, sizeof(g_synth_cs), &m_csSynth, "synthesis"},
      {g_blend_cs, sizeof(g_blend_cs), &m_csBlend, "blend"},
  };
  for (auto& it : items) {
    HRESULT hr = m_dev->CreateComputeShader(it.code, it.size, nullptr, it.out->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      if (error) *error = std::string("Failed to create compute shader '") + it.name + "': " + hrToString(hr);
      return false;
    }
  }
  return true;
}

void BlurEngine::shutdown() {
  releaseSizeResources();
  m_csLuma.Reset(); m_csDown.Reset(); m_csMe.Reset(); m_csMedian.Reset();
  m_csStats.Reset(); m_csSynth.Reset(); m_csBlend.Reset();
  m_cbPyramid.Reset(); m_cbMe.Reset(); m_cbMedian.Reset(); m_cbStats.Reset();
  m_cbSynth.Reset(); m_cbBlend.Reset();
  m_linearClamp.Reset();
  m_statsTemp.Reset(); m_statsAll.Reset(); m_statsTempUav.Reset(); m_statsAllSrv.Reset();
  for (auto& b : m_statsStaging) b.Reset();
  for (auto& t : m_timing) t = {};
  m_ctx.Reset();
  m_dev.Reset();
}

void BlurEngine::releaseSizeResources() {
  m_colorRing.Reset(); m_colorRingSrv.Reset();
  for (auto& l : m_levels) l = {};
  m_flowRing.Reset(); m_flowRingSrv.Reset();
  m_out.Reset(); m_outSrv.Reset(); m_outUav.Reset();
  m_w = m_h = 0;
  m_hasOutput = false;
}

bool BlurEngine::createSizeResources(uint32_t w, uint32_t h) {
  releaseSizeResources();

  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = kSlots;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_colorRing))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = td.Format;
  sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  sv.Texture2DArray.MipLevels = 1;
  sv.Texture2DArray.ArraySize = kSlots;
  if (FAILED(m_dev->CreateShaderResourceView(m_colorRing.Get(), &sv, &m_colorRingSrv))) return false;

  uint32_t lw = w, lh = h;
  for (int i = 0; i < kLevels; ++i) {
    Level& L = m_levels[size_t(i)];
    lw = std::max(1u, divUp(lw, 2));
    lh = std::max(1u, divUp(lh, 2));
    L.w = lw;
    L.h = lh;
    L.gw = std::max(1u, divUp(lw, 4));
    L.gh = std::max(1u, divUp(lh, 4));

    D3D11_TEXTURE2D_DESC ld{};
    ld.Width = lw;
    ld.Height = lh;
    ld.MipLevels = 1;
    ld.ArraySize = kSlots;
    ld.Format = DXGI_FORMAT_R16_FLOAT;
    ld.SampleDesc.Count = 1;
    ld.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(m_dev->CreateTexture2D(&ld, nullptr, &L.luma))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC lsv{};
    lsv.Format = ld.Format;
    lsv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    lsv.Texture2DArray.MipLevels = 1;
    lsv.Texture2DArray.ArraySize = kSlots;
    if (FAILED(m_dev->CreateShaderResourceView(L.luma.Get(), &lsv, &L.lumaSrv))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC luv{};
    luv.Format = ld.Format;
    luv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    luv.Texture2DArray.ArraySize = kSlots;
    if (FAILED(m_dev->CreateUnorderedAccessView(L.luma.Get(), &luv, &L.lumaUav))) return false;

    D3D11_TEXTURE2D_DESC fd{};
    fd.Width = L.gw;
    fd.Height = L.gh;
    fd.MipLevels = 1;
    fd.ArraySize = 1;
    fd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fd.SampleDesc.Count = 1;
    fd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (int k = 0; k < 2; ++k) {
      Tex& t = k == 0 ? L.flowA : L.flowB;
      SRV& s = k == 0 ? L.flowASrv : L.flowBSrv;
      UAV& u = k == 0 ? L.flowAUav : L.flowBUav;
      if (FAILED(m_dev->CreateTexture2D(&fd, nullptr, &t))) return false;
      if (FAILED(m_dev->CreateShaderResourceView(t.Get(), nullptr, &s))) return false;
      if (FAILED(m_dev->CreateUnorderedAccessView(t.Get(), nullptr, &u))) return false;
    }
  }

  // Final motion field history (finest grid = 8x8 full-res pixels per vector).
  D3D11_TEXTURE2D_DESC frd{};
  frd.Width = m_levels[0].gw;
  frd.Height = m_levels[0].gh;
  frd.MipLevels = 1;
  frd.ArraySize = kSlots;
  frd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  frd.SampleDesc.Count = 1;
  frd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(m_dev->CreateTexture2D(&frd, nullptr, &m_flowRing))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC fsv{};
  fsv.Format = frd.Format;
  fsv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  fsv.Texture2DArray.MipLevels = 1;
  fsv.Texture2DArray.ArraySize = kSlots;
  if (FAILED(m_dev->CreateShaderResourceView(m_flowRing.Get(), &fsv, &m_flowRingSrv))) return false;

  D3D11_TEXTURE2D_DESC od{};
  od.Width = w;
  od.Height = h;
  od.MipLevels = 1;
  od.ArraySize = 1;
  od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  od.SampleDesc.Count = 1;
  od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(m_dev->CreateTexture2D(&od, nullptr, &m_out))) return false;
  if (FAILED(m_dev->CreateShaderResourceView(m_out.Get(), nullptr, &m_outSrv))) return false;
  if (FAILED(m_dev->CreateUnorderedAccessView(m_out.Get(), nullptr, &m_outUav))) return false;

  m_w = w;
  m_h = h;
  logInfo("Engine resources allocated for {}x{} (motion grid {}x{}, {} history slots)", w, h, m_levels[0].gw,
          m_levels[0].gh, kSlots);
  return true;
}

void BlurEngine::resetHistory() {
  for (auto& s : m_slots) s = {};
  m_newest = -1;
  m_count = 0;
}

template <class T>
void BlurEngine::updateCB(ID3D11Buffer* cb, const T& data) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(m_ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, &data, sizeof(T));
    m_ctx->Unmap(cb, 0);
  }
}

void BlurEngine::dispatch(ID3D11ComputeShader* cs, ID3D11Buffer* cb,
                          std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11UnorderedAccessView* uav,
                          uint32_t gx, uint32_t gy) {
  ID3D11ShaderResourceView* srvArr[4] = {};
  UINT n = 0;
  for (auto* s : srvs) srvArr[n++] = s;
  m_ctx->CSSetShader(cs, nullptr, 0);
  m_ctx->CSSetConstantBuffers(0, 1, &cb);
  m_ctx->CSSetShaderResources(0, n, srvArr);
  m_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ID3D11SamplerState* samp = m_linearClamp.Get();
  m_ctx->CSSetSamplers(0, 1, &samp);
  m_ctx->Dispatch(gx, gy, 1);
  // Unbind so the next pass can use these resources with a different view type.
  ID3D11ShaderResourceView* nullSrv[4] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  m_ctx->CSSetShaderResources(0, n, nullSrv);
  m_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

void BlurEngine::collectHistory(std::vector<HistoryFrame>& out) const {
  out.clear();
  for (int i = 0; i < m_count; ++i) {
    int slot = (m_newest - i + kSlots) % kSlots;
    out.push_back({slot, m_slots[size_t(slot)].time, m_slots[size_t(slot)].discontinuityBefore});
  }
}

bool BlurEngine::submitRegion(ID3D11Texture2D* source, uint32_t srcX, uint32_t srcY, uint32_t srcW, uint32_t srcH,
                              double timeSeconds, const EngineParams& params, EngineFrameInfo* info) {
  if (!m_dev || !source) return false;
  EngineFrameInfo localInfo;
  EngineFrameInfo& fi = info ? *info : localInfo;
  fi = {};

  D3D11_TEXTURE2D_DESC sd;
  source->GetDesc(&sd);
  if (srcX >= sd.Width || srcY >= sd.Height) srcX = srcY = 0;
  srcW = std::min(srcW, sd.Width - srcX);
  srcH = std::min(srcH, sd.Height - srcY);
  if (srcW < 16 || srcH < 16) return false;
  if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM && sd.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS &&
      sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
    logError("Unsupported capture format {}", int(sd.Format));
    return false;
  }

  if (srcW != m_w || srcH != m_h) {
    if (!createSizeResources(srcW, srcH)) {
      logError("Failed to allocate engine resources for {}x{}", srcW, srcH);
      releaseSizeResources();
      return false;
    }
    resetHistory();
    fi.historyReset = true;
  }

  // Temporal continuity with the previous frame.
  // WGC timestamps are DWM target display times: when the game presents twice within one composition
  // interval, both frames carry the same time (measured: ~7% of frames at 280 fps). The newer one
  // supersedes the older (which is never displayed), so it REPLACES the newest history entry instead
  // of breaking the history - otherwise motion blur would drop out on those frames.
  bool discontinuity = m_count == 0;
  bool replace = false;
  if (m_count > 0) {
    double dt = timeSeconds - m_slots[size_t(m_newest)].time;
    if (std::abs(dt) <= 1e-5) replace = true;
    else if (dt < 0 || dt > params.maxGapSeconds) discontinuity = true;
  }
  fi.discontinuity = discontinuity;
  const int slot = replace ? m_newest : (m_newest + 1 + kSlots) % kSlots;
  const int prevSlot = replace ? (m_count > 1 ? (m_newest - 1 + kSlots) % kSlots : -1) : m_newest;
  // Motion analysis (luma pyramid + block matching) is only needed while blur is on. When it is off
  // the frame is just stored and passed through, saving ~0.4 ms of GPU time per 1080p frame. The
  // first frame after re-enabling has no predecessor analysis, so it starts a fresh history.
  const bool analyze = params.enabled;
  const bool noFlow = discontinuity || !analyze || prevSlot < 0 || !m_slots[size_t(prevSlot)].hasLuma ||
                      (replace && m_slots[size_t(slot)].discontinuityBefore);

  // GPU timing (skipped when the ring entry has not been consumed yet).
  TimingQuery& tq = m_timing[size_t(m_timingWrite)];
  if (tq.pending) pollGpuStats();
  const bool timeThisFrame = !tq.pending;
  if (timeThisFrame) {
    m_ctx->Begin(tq.disjoint.Get());
    m_ctx->End(tq.begin.Get());
  }

  D3D11_BOX box{srcX, srcY, 0, srcX + srcW, srcY + srcH, 1};
  m_ctx->CopySubresourceRegion(m_colorRing.Get(), D3D11CalcSubresource(0, UINT(slot), 1), 0, 0, 0, source, 0, &box);

  // Luma pyramid for the new slot.
  if (analyze) {
    PyramidCB cb{m_w, m_h, m_levels[0].w, m_levels[0].h, uint32_t(slot), uint32_t(slot), {0, 0}};
    updateCB(m_cbPyramid.Get(), cb);
    dispatch(m_csLuma.Get(), m_cbPyramid.Get(), {m_colorRingSrv.Get()}, m_levels[0].lumaUav.Get(),
             divUp(m_levels[0].w, 8), divUp(m_levels[0].h, 8));
    for (int i = 1; i < kLevels; ++i) {
      const Level& s = m_levels[size_t(i - 1)];
      const Level& d = m_levels[size_t(i)];
      PyramidCB c2{s.w, s.h, d.w, d.h, uint32_t(slot), uint32_t(slot), {0, 0}};
      updateCB(m_cbPyramid.Get(), c2);
      dispatch(m_csDown.Get(), m_cbPyramid.Get(), {s.lumaSrv.Get()}, d.lumaUav.Get(), divUp(d.w, 8), divUp(d.h, 8));
    }
  }

  const UINT zero4[4] = {0, 0, 0, 0};
  m_ctx->ClearUnorderedAccessViewUint(m_statsTempUav.Get(), zero4);
  if (!noFlow) runMotionEstimation(prevSlot, slot);
  // Publish this frame's statistics (zero when no motion was estimated).
  D3D11_BOX sb{0, 0, 0, 16, 1, 1};
  m_ctx->CopySubresourceRegion(m_statsAll.Get(), 0, UINT(slot) * 16, 0, 0, m_statsTemp.Get(), 0, &sb);
  if (!noFlow && !m_statsPending[size_t(m_statsWrite)]) {
    m_ctx->CopyResource(m_statsStaging[size_t(m_statsWrite)].Get(), m_statsTemp.Get());
    m_statsPending[size_t(m_statsWrite)] = true;
    m_statsSceneAbs[size_t(m_statsWrite)] = params.sceneCutAbs;
    m_statsWrite = (m_statsWrite + 1) % kReadbackRing;
  }

  m_slots[size_t(slot)] = {timeSeconds, noFlow, analyze};
  m_newest = slot;
  if (!replace) m_count = std::min(m_count + 1, kSlots);

  runSynthesis(params, &fi);

  if (timeThisFrame) {
    m_ctx->End(tq.end.Get());
    m_ctx->End(tq.disjoint.Get());
    tq.pending = true;
    m_timingWrite = (m_timingWrite + 1) % kReadbackRing;
  }
  m_lastParams = params;
  m_hasOutput = true;
  return true;
}

void BlurEngine::runMotionEstimation(int prevSlot, int curSlot) {
  // Coarse to fine. Each level: block matching into flowA, vector median into flowB.
  for (int i = kLevels - 1; i >= 0; --i) {
    Level& L = m_levels[size_t(i)];
    const bool coarsest = i == kLevels - 1;
    const Level* C = coarsest ? nullptr : &m_levels[size_t(i + 1)];
    MeCB cb{};
    cb.levelW = int32_t(L.w);
    cb.levelH = int32_t(L.h);
    cb.gridW = int32_t(L.gw);
    cb.gridH = int32_t(L.gh);
    cb.coarseW = C ? int32_t(C->gw) : 1;
    cb.coarseH = C ? int32_t(C->gh) : 1;
    cb.curSlice = uint32_t(curSlot);
    cb.prevSlice = uint32_t(prevSlot);
    cb.radius = kRefineRadius[i];
    cb.coarsest = coarsest ? 1 : 0;
    cb.levelScale = float(1u << (i + 1));
    cb.lambda = kLambda;
    cb.subpixel = i == 0 ? 1 : 0;
    updateCB(m_cbMe.Get(), cb);
    dispatch(m_csMe.Get(), m_cbMe.Get(), {L.lumaSrv.Get(), C ? C->flowBSrv.Get() : nullptr}, L.flowAUav.Get(),
             divUp(L.gw, 8), divUp(L.gh, 8));

    MedianCB mc{int32_t(L.gw), int32_t(L.gh), {0, 0}};
    updateCB(m_cbMedian.Get(), mc);
    dispatch(m_csMedian.Get(), m_cbMedian.Get(), {L.flowASrv.Get()}, L.flowBUav.Get(), divUp(L.gw, 8),
             divUp(L.gh, 8));
  }

  const Level& fine = m_levels[0];
  m_ctx->CopySubresourceRegion(m_flowRing.Get(), D3D11CalcSubresource(0, UINT(curSlot), 1), 0, 0, 0,
                               fine.flowB.Get(), 0, nullptr);

  const Level& coarse = m_levels[kLevels - 1];
  StatsCB sc{int32_t(fine.gw), int32_t(fine.gh), int32_t(coarse.w), int32_t(coarse.h), uint32_t(curSlot), {0, 0, 0}};
  updateCB(m_cbStats.Get(), sc);
  dispatch(m_csStats.Get(), m_cbStats.Get(), {fine.flowBSrv.Get(), coarse.lumaSrv.Get()}, m_statsTempUav.Get(),
           divUp(fine.gw, 16), divUp(fine.gh, 16));
}

void BlurEngine::runSynthesis(const EngineParams& params, EngineFrameInfo* info) {
  std::vector<HistoryFrame> hist;
  collectHistory(hist);
  const float invGamma = 1.0f / std::max(0.01f, params.gamma);
  m_sceneRel = params.sceneCutRel;

  if (params.mode == BlurMode::FrameBlend) {
    BlendPlan plan = planBlend(hist, params.blendFrames, params.weighting);
    BlendCB cb{};
    cb.w = m_w;
    cb.h = m_h;
    cb.count = uint32_t(std::min<size_t>(plan.slots.size(), 16));
    cb.enabled = params.enabled ? 1u : 0u;
    cb.gamma = params.gamma;
    cb.invGamma = invGamma;
    cb.sceneAbs = params.sceneCutAbs;
    cb.sceneRel = params.sceneCutRel;
    for (uint32_t i = 0; i < cb.count; ++i) {
      cb.slices[i] = uint32_t(plan.slots[i]);
      cb.weights[i] = plan.weights[i];
    }
    if (cb.count == 0) {
      cb.count = 1;
      cb.slices[0] = uint32_t(m_newest);
      cb.weights[0] = 1;
    }
    if (info) {
      info->segments = int(cb.count);
      info->samples = int(cb.count);
      info->coverage = 1.0f;
    }
    updateCB(m_cbBlend.Get(), cb);
    dispatch(m_csBlend.Get(), m_cbBlend.Get(), {m_colorRingSrv.Get(), m_statsAllSrv.Get()}, m_outUav.Get(),
             divUp(m_w, 8), divUp(m_h, 8));
    return;
  }

  FlowPlan plan;
  if (params.enabled) plan = planFlowSegments(hist, params.shutterSeconds, params.sampleBudget, kMaxSegments);

  SynthCB cb{};
  cb.w = m_w;
  cb.h = m_h;
  cb.invW = 1.0f / float(m_w);
  cb.invH = 1.0f / float(m_h);
  cb.flowUVScaleX = 1.0f / float(m_levels[0].gw * 8);
  cb.flowUVScaleY = 1.0f / float(m_levels[0].gh * 8);
  cb.segCount = uint32_t(plan.segments.size());
  cb.newestSlice = uint32_t(m_newest);
  cb.gamma = params.gamma;
  cb.invGamma = invGamma;
  cb.confLo = params.confidenceLow;
  cb.confHi = std::max(params.confidenceHigh, params.confidenceLow + 1e-3f);
  cb.sceneAbs = params.sceneCutAbs;
  cb.sceneRel = params.sceneCutRel;
  cb.enabled = params.enabled ? 1u : 0u;
  for (size_t i = 0; i < plan.segments.size(); ++i) {
    const PlannedSegment& s = plan.segments[i];
    cb.seg[i] = {uint32_t(s.prevSlot), uint32_t(s.curSlot), uint32_t(s.samples), 0, s.u0, s.u1, s.tau0, s.tau1};
  }
  auto lut = buildWeightLut(params.weighting);
  std::memcpy(cb.lut, lut.data(), sizeof(cb.lut));
  if (info) {
    info->segments = int(plan.segments.size());
    info->samples = plan.totalSamples;
    info->coverage = plan.coverage;
  }
  updateCB(m_cbSynth.Get(), cb);
  ID3D11ShaderResourceView* srvs[3] = {m_colorRingSrv.Get(), m_flowRingSrv.Get(), m_statsAllSrv.Get()};
  dispatch(m_csSynth.Get(), m_cbSynth.Get(), {srvs[0], srvs[1], srvs[2]}, m_outUav.Get(), divUp(m_w, 8),
           divUp(m_h, 8));
}

bool BlurEngine::renderNewestUnblurred() {
  if (!m_dev || m_newest < 0 || !m_out) return false;
  EngineParams p = m_lastParams;
  p.enabled = false;
  runSynthesis(p, nullptr);
  return true;
}

EngineGpuStats BlurEngine::pollGpuStats() {
  if (!m_ctx) return m_lastStats;
  for (int k = 0; k < kReadbackRing; ++k) {
    int i = (m_timingWrite + k) % kReadbackRing;  // oldest first
    TimingQuery& t = m_timing[size_t(i)];
    if (!t.pending) continue;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (m_ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    UINT64 b = 0, e = 0;
    if (m_ctx->GetData(t.begin.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    if (m_ctx->GetData(t.end.Get(), &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    t.pending = false;
    if (!dj.Disjoint && dj.Frequency > 0 && e >= b) {
      m_lastStats.gpuMs = float(double(e - b) * 1000.0 / double(dj.Frequency));
      m_lastStats.timingValid = true;
    }
  }
  for (int k = 0; k < kReadbackRing; ++k) {
    int i = (m_statsWrite + k) % kReadbackRing;
    if (!m_statsPending[size_t(i)]) continue;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = m_ctx->Map(m_statsStaging[size_t(i)].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr != S_OK) continue;
    uint32_t v[4];
    std::memcpy(v, m.pData, sizeof(v));
    m_ctx->Unmap(m_statsStaging[size_t(i)].Get(), 0);
    m_statsPending[size_t(i)] = false;
    if (v[2] > 0) {
      double inv = 1.0 / (10000.0 * double(v[2]));
      m_lastStats.meanBlockError = float(double(v[0]) * inv);
      m_lastStats.meanZeroError = float(double(v[1]) * inv);
      m_lastStats.meanLuma = float(double(v[3]) * inv);
      m_lastStats.sceneCut = m_lastStats.meanBlockError > m_statsSceneAbs[size_t(i)] &&
                             m_lastStats.meanBlockError > m_sceneRel * m_lastStats.meanZeroError;
      m_lastStats.motionValid = true;
    }
  }
  return m_lastStats;
}

bool BlurEngine::readbackOutput(std::vector<uint8_t>& rgba) {
  if (!m_out) return false;
  D3D11_TEXTURE2D_DESC d;
  m_out->GetDesc(&d);
  d.BindFlags = 0;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> stg;
  if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &stg))) return false;
  m_ctx->CopyResource(stg.Get(), m_out.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(m_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  rgba.resize(size_t(d.Width) * d.Height * 4);
  for (UINT y = 0; y < d.Height; ++y)
    std::memcpy(rgba.data() + size_t(y) * d.Width * 4, static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                size_t(d.Width) * 4);
  m_ctx->Unmap(stg.Get(), 0);
  return true;
}

bool BlurEngine::readbackNewestFlow(std::vector<float>& xyzw, uint32_t& gridW, uint32_t& gridH) {
  if (!m_flowRing || m_newest < 0) return false;
  D3D11_TEXTURE2D_DESC d;
  m_flowRing->GetDesc(&d);
  d.ArraySize = 1;
  d.BindFlags = 0;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> stg;
  if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &stg))) return false;
  m_ctx->CopySubresourceRegion(stg.Get(), 0, 0, 0, 0, m_flowRing.Get(), D3D11CalcSubresource(0, UINT(m_newest), 1),
                               nullptr);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(m_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  gridW = d.Width;
  gridH = d.Height;
  xyzw.resize(size_t(gridW) * gridH * 4);
  for (UINT y = 0; y < gridH; ++y) {
    auto* row = reinterpret_cast<const DirectX::PackedVector::HALF*>(static_cast<uint8_t*>(m.pData) +
                                                                     size_t(y) * m.RowPitch);
    for (UINT x = 0; x < gridW * 4; ++x)
      xyzw[(size_t(y) * gridW * 4) + x] = DirectX::PackedVector::XMConvertHalfToFloat(row[x]);
  }
  m_ctx->Unmap(stg.Get(), 0);
  return true;
}

float BlurEngine::measureLastGpuMsBlocking() {
  int i = (m_timingWrite - 1 + kReadbackRing) % kReadbackRing;
  TimingQuery& t = m_timing[size_t(i)];
  if (!t.pending) return m_lastStats.gpuMs;
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
  while (m_ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {}
  UINT64 b = 0, e = 0;
  while (m_ctx->GetData(t.begin.Get(), &b, sizeof(b), 0) == S_FALSE) {}
  while (m_ctx->GetData(t.end.Get(), &e, sizeof(e), 0) == S_FALSE) {}
  t.pending = false;
  if (dj.Disjoint || dj.Frequency == 0) return -1.0f;
  m_lastStats.gpuMs = float(double(e - b) * 1000.0 / double(dj.Frequency));
  m_lastStats.timingValid = true;
  return m_lastStats.gpuMs;
}

}  // namespace bl
