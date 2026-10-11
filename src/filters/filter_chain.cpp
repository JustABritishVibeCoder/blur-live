#include "filters/filter_chain.h"

#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/util.h"
#include "filters/color_math.h"
#include "filters/lut.h"
#include "fx_bloom_cs.h"
#include "fx_denoise_cs.h"
#include "fx_pyramid_color_cs.h"
#include "fx_pyramid_cs.h"
#include "fx_uber_cs.h"
#include "present_ps.h"
#include "present_vs.h"

using Microsoft::WRL::ComPtr;

namespace bl {

namespace {

// Must match cbuffer FxParams in fx_uber_cs.hlsl.
struct FxCB {
  uint32_t w, h;
  float invW, invH;
  uint32_t flags, frame;
  float compareSplit;
  uint32_t compare;
  uint32_t origSlice, dither;
  float pyr1[2];
  float pyr2[2], pyr3[2];
  float pyr4[2], pad0[2];
  float sharpen[4], sharpen2[4], detail[4], exposure[4], tone[4];
  float colorM0[4], colorM1[4], colorM2[4];
  float shadowHi[4], levels[4], lift[4], invGamma[4], gain[4];
  float gradeShadow[4], gradeHigh[4], gradeAmbient[4];
  float lut[4], bloom[4], vignette[4], grain[4], chroma[4];
  float cvd0[4], cvd1[4], cvd2[4], cvdParams[4];
  float focus[4];
  float selective[kSelectiveBands][4];
  float selParams[4];
};
static_assert(sizeof(FxCB) == 40 * 16, "FxCB must match the HLSL cbuffer");

struct SmallCB {  // shared by the pyramid / bloom / denoise passes (layouts are prefixes of this)
  uint32_t a0, a1;
  float f0, f1;
  uint32_t mode;
  float threshold, knee, upWeight;
};
static_assert(sizeof(SmallCB) == 32);

constexpr uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
constexpr float kTwoPi = 6.28318531f;

void set4(float* d, float a, float b = 0, float c = 0, float e = 0) {
  d[0] = a;
  d[1] = b;
  d[2] = c;
  d[3] = e;
}

}  // namespace

bool FilterChain::init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::string* error) {
  m_dev = device;
  m_ctx = ctx;
  struct S {
    const BYTE* code;
    size_t size;
    ComPtr<ID3D11ComputeShader>* out;
  } shaders[] = {{g_fx_uber_cs, sizeof(g_fx_uber_cs), &m_csUber},
                 {g_fx_denoise_cs, sizeof(g_fx_denoise_cs), &m_csDenoise},
                 {g_fx_pyramid_color_cs, sizeof(g_fx_pyramid_color_cs), &m_csPyrColor},
                 {g_fx_pyramid_cs, sizeof(g_fx_pyramid_cs), &m_csPyr},
                 {g_fx_bloom_cs, sizeof(g_fx_bloom_cs), &m_csBloom}};
  for (auto& s : shaders) {
    HRESULT hr = m_dev->CreateComputeShader(s.code, s.size, nullptr, s.out->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      if (error) *error = "Filter shader creation failed: " + hrToString(hr);
      return false;
    }
  }
  if (FAILED(m_dev->CreateVertexShader(g_present_vs, sizeof(g_present_vs), nullptr, &m_vs)) ||
      FAILED(m_dev->CreatePixelShader(g_present_ps, sizeof(g_present_ps), nullptr, &m_ps))) {
    if (error) *error = "Preview shader creation failed";
    return false;
  }
  auto makeCB = [&](UINT size, ComPtr<ID3D11Buffer>& out) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(m_dev->CreateBuffer(&d, nullptr, &out));
  };
  if (!makeCB(sizeof(FxCB), m_cbUber) || !makeCB(sizeof(SmallCB), m_cbSmall)) {
    if (error) *error = "Filter constant buffers could not be created";
    return false;
  }
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(m_dev->CreateSamplerState(&sd, &m_linear))) return false;
  D3D11_QUERY_DESC qd{};
  for (auto& t : m_timing) {
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    m_dev->CreateQuery(&qd, &t.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    m_dev->CreateQuery(&qd, &t.begin);
    m_dev->CreateQuery(&qd, &t.end);
  }
  return true;
}

void FilterChain::shutdown() {
  m_out = {};
  m_denoised = {};
  for (auto& t : m_pyr) t = {};
  for (auto& t : m_bloomDown) t = {};
  for (auto& t : m_bloomUp) t = {};
  m_lutTex.Reset();
  m_lutSrv.Reset();
  m_lutSize = 0;
  m_loadedLutPath.clear();
  m_preview.Reset();
  m_previewRtv.Reset();
  m_previewMutex.Reset();
  m_previewHandle = nullptr;
  m_csUber.Reset(); m_csDenoise.Reset(); m_csPyrColor.Reset(); m_csPyr.Reset(); m_csBloom.Reset();
  m_cbUber.Reset(); m_cbSmall.Reset(); m_linear.Reset(); m_vs.Reset(); m_ps.Reset();
  for (auto& t : m_timing) t = {};
  m_w = m_h = 0;
  m_ctx.Reset();
  m_dev.Reset();
}

void FilterChain::setSettings(const FilterSettings& fs) {
  m_fs = fs;
  if (m_fs.lutPath != m_loadedLutPath) loadLut();
}

void FilterChain::loadLut() {
  m_loadedLutPath = m_fs.lutPath;
  m_lutTex.Reset();
  m_lutSrv.Reset();
  m_lutSize = 0;
  m_lutMessage.clear();
  if (m_fs.lutPath.empty() || !m_dev) return;
  CubeLut lut;
  std::string err;
  if (!loadCubeLut(toWide(m_fs.lutPath), lut, &err)) {
    m_lutMessage = err;
    logWarn("LUT \"{}\" not loaded: {}", m_fs.lutPath, err);
    return;
  }
  D3D11_TEXTURE3D_DESC d{};
  d.Width = d.Height = d.Depth = UINT(lut.size);
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{lut.rgba.data(), UINT(lut.size * 16), UINT(lut.size * lut.size * 16)};
  ComPtr<ID3D11Texture3D> tex;
  if (FAILED(m_dev->CreateTexture3D(&d, &init, &tex)) ||
      FAILED(m_dev->CreateShaderResourceView(tex.Get(), nullptr, &m_lutSrv))) {
    m_lutMessage = "GPU texture for the LUT could not be created.";
    return;
  }
  m_lutSize = lut.size;
  m_lutMessage = (lut.title.empty() ? std::string("LUT") : lut.title) + " (" + std::to_string(lut.size) + "^3)";
  logInfo("LUT loaded: {} {}^3", m_fs.lutPath, lut.size);
}

bool FilterChain::needsProcessing(bool compare) const {
  if (compare) return true;
  for (int i = 0; i < kFilterCount; ++i)
    if (m_fs.filters[size_t(i)].enabled && (FilterId(i) != FilterId::Lut || m_lutSize > 0)) return true;
  return false;
}

bool FilterChain::makeTarget(Target& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
  t = {};
  D3D11_TEXTURE2D_DESC d{};
  d.Width = std::max(1u, w);
  d.Height = std::max(1u, h);
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &t.tex))) return false;
  if (FAILED(m_dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) return false;
  if (FAILED(m_dev->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav))) return false;
  t.w = d.Width;
  t.h = d.Height;
  return true;
}

bool FilterChain::ensureResources(uint32_t w, uint32_t h) {
  if (w != m_w || h != m_h) {
    m_denoised = {};
    for (auto& t : m_pyr) t = {};
    for (auto& t : m_bloomDown) t = {};
    for (auto& t : m_bloomUp) t = {};
    if (!makeTarget(m_out, w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    m_w = w;
    m_h = h;
  }
  // Intermediate targets are only allocated once a filter needs them.
  if (on(FilterId::Denoise) && !m_denoised.tex && !makeTarget(m_denoised, w, h, DXGI_FORMAT_R8G8B8A8_UNORM))
    return false;
  const bool needPyr = on(FilterId::Detail) || on(FilterId::Clarity) || on(FilterId::LocalContrast) ||
                       on(FilterId::ShadowsHighlights);
  if (needPyr && !m_pyr[0].tex) {
    uint32_t lw = w, lh = h;
    for (auto& t : m_pyr) {
      lw = std::max(1u, divUp(lw, 2));
      lh = std::max(1u, divUp(lh, 2));
      if (!makeTarget(t, lw, lh, DXGI_FORMAT_R16_FLOAT)) return false;
    }
  }
  if (on(FilterId::Bloom) && !m_bloomDown[0].tex) {
    uint32_t lw = w, lh = h;
    for (size_t i = 0; i < m_bloomDown.size(); ++i) {
      lw = std::max(1u, divUp(lw, 2));
      lh = std::max(1u, divUp(lh, 2));
      if (!makeTarget(m_bloomDown[i], lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
      if (i < m_bloomUp.size() && !makeTarget(m_bloomUp[i], lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
    }
  }
  return true;
}

template <class T>
void FilterChain::updateCB(ID3D11Buffer* cb, const T& data) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(m_ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, &data, sizeof(T));
    m_ctx->Unmap(cb, 0);
  }
}

void FilterChain::dispatch(ID3D11ComputeShader* cs, ID3D11Buffer* cb,
                           std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11UnorderedAccessView* uav,
                           uint32_t w, uint32_t h) {
  ID3D11ShaderResourceView* arr[8] = {};
  UINT n = 0;
  for (auto* s : srvs) arr[n++] = s;
  m_ctx->CSSetShader(cs, nullptr, 0);
  m_ctx->CSSetConstantBuffers(0, 1, &cb);
  m_ctx->CSSetShaderResources(0, n, arr);
  m_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ID3D11SamplerState* s = m_linear.Get();
  m_ctx->CSSetSamplers(0, 1, &s);
  m_ctx->Dispatch(divUp(w, 8), divUp(h, 8), 1);
  ID3D11ShaderResourceView* nullSrv[8] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  m_ctx->CSSetShaderResources(0, n, nullSrv);
  m_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

void FilterChain::packConstants(const FilterFrameInput& in) {
  using color::Vec3;
  FxCB cb{};
  cb.w = in.width;
  cb.h = in.height;
  cb.invW = 1.0f / float(in.width);
  cb.invH = 1.0f / float(in.height);
  cb.frame = m_frame;
  cb.compare = in.compare && in.originalArray && in.originalSlice >= 0 ? 1u : 0u;
  cb.compareSplit = std::clamp(in.compareSplit, 0.0f, 1.0f);
  cb.origSlice = uint32_t(std::max(0, in.originalSlice));
  if (m_pyr[0].tex) {
    cb.pyr1[0] = float(m_pyr[0].w); cb.pyr1[1] = float(m_pyr[0].h);
    cb.pyr2[0] = float(m_pyr[1].w); cb.pyr2[1] = float(m_pyr[1].h);
    cb.pyr3[0] = float(m_pyr[2].w); cb.pyr3[1] = float(m_pyr[2].h);
    cb.pyr4[0] = float(m_pyr[3].w); cb.pyr4[1] = float(m_pyr[3].h);
  }

  uint32_t flags = 0;
  bool alters = false;
  for (int i = 0; i < kFilterCount; ++i) {
    FilterId id = FilterId(i);
    if (!m_fs[id].enabled || id == FilterId::Denoise) continue;  // denoise is its own pass
    if (id == FilterId::Lut && m_lutSize == 0) continue;
    flags |= 1u << i;
    if (id != FilterId::Sharpen && id != FilterId::Detail && id != FilterId::Clarity &&
        id != FilterId::LocalContrast && id != FilterId::FilmGrain && filterAltersImage(m_fs, id))
      alters = true;  // colour / tone changes: dither the 8-bit output to avoid banding
  }
  cb.flags = flags;
  cb.dither = alters ? 1u : 0u;

  auto v = [&](FilterId id, int p) { return m_fs.value(id, p); };
  set4(cb.sharpen, v(FilterId::Sharpen, 0), v(FilterId::Sharpen, 1), v(FilterId::Sharpen, 2), v(FilterId::Sharpen, 3));
  set4(cb.sharpen2, v(FilterId::Sharpen, 4), v(FilterId::Sharpen, 5));
  set4(cb.detail, v(FilterId::Detail, 0), v(FilterId::Clarity, 0), v(FilterId::Clarity, 1), v(FilterId::LocalContrast, 0));
  set4(cb.exposure, std::exp2(v(FilterId::Exposure, 0)), v(FilterId::Exposure, 1), v(FilterId::Brightness, 0),
       std::exp2(v(FilterId::Contrast, 0) * 1.3f));
  set4(cb.tone, 1.0f / std::max(0.05f, v(FilterId::Gamma, 0)), v(FilterId::Saturation, 0), v(FilterId::Vibrance, 0));

  color::Mat3 m = color::identity();
  if (on(FilterId::WhiteBalance)) m = color::whiteBalanceMatrix(v(FilterId::WhiteBalance, 0), v(FilterId::WhiteBalance, 1));
  if (on(FilterId::RgbChannels))
    m = color::mul(color::diag(v(FilterId::RgbChannels, 0), v(FilterId::RgbChannels, 1), v(FilterId::RgbChannels, 2)), m);
  set4(cb.colorM0, m[0], m[1], m[2]);
  set4(cb.colorM1, m[3], m[4], m[5]);
  set4(cb.colorM2, m[6], m[7], m[8]);

  set4(cb.shadowHi, v(FilterId::ShadowsHighlights, 0), v(FilterId::ShadowsHighlights, 1), v(FilterId::ShadowsHighlights, 2));
  set4(cb.levels, v(FilterId::Levels, 0), v(FilterId::Levels, 1), v(FilterId::Levels, 2), v(FilterId::Levels, 3));

  {  // lift / gamma / gain, each optionally tinted towards a hue
    const FilterId L = FilterId::LiftGammaGain;
    Vec3 lt = color::tintOffset(v(L, 3)), gt = color::tintOffset(v(L, 5)), nt = color::tintOffset(v(L, 7));
    for (int c = 0; c < 3; ++c) {
      cb.lift[c] = v(L, 0) + lt[size_t(c)] * v(L, 4) * 0.15f;
      float g = v(L, 1) * (1.0f + gt[size_t(c)] * v(L, 6) * 0.5f);
      cb.invGamma[c] = 1.0f / std::max(0.05f, g);
      cb.gain[c] = v(L, 2) * (1.0f + nt[size_t(c)] * v(L, 8) * 0.5f);
    }
  }
  {  // colour grading (zero-luma tint offsets)
    const FilterId G = FilterId::ColorGrading;
    float a = v(G, 0);
    Vec3 s = color::tintOffset(v(G, 1)), h = color::tintOffset(v(G, 3)), am = color::tintOffset(v(G, 6));
    for (int c = 0; c < 3; ++c) {
      cb.gradeShadow[c] = s[size_t(c)] * v(G, 2) * a * 0.8f;
      cb.gradeHigh[c] = h[size_t(c)] * v(G, 4) * a * 0.8f;
      cb.gradeAmbient[c] = am[size_t(c)] * v(G, 7) * a * 0.6f;
    }
    cb.gradeShadow[3] = v(G, 5);
  }
  if (m_lutSize > 0)
    set4(cb.lut, v(FilterId::Lut, 0), float(m_lutSize - 1) / float(m_lutSize), 0.5f / float(m_lutSize));
  set4(cb.bloom, v(FilterId::Bloom, 0) * 0.35f);
  set4(cb.vignette, v(FilterId::Vignette, 0), 0.25f + 0.6f * v(FilterId::Vignette, 1),
       0.15f + 0.75f * v(FilterId::Vignette, 2), float(in.width) / float(in.height));
  set4(cb.grain, v(FilterId::FilmGrain, 0), v(FilterId::FilmGrain, 1));
  set4(cb.chroma, v(FilterId::ChromaticAberration, 0) * 0.012f);

  {  // colour-vision assistance
    const FilterId C = FilterId::ColorVision;
    int type = int(std::lround(v(C, 1)));
    color::Mat3 sim = color::cvdSimulation(type);
    set4(cb.cvd0, sim[0], sim[1], sim[2]);
    set4(cb.cvd1, sim[3], sim[4], sim[5]);
    set4(cb.cvd2, sim[6], sim[7], sim[8]);
    set4(cb.cvdParams, v(C, 0), v(C, 2), v(C, 3) >= 0.5f ? 1.0f : 0.0f, type == 2 ? 1.0f : 0.0f);
  }
  {
    const FilterId F = FilterId::ColorFocus;
    float width = (15.0f + 85.0f * v(F, 2)) * kTwoPi / 360.0f;
    set4(cb.focus, v(F, 0), v(F, 1) * kTwoPi, width, v(F, 3));
  }
  for (int b = 0; b < kSelectiveBands; ++b) {
    const FilterId S = FilterId::SelectiveColor;
    set4(cb.selective[b], v(S, 1 + b * 3) * (30.0f * kTwoPi / 360.0f), v(S, 2 + b * 3), v(S, 3 + b * 3),
         selectiveBandHue(b) * kTwoPi);
  }
  set4(cb.selParams, v(FilterId::SelectiveColor, 0));
  updateCB(m_cbUber.Get(), cb);
}

ID3D11ShaderResourceView* FilterChain::process(const FilterFrameInput& in) {
  if (!m_dev || !in.source || in.width < 16 || in.height < 16) return in.source;
  if (!needsProcessing(in.compare)) return in.source;
  if (!ensureResources(in.width, in.height)) {
    logError("Filter resources could not be allocated for {}x{}", in.width, in.height);
    return in.source;
  }
  ++m_frame;

  Timing& tq = m_timing[size_t(m_timingWrite)];
  if (tq.pending) pollGpuMs();
  const bool timed = !tq.pending;
  if (timed) {
    m_ctx->Begin(tq.disjoint.Get());
    m_ctx->End(tq.begin.Get());
  }

  ID3D11ShaderResourceView* src = in.source;
  if (on(FilterId::Denoise) && m_fs.value(FilterId::Denoise, 0) > 0.0f) {
    float sigma = 0.25f - 0.22f * m_fs.value(FilterId::Denoise, 1);
    SmallCB cb{in.width, in.height, m_fs.value(FilterId::Denoise, 0), 1.0f / (2.0f * sigma * sigma), 0, 0, 0, 0};
    updateCB(m_cbSmall.Get(), cb);
    dispatch(m_csDenoise.Get(), m_cbSmall.Get(), {src}, m_denoised.uav.Get(), in.width, in.height);
    src = m_denoised.srv.Get();
  }

  if (m_pyr[0].tex && (on(FilterId::Detail) || on(FilterId::Clarity) || on(FilterId::LocalContrast) ||
                       on(FilterId::ShadowsHighlights))) {
    uint32_t sw = in.width, sh = in.height;
    ID3D11ShaderResourceView* prev = src;
    for (size_t i = 0; i < m_pyr.size(); ++i) {
      SmallCB cb{m_pyr[i].w, m_pyr[i].h, 1.0f / float(sw), 1.0f / float(sh), 0, 0, 0, 0};
      updateCB(m_cbSmall.Get(), cb);
      dispatch(i == 0 ? m_csPyrColor.Get() : m_csPyr.Get(), m_cbSmall.Get(), {prev}, m_pyr[i].uav.Get(), m_pyr[i].w,
               m_pyr[i].h);
      prev = m_pyr[i].srv.Get();
      sw = m_pyr[i].w;
      sh = m_pyr[i].h;
    }
  }

  ID3D11ShaderResourceView* bloom = nullptr;
  if (on(FilterId::Bloom) && m_bloomDown[0].tex && m_fs.value(FilterId::Bloom, 0) > 0.0f) {
    float thr = color::srgbToLinear(m_fs.value(FilterId::Bloom, 1));
    float upW = 0.35f + 0.65f * m_fs.value(FilterId::Bloom, 2);
    uint32_t sw = in.width, sh = in.height;
    ID3D11ShaderResourceView* prev = src;
    for (size_t i = 0; i < m_bloomDown.size(); ++i) {
      SmallCB cb{m_bloomDown[i].w, m_bloomDown[i].h, 1.0f / float(sw), 1.0f / float(sh), i == 0 ? 0u : 1u, thr,
                 std::max(1e-4f, thr * 0.5f), upW};
      updateCB(m_cbSmall.Get(), cb);
      dispatch(m_csBloom.Get(), m_cbSmall.Get(), {prev}, m_bloomDown[i].uav.Get(), m_bloomDown[i].w, m_bloomDown[i].h);
      prev = m_bloomDown[i].srv.Get();
      sw = m_bloomDown[i].w;
      sh = m_bloomDown[i].h;
    }
    // Upsample: up[i] = down[i] + w * tent(smaller)
    for (int i = int(m_bloomUp.size()) - 1; i >= 0; --i) {
      const Target& smaller = i == int(m_bloomUp.size()) - 1 ? m_bloomDown[size_t(i + 1)] : m_bloomUp[size_t(i + 1)];
      SmallCB cb{m_bloomUp[size_t(i)].w, m_bloomUp[size_t(i)].h, 1.0f / float(smaller.w), 1.0f / float(smaller.h), 2u,
                 thr, 0, upW};
      updateCB(m_cbSmall.Get(), cb);
      dispatch(m_csBloom.Get(), m_cbSmall.Get(), {smaller.srv.Get(), m_bloomDown[size_t(i)].srv.Get()},
               m_bloomUp[size_t(i)].uav.Get(), m_bloomUp[size_t(i)].w, m_bloomUp[size_t(i)].h);
    }
    bloom = m_bloomUp[0].srv.Get();
  }

  packConstants(in);
  dispatch(m_csUber.Get(), m_cbUber.Get(),
           {src, in.originalArray, m_pyr[0].srv.Get(), m_pyr[1].srv.Get(), m_pyr[2].srv.Get(), m_pyr[3].srv.Get(),
            bloom, m_lutSrv.Get()},
           m_out.uav.Get(), in.width, in.height);

  if (timed) {
    m_ctx->End(tq.end.Get());
    m_ctx->End(tq.disjoint.Get());
    tq.pending = true;
    m_timingWrite = (m_timingWrite + 1) % int(m_timing.size());
  }
  return m_out.srv.Get();
}

float FilterChain::pollGpuMs() {
  if (!m_ctx) return m_gpuMs;
  for (size_t k = 0; k < m_timing.size(); ++k) {
    Timing& t = m_timing[(size_t(m_timingWrite) + k) % m_timing.size()];
    if (!t.pending) continue;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    UINT64 b = 0, e = 0;
    if (m_ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    if (m_ctx->GetData(t.begin.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    if (m_ctx->GetData(t.end.Get(), &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    t.pending = false;
    if (!dj.Disjoint && dj.Frequency && e >= b) m_gpuMs = float(double(e - b) * 1000.0 / double(dj.Frequency));
  }
  return m_gpuMs;
}

float FilterChain::measureLastGpuMsBlocking() {
  Timing& t = m_timing[size_t((m_timingWrite + int(m_timing.size()) - 1) % int(m_timing.size()))];
  if (!t.pending) return m_gpuMs;
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
  UINT64 b = 0, e = 0;
  while (m_ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {}
  while (m_ctx->GetData(t.begin.Get(), &b, sizeof(b), 0) == S_FALSE) {}
  while (m_ctx->GetData(t.end.Get(), &e, sizeof(e), 0) == S_FALSE) {}
  t.pending = false;
  if (!dj.Disjoint && dj.Frequency && e >= b) m_gpuMs = float(double(e - b) * 1000.0 / double(dj.Frequency));
  return m_gpuMs;
}

bool FilterChain::updatePreview(ID3D11ShaderResourceView* frame, uint32_t w, uint32_t h) {
  if (!m_dev || !frame || w == 0 || h == 0) return false;
  uint32_t pw = 640, ph = std::max(1u, uint32_t(std::lround(640.0 * double(h) / double(w))));
  if (!m_preview || pw != m_previewW || ph != m_previewH) {
    m_preview.Reset();
    m_previewRtv.Reset();
    m_previewMutex.Reset();
    m_previewHandle = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = pw;
    d.Height = ph;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &m_preview))) return false;
    m_dev->CreateRenderTargetView(m_preview.Get(), nullptr, &m_previewRtv);
    m_preview.As(&m_previewMutex);
    ComPtr<IDXGIResource> res;
    m_preview.As(&res);
    if (res) res->GetSharedHandle(&m_previewHandle);
    m_previewW = pw;
    m_previewH = ph;
  }
  if (!m_previewMutex || m_previewMutex->AcquireSync(0, 0) != S_OK) return false;  // UI is reading: skip
  D3D11_VIEWPORT vp{0, 0, float(pw), float(ph), 0, 1};
  ID3D11RenderTargetView* rtv = m_previewRtv.Get();
  m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
  m_ctx->RSSetViewports(1, &vp);
  m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_ctx->IASetInputLayout(nullptr);
  m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
  m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
  m_ctx->PSSetShaderResources(0, 1, &frame);
  ID3D11SamplerState* s = m_linear.Get();
  m_ctx->PSSetSamplers(0, 1, &s);
  m_ctx->Draw(3, 0);
  ID3D11ShaderResourceView* nullSrv = nullptr;
  m_ctx->PSSetShaderResources(0, 1, &nullSrv);
  m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  m_previewMutex->ReleaseSync(0);
  return true;
}

bool FilterChain::readbackOutput(std::vector<uint8_t>& rgba) {
  if (!m_out.tex) return false;
  D3D11_TEXTURE2D_DESC d;
  m_out.tex->GetDesc(&d);
  d.BindFlags = 0;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> stg;
  if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &stg))) return false;
  m_ctx->CopyResource(stg.Get(), m_out.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(m_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  rgba.resize(size_t(d.Width) * d.Height * 4);
  for (UINT y = 0; y < d.Height; ++y)
    std::memcpy(rgba.data() + size_t(y) * d.Width * 4, static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                size_t(d.Width) * 4);
  m_ctx->Unmap(stg.Get(), 0);
  return true;
}

}  // namespace bl
