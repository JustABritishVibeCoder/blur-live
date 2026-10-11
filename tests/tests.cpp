// Blur Live test suite. Plain console executable, no framework dependency.
//   BlurLiveTests.exe            run everything
//   BlurLiveTests.exe <filter>   run tests whose name contains <filter>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/segment_planner.h"
#include "core/settings.h"
#include "core/util.h"
#include "core/weighting.h"
#include "capture/wgc_capture.h"
#include "capture/window_list.h"
#include "engine/blur_engine.h"
#include "pipeline/pipeline.h"
#include "present/output_window.h"

using Microsoft::WRL::ComPtr;
using namespace bl;

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------
static int g_checks = 0, g_failures = 0;
static std::string g_currentTest;

#define CHECK(cond, ...)                                                                    \
  do {                                                                                      \
    ++g_checks;                                                                             \
    if (!(cond)) {                                                                          \
      ++g_failures;                                                                         \
      std::printf("    FAIL %s:%d: %s ", __FILE__, __LINE__, #cond);                        \
      std::printf("" __VA_ARGS__);                                                          \
      std::printf("\n");                                                                    \
    }                                                                                       \
  } while (0)

static bool nearf(double a, double b, double eps) { return std::abs(a - b) <= eps; }

struct TestCase {
  const char* name;
  std::function<void()> fn;
};
static std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
struct Registrar {
  Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name)                                     \
  static void test_##name();                           \
  static Registrar reg_##name(#name, test_##name);     \
  static void test_##name()

// ---------------------------------------------------------------------------
// Weighting
// ---------------------------------------------------------------------------
TEST(weighting_sums_to_one) {
  for (int t = 0; t < int(Weighting::Count); ++t) {
    WeightingParams p;
    p.type = Weighting(t);
    for (int n : {1, 2, 5, 21, 64}) {
      auto w = computeWeights(n, p);
      CHECK(int(w.size()) == n);
      double s = std::accumulate(w.begin(), w.end(), 0.0);
      CHECK(nearf(s, 1.0, 1e-4), "type=%s n=%d sum=%f", weightingName(Weighting(t)), n, s);
      for (float x : w) CHECK(x >= 0.0f);
    }
  }
}

TEST(weighting_shapes) {
  WeightingParams p;
  p.type = Weighting::Equal;
  auto e = computeWeights(4, p);
  CHECK(nearf(e[0], 0.25, 1e-6) && nearf(e[3], 0.25, 1e-6));

  p.type = Weighting::Gaussian;  // Blur defaults: mean 2, bound [0,2] -> peak at newest
  auto g = computeWeights(9, p);
  CHECK(g.back() > g.front());
  CHECK(std::is_sorted(g.begin(), g.end()));

  p.type = Weighting::GaussianSym;
  auto gs = computeWeights(9, p);
  CHECK(nearf(gs[0], gs[8], 1e-6) && gs[4] > gs[0]);

  p.type = Weighting::Pyramid;
  auto py = computeWeights(5, p);  // 1,2,3,2,1 / 9
  CHECK(nearf(py[2], 3.0 / 9.0, 1e-6) && nearf(py[0], 1.0 / 9.0, 1e-6));

  p.type = Weighting::Ascending;
  auto as = computeWeights(4, p);  // 1,2,3,4 / 10
  CHECK(nearf(as[3], 0.4, 1e-6));

  p.type = Weighting::Vegas;
  auto vg = computeWeights(4, p);  // 1,2,2,1 / 6
  CHECK(nearf(vg[0], 1.0 / 6.0, 1e-6) && nearf(vg[1], 2.0 / 6.0, 1e-6));

  p.type = Weighting::Custom;
  p.custom = "1, 3";
  auto cu = computeWeights(4, p);  // stretched: 1,1,3,3 / 8
  CHECK(nearf(cu[0], 1.0 / 8.0, 1e-6) && nearf(cu[3], 3.0 / 8.0, 1e-6));

  std::string err;
  p.custom = "1,,x";
  auto bad = computeWeights(3, p, &err);
  CHECK(!err.empty());
  CHECK(nearf(bad[0], 1.0 / 3.0, 1e-6));

  auto lut = buildWeightLut(WeightingParams{});
  double mean = std::accumulate(lut.begin(), lut.end(), 0.0) / lut.size();
  CHECK(nearf(mean, 1.0, 1e-4));
}

// ---------------------------------------------------------------------------
// Segment planner
// ---------------------------------------------------------------------------
static std::vector<HistoryFrame> makeHistory(int n, double interval, double tNewest = 10.0) {
  std::vector<HistoryFrame> h;
  for (int i = 0; i < n; ++i) h.push_back({i, tNewest - i * interval, false});
  return h;
}

TEST(planner_60fps_one_segment) {
  auto h = makeHistory(4, 1.0 / 60.0);
  auto plan = planFlowSegments(h, 1.0 / 60.0, 48, 12);
  CHECK(plan.segments.size() == 1);
  CHECK(nearf(plan.segments[0].u0, 0.0, 1e-4));
  CHECK(nearf(plan.segments[0].tau0, 0.0, 1e-4) && nearf(plan.segments[0].tau1, 1.0, 1e-4));
  CHECK(nearf(plan.coverage, 1.0, 1e-4));
  CHECK(plan.segments[0].prevSlot == 1 && plan.segments[0].curSlot == 0);
}

TEST(planner_240fps_spans_four_pairs) {
  auto h = makeHistory(8, 1.0 / 240.0);
  auto plan = planFlowSegments(h, 1.0 / 60.0, 48, 12);
  CHECK(plan.segments.size() == 4, "got %zu", plan.segments.size());
  CHECK(nearf(plan.coverage, 1.0, 1e-3));
  CHECK(plan.totalSamples >= 48);
  for (auto& s : plan.segments) CHECK(nearf(s.tau1 - s.tau0, 0.25, 1e-3));
}

TEST(planner_partial_and_fractional) {
  // Shutter of half a frame interval: only the newest half of the newest pair.
  auto h = makeHistory(3, 1.0 / 60.0);
  auto plan = planFlowSegments(h, 0.5 / 60.0, 48, 12);
  CHECK(plan.segments.size() == 1);
  CHECK(nearf(plan.segments[0].u0, 0.5, 1e-3));
  // History shorter than the shutter -> coverage < 1.
  auto h2 = makeHistory(2, 1.0 / 240.0);
  auto plan2 = planFlowSegments(h2, 1.0 / 60.0, 48, 12);
  CHECK(plan2.segments.size() == 1);
  CHECK(nearf(plan2.coverage, 0.25, 1e-3));
}

TEST(planner_stops_at_discontinuity) {
  auto h = makeHistory(8, 1.0 / 240.0);
  h[2].discontinuityBefore = true;  // gap between frame 3 and frame 2
  auto plan = planFlowSegments(h, 1.0 / 60.0, 48, 12);
  CHECK(plan.segments.size() == 2);
  h[0].discontinuityBefore = true;
  CHECK(planFlowSegments(h, 1.0 / 60.0, 48, 12).segments.empty());
  CHECK(planFlowSegments(h, 0.0, 48, 12).segments.empty());
  CHECK(planFlowSegments(std::span<const HistoryFrame>(h.data(), 1), 1.0 / 60.0, 48, 12).segments.empty());
}

TEST(planner_blend) {
  auto h = makeHistory(10, 1.0 / 60.0);
  WeightingParams w;
  w.type = Weighting::Ascending;  // oldest-first ascending => newest has the most weight
  auto plan = planBlend(h, 4, w);
  CHECK(plan.slots.size() == 4);
  CHECK(plan.slots[0] == 0);
  CHECK(plan.weights[0] > plan.weights[3]);
  h[1].discontinuityBefore = true;
  CHECK(planBlend(h, 4, w).slots.size() == 2);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
TEST(settings_roundtrip) {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = std::wstring(tmp) + L"blurlive_test_settings.ini";
  Settings a;
  a.blurAmount = 1.37f;
  a.weighting.type = Weighting::Pyramid;
  a.weighting.custom = "4,5,6";
  a.outputMode = OutputMode::Monitor;
  a.quality = 3;
  a.lastTargetExe = "game.exe";
  a.lastTargetTitle = "My Game \xE2\x80\x94 Ultra";
  CHECK(saveSettings(a, path));
  Settings b;
  CHECK(loadSettings(b, path));
  CHECK(nearf(b.blurAmount, 1.37, 1e-4));
  CHECK(b.weighting.type == Weighting::Pyramid);
  CHECK(b.weighting.custom == "4,5,6");
  CHECK(b.outputMode == OutputMode::Monitor);
  CHECK(b.quality == 3);
  CHECK(b.lastTargetTitle == a.lastTargetTitle);
  DeleteFileW(path.c_str());

  Settings c;
  c.blurAmount = 99;
  c.quality = -4;
  sanitize(c);
  CHECK(c.blurAmount <= 3.0f && c.quality == 0);

  Settings d;
  applyProfile(d, Profile::Subtle);
  CHECK(nearf(d.blurAmount, 0.5, 1e-6));
  applyProfile(d, Profile::Responsive);
  CHECK(d.weighting.type == Weighting::Gaussian);
}

// ---------------------------------------------------------------------------
// GPU helpers
// ---------------------------------------------------------------------------
struct Gpu {
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  std::string adapter;
};

static bool createGpu(Gpu& g) {
  ComPtr<IDXGIFactory6> f6;
  ComPtr<IDXGIAdapter1> adapter;
  if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f6))))
    f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
  if (FAILED(hr)) return false;
  if (adapter) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    g.adapter = toUtf8(d.Description);
  }
  return true;
}

static Gpu& gpu() {
  static Gpu g;
  static bool tried = false;
  if (!tried) {
    tried = true;
    if (createGpu(g)) std::printf("  [gpu] %s\n", g.adapter.c_str());
  }
  return g;
}

static ComPtr<ID3D11Texture2D> makeTexture(const std::vector<uint32_t>& bgra, UINT w, UINT h) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{bgra.data(), w * 4, 0};
  ComPtr<ID3D11Texture2D> t;
  gpu().dev->CreateTexture2D(&d, &init, &t);
  return t;
}

static uint32_t grey(float v) {
  uint32_t c = uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
  return 0xFF000000u | (c << 16) | (c << 8) | c;
}

// Smooth pseudo-random texture (value noise, two octaves) for motion-estimation tests.
static float hash2(int x, int y, int seed) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return float((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
static float valueNoise(float x, float y, float scale, int seed) {
  float fx = x / scale, fy = y / scale;
  int ix = int(std::floor(fx)), iy = int(std::floor(fy));
  float tx = fx - ix, ty = fy - iy;
  tx = tx * tx * (3 - 2 * tx);
  ty = ty * ty * (3 - 2 * ty);
  float a = hash2(ix, iy, seed), b = hash2(ix + 1, iy, seed), c = hash2(ix, iy + 1, seed), d = hash2(ix + 1, iy + 1, seed);
  return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}
static std::vector<uint32_t> noiseFrame(UINT w, UINT h, float ox, float oy, int seed) {
  std::vector<uint32_t> px(size_t(w) * h);
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) {
      float sx = float(x) - ox, sy = float(y) - oy;
      float v = 0.65f * valueNoise(sx, sy, 12.0f, seed) + 0.35f * valueNoise(sx, sy, 4.0f, seed + 7);
      px[size_t(y) * w + x] = grey(v);
    }
  return px;
}

static float outGrey(const std::vector<uint8_t>& rgba, UINT w, UINT x, UINT y) {
  return float(rgba[(size_t(y) * w + x) * 4 + 1]) / 255.0f;  // G channel
}

static EngineParams defaultParams() {
  EngineParams p;
  p.shutterSeconds = 1.0 / 60.0;
  p.sampleBudget = 48;
  return p;
}

#define REQUIRE_GPU()                                         \
  if (!gpu().dev) {                                           \
    std::printf("    SKIP: no D3D11 hardware device\n");      \
    return;                                                   \
  }

// ---------------------------------------------------------------------------
// Engine tests
// ---------------------------------------------------------------------------
TEST(engine_static_frames_unchanged) {
  REQUIRE_GPU();
  BlurEngine e;
  std::string err;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), &err), "%s", err.c_str());
  const UINT W = 320, H = 180;
  auto f = noiseFrame(W, H, 0, 0, 1);
  auto t = makeTexture(f, W, H);
  for (int i = 0; i < 4; ++i) CHECK(e.submitFrame(t.Get(), W, H, i / 60.0, defaultParams()));
  std::vector<uint8_t> out;
  CHECK(e.readbackOutput(out));
  int maxDiff = 0;
  for (size_t i = 0; i < f.size(); ++i) maxDiff = std::max(maxDiff, std::abs(int(out[i * 4 + 1]) - int((f[i] >> 8) & 0xFF)));
  CHECK(maxDiff <= 1, "maxDiff=%d", maxDiff);
}

TEST(engine_motion_estimation_translation) {
  REQUIRE_GPU();
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  const UINT W = 640, H = 360;
  const float dx = 13.0f, dy = -6.0f;
  auto t0 = makeTexture(noiseFrame(W, H, 0, 0, 3), W, H);
  auto t1 = makeTexture(noiseFrame(W, H, dx, dy, 3), W, H);
  e.submitFrame(t0.Get(), W, H, 0.0, defaultParams());
  e.submitFrame(t1.Get(), W, H, 1.0 / 60.0, defaultParams());
  std::vector<float> flow;
  uint32_t gw = 0, gh = 0;
  CHECK(e.readbackNewestFlow(flow, gw, gh));
  int good = 0, total = 0;
  double sx = 0, sy = 0;
  for (uint32_t y = 4; y + 4 < gh; ++y)
    for (uint32_t x = 4; x + 4 < gw; ++x) {
      float fx = flow[(size_t(y) * gw + x) * 4 + 0], fy = flow[(size_t(y) * gw + x) * 4 + 1];
      ++total;
      sx += fx;
      sy += fy;
      if (std::abs(fx - dx) <= 1.5f && std::abs(fy - dy) <= 1.5f) ++good;
    }
  double frac = double(good) / std::max(1, total);
  std::printf("    flow grid %ux%u, mean (%.2f, %.2f), %.1f%% within 1.5 px of (%.0f, %.0f)\n", gw, gh, sx / total,
              sy / total, frac * 100, dx, dy);
  CHECK(frac > 0.9, "only %.1f%% of vectors correct", frac * 100);

  // Residual error statistics for genuine (sub-pixel) motion: must stay well below cut thresholds.
  for (float fdx : {7.5f, 13.3f, 2.25f}) {
    BlurEngine e2;
    e2.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr);
    auto a = makeTexture(noiseFrame(W, H, 0, 0, 8), W, H);
    auto b = makeTexture(noiseFrame(W, H, fdx, fdx * 0.4f, 8), W, H);
    e2.submitFrame(a.Get(), W, H, 0.0, defaultParams());
    e2.submitFrame(b.Get(), W, H, 1.0 / 60.0, defaultParams());
    e2.measureLastGpuMsBlocking();
    auto st = e2.pollGpuStats();
    std::printf("    sub-pixel motion (%.2f, %.2f): best=%.4f zero=%.4f ratio=%.3f\n", fdx, fdx * 0.4f,
                st.meanBlockError, st.meanZeroError, st.meanBlockError / std::max(1e-6f, st.meanZeroError));
    CHECK(!st.sceneCut);
  }
}

TEST(engine_large_motion) {
  REQUIRE_GPU();
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  const UINT W = 1280, H = 720;
  const float dx = -70.0f, dy = 22.0f;
  auto t0 = makeTexture(noiseFrame(W, H, 0, 0, 5), W, H);
  auto t1 = makeTexture(noiseFrame(W, H, dx, dy, 5), W, H);
  e.submitFrame(t0.Get(), W, H, 0.0, defaultParams());
  e.submitFrame(t1.Get(), W, H, 1.0 / 60.0, defaultParams());
  std::vector<float> flow;
  uint32_t gw = 0, gh = 0;
  e.readbackNewestFlow(flow, gw, gh);
  int good = 0, total = 0;
  for (uint32_t y = 12; y + 12 < gh; ++y)
    for (uint32_t x = 12; x + 12 < gw; ++x) {
      float fx = flow[(size_t(y) * gw + x) * 4 + 0], fy = flow[(size_t(y) * gw + x) * 4 + 1];
      ++total;
      if (std::abs(fx - dx) <= 2.0f && std::abs(fy - dy) <= 2.0f) ++good;
    }
  double frac = double(good) / std::max(1, total);
  std::printf("    %.1f%% of vectors within 2 px of (%.0f, %.0f)\n", frac * 100, dx, dy);
  CHECK(frac > 0.85);
}

// A textured bright square sliding over a flat background.
static std::vector<uint32_t> squareFrame(UINT w, UINT h, int sqX, int sqY, int size, float bg) {
  std::vector<uint32_t> px(size_t(w) * h, grey(bg));
  for (int y = sqY; y < sqY + size; ++y)
    for (int x = sqX; x < sqX + size; ++x) {
      if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) continue;
      bool checker = (((x - sqX) / 4) + ((y - sqY) / 4)) % 2 == 0;
      px[size_t(y) * w + x] = grey(checker ? 1.0f : 0.8f);
    }
  return px;
}

TEST(engine_motion_blur_streak) {
  REQUIRE_GPU();
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  const UINT W = 512, H = 256;
  const int size = 40, speed = 32, y0 = 100;
  const float bg = 0.1f;
  for (int i = 0; i < 4; ++i) {
    auto t = makeTexture(squareFrame(W, H, 60 + i * speed, y0, size, bg), W, H);
    e.submitFrame(t.Get(), W, H, i / 60.0, defaultParams());
  }
  std::vector<uint8_t> out;
  e.readbackOutput(out);
  const int curX = 60 + 3 * speed;
  const UINT row = UINT(y0 + size / 2);
  // Behind the square (inside the swept region) the background must be lifted towards the square.
  int lifted = 0;
  for (int x = curX - speed + 2; x < curX - 2; ++x)
    if (outGrey(out, W, UINT(x), row) > bg + 0.08f) ++lifted;
  // Far ahead and far behind: untouched background.
  float ahead = outGrey(out, W, UINT(curX + size + 20), row);
  float farBehind = outGrey(out, W, UINT(curX - speed - 20), row);
  // Energy conservation: excess brightness along the row ~= what one square contributes.
  double excessOut = 0;
  for (UINT x = 0; x < W; ++x) excessOut += outGrey(out, W, x, row) - bg;
  double excessSharp = 0;
  {
    auto sharp = squareFrame(W, H, curX, y0, size, bg);
    for (UINT x = 0; x < W; ++x) excessSharp += float((sharp[row * W + x] >> 8) & 0xFF) / 255.0f - bg;
  }
  std::printf("    lifted %d/%d px behind the square, ahead=%.3f behind=%.3f, energy ratio %.3f\n", lifted,
              speed - 4, ahead, farBehind, excessOut / excessSharp);
  CHECK(lifted >= (speed - 4) * 3 / 4);
  CHECK(nearf(ahead, bg, 0.02) && nearf(farBehind, bg, 0.02));
  CHECK(nearf(excessOut / excessSharp, 1.0, 0.12));
  // The streak must be continuous (no gaps = no discrete ghost copies): check monotonic-ish ramp.
  int dips = 0;
  float prev = 0;
  for (int x = curX - speed; x < curX + 2; ++x) {
    float v = outGrey(out, W, UINT(x), row);
    if (v + 0.06f < prev) ++dips;
    prev = v;
  }
  CHECK(dips <= 2, "dips=%d", dips);
}

TEST(engine_blend_mode_ghosts) {
  REQUIRE_GPU();
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  const UINT W = 256, H = 128;
  EngineParams p = defaultParams();
  p.mode = BlurMode::FrameBlend;
  p.blendFrames = 4;
  const float levels[4] = {0.40f, 0.42f, 0.44f, 0.46f};
  for (int i = 0; i < 4; ++i) {
    auto t = makeTexture(std::vector<uint32_t>(W * H, grey(levels[i])), W, H);
    e.submitFrame(t.Get(), W, H, i / 60.0, p);
  }
  std::vector<uint8_t> out;
  e.readbackOutput(out);
  float v = outGrey(out, W, 100, 60);
  CHECK(nearf(v, 0.43, 1.5 / 255.0), "v=%f", v);
}

TEST(engine_scene_cut_stays_sharp) {
  REQUIRE_GPU();
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  const UINT W = 320, H = 180;
  auto a = noiseFrame(W, H, 0, 0, 11);
  auto b = noiseFrame(W, H, 0, 0, 99);
  for (auto& px : b) px = grey(1.0f - float((px >> 8) & 0xFF) / 255.0f);  // invert: guaranteed different
  auto ta = makeTexture(a, W, H), tb = makeTexture(b, W, H);
  e.submitFrame(ta.Get(), W, H, 0.0, defaultParams());
  e.submitFrame(tb.Get(), W, H, 1.0 / 60.0, defaultParams());
  std::vector<uint8_t> out;
  e.readbackOutput(out);
  int maxDiff = 0;
  for (size_t i = 0; i < b.size(); ++i) maxDiff = std::max(maxDiff, std::abs(int(out[i * 4 + 1]) - int((b[i] >> 8) & 0xFF)));
  e.measureLastGpuMsBlocking();
  auto st = e.pollGpuStats();
  std::printf("    stats: best=%.4f zero=%.4f cut=%d\n", st.meanBlockError, st.meanZeroError, int(st.sceneCut));
  CHECK(st.motionValid && st.sceneCut);
  CHECK(maxDiff <= 2, "maxDiff=%d", maxDiff);
}

TEST(engine_time_gap_resets_history) {
  REQUIRE_GPU();
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  const UINT W = 320, H = 180;
  auto a = noiseFrame(W, H, 0, 0, 4), b = noiseFrame(W, H, 10, 0, 4);
  auto ta = makeTexture(a, W, H), tb = makeTexture(b, W, H);
  EngineFrameInfo info;
  e.submitFrame(ta.Get(), W, H, 0.0, defaultParams(), &info);
  e.submitFrame(tb.Get(), W, H, 0.5, defaultParams(), &info);  // 500 ms gap (game paused)
  CHECK(info.discontinuity);
  CHECK(info.segments == 0);
  std::vector<uint8_t> out;
  e.readbackOutput(out);
  int maxDiff = 0;
  for (size_t i = 0; i < b.size(); ++i) maxDiff = std::max(maxDiff, std::abs(int(out[i * 4 + 1]) - int((b[i] >> 8) & 0xFF)));
  CHECK(maxDiff <= 1, "maxDiff=%d", maxDiff);
}

TEST(engine_duplicate_timestamps_keep_blur) {
  REQUIRE_GPU();
  // Two frames for the same DWM composition (identical timestamps) must not reset the history.
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  const UINT W = 320, H = 180;
  std::vector<ComPtr<ID3D11Texture2D>> f;
  for (int i = 0; i < 4; ++i) f.push_back(makeTexture(noiseFrame(W, H, i * 6.0f, 0, 2), W, H));
  EngineFrameInfo info;
  e.submitFrame(f[0].Get(), W, H, 0.0, defaultParams(), &info);
  e.submitFrame(f[1].Get(), W, H, 1 / 240.0, defaultParams(), &info);
  e.submitFrame(f[2].Get(), W, H, 2 / 240.0, defaultParams(), &info);
  e.submitFrame(f[3].Get(), W, H, 2 / 240.0, defaultParams(), &info);  // same composition time as f[2]
  CHECK(!info.discontinuity);
  CHECK(info.segments >= 2, "segments=%d (history was reset)", info.segments);
  std::vector<float> flow;
  uint32_t gw = 0, gh = 0;
  e.readbackNewestFlow(flow, gw, gh);
  // The replacing frame (f[3], 18 px from f[1]) is matched against f[1].
  float fx = flow[(size_t(gh / 2) * gw + gw / 2) * 4];
  CHECK(std::abs(fx - 12.0f) < 1.5f, "flow %.2f", fx);
}

TEST(engine_resize_and_disable) {
  REQUIRE_GPU();
  BlurEngine e;
  CHECK(e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr));
  auto t1 = makeTexture(noiseFrame(300, 200, 0, 0, 1), 300, 200);
  auto t2 = makeTexture(noiseFrame(640, 400, 0, 0, 1), 640, 400);
  EngineFrameInfo info;
  CHECK(e.submitFrame(t1.Get(), 300, 200, 0.0, defaultParams(), &info));
  CHECK(e.submitFrame(t2.Get(), 640, 400, 1 / 60.0, defaultParams(), &info));
  CHECK(info.historyReset);
  CHECK(e.width() == 640 && e.height() == 400);
  // Cropped submit (pool texture larger than content).
  CHECK(e.submitFrame(t2.Get(), 600, 380, 2 / 60.0, defaultParams(), &info));
  CHECK(e.width() == 600 && e.height() == 380);
  EngineParams p = defaultParams();
  p.enabled = false;
  CHECK(e.submitFrame(t2.Get(), 600, 380, 3 / 60.0, p, &info));
  CHECK(info.segments == 0);
  CHECK(e.renderNewestUnblurred());
  CHECK(!e.submitFrame(nullptr, 600, 380, 4 / 60.0, p));
}

// ---------------------------------------------------------------------------
// GPU performance (measured, 1080p)
// ---------------------------------------------------------------------------
static void runPerf(const char* label, double fps, double shutter, int quality, BlurMode mode) {
  BlurEngine e;
  e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr);
  const UINT W = 1920, H = 1080;
  const int kFrames = 8;
  std::vector<ComPtr<ID3D11Texture2D>> frames;
  for (int i = 0; i < kFrames; ++i) frames.push_back(makeTexture(noiseFrame(W, H, i * 9.0f, i * 2.0f, 21), W, H));
  EngineParams p;
  p.shutterSeconds = shutter;
  p.sampleBudget = qualitySamples(quality);
  p.mode = mode;
  p.blendFrames = 6;
  std::vector<float> times;
  EngineFrameInfo info;
  for (int i = 0; i < 60; ++i) {
    e.submitFrame(frames[size_t(i % kFrames)].Get(), W, H, i / fps, p, &info);
    float ms = e.measureLastGpuMsBlocking();
    if (i >= 10 && ms >= 0) times.push_back(ms);
  }
  std::sort(times.begin(), times.end());
  double avg = std::accumulate(times.begin(), times.end(), 0.0) / std::max<size_t>(1, times.size());
  std::printf("    %-40s avg %.3f ms  p95 %.3f ms  (segments %d, samples %d)\n", label, avg,
              times.empty() ? 0.0 : times[times.size() * 95 / 100], info.segments, info.samples);
  CHECK(!times.empty());
}

TEST(perf_1080p) {
  REQUIRE_GPU();
  runPerf("1080p @60 fps, 1/60 s, High (48)", 60, 1 / 60.0, 2, BlurMode::MotionFlow);
  runPerf("1080p @144 fps, 1/60 s, High (48)", 144, 1 / 60.0, 2, BlurMode::MotionFlow);
  runPerf("1080p @240 fps, 1/60 s, Medium (24)", 240, 1 / 60.0, 1, BlurMode::MotionFlow);
  runPerf("1080p @240 fps, 1/60 s, High (48)", 240, 1 / 60.0, 2, BlurMode::MotionFlow);
  runPerf("1080p @240 fps, 1/60 s, Ultra (96)", 240, 1 / 60.0, 3, BlurMode::MotionFlow);
  runPerf("1080p @280 fps, 1.75/60 s, High (48)", 280, 1.75 / 60.0, 2, BlurMode::MotionFlow);
  runPerf("1080p frame blend, 6 frames", 240, 1 / 60.0, 2, BlurMode::FrameBlend);
}

#include "tests_capture.inl"
#include "tests_filters.inl"

// ---------------------------------------------------------------------------
// Developer utility: BlurLiveTests.exe --snap "<window title substring>" out.png
// Captures one frame of a window through WGC and writes it as PNG (used to verify the UI visually).
#include <wincodec.h>

// screen == true: capture the monitor under the window and crop to the window's client area, i.e.
// what the user actually sees there (including any overlay on top).
static int snapWindow(const std::string& titleUtf8, const std::wstring& outPath, bool screen = false) {
  std::wstring needle = toWide(titleUtf8);
  HWND target = nullptr;
  auto windows = enumerateCaptureTargets(GetCurrentProcessId(), true);
  for (auto& w : windows)  // exact title first, then substring
    if (w.title == needle) target = w.hwnd;
  for (auto& w : windows)
    if (!target && w.title.find(needle) != std::wstring::npos) target = w.hwnd;
  if (!target || !gpu().dev) {
    std::printf("window not found or no GPU\n");
    return 2;
  }
  WgcCapture cap;
  std::string err;
  HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  RECT crop = windowClientRect(target);
  bool started = screen ? cap.startMonitor(mon, gpu().dev.Get(), CaptureOptions{}, &err)
                        : cap.startWindow(target, gpu().dev.Get(), CaptureOptions{}, &err);
  if (!started) {
    std::printf("capture failed: %s\n", err.c_str());
    return 3;
  }
  CapturedFrame f;
  double end = qpcSeconds() + 3.0;
  while (qpcSeconds() < end) {
    HANDLE ev = cap.frameEvent();
    MsgWaitForMultipleObjects(1, &ev, FALSE, 50, QS_ALLINPUT);
    if (cap.acquireLatest(f)) break;
  }
  if (!f.texture) {
    std::printf("no frame\n");
    return 4;
  }
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

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> wic;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  ComPtr<IWICStream> stream;
  wic->CreateStream(&stream);
  stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE);
  ComPtr<IWICBitmapEncoder> enc;
  wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
  enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> frame;
  enc->CreateNewFrame(&frame, nullptr);
  frame->Initialize(nullptr);
  UINT ox = 0, oy = 0, ow = f.width, oh = f.height;
  if (screen) {
    ox = UINT(std::max<LONG>(0, crop.left - mi.rcMonitor.left));
    oy = UINT(std::max<LONG>(0, crop.top - mi.rcMonitor.top));
    ow = std::min<UINT>(UINT(crop.right - crop.left), f.width - ox);
    oh = std::min<UINT>(UINT(crop.bottom - crop.top), f.height - oy);
  }
  frame->SetSize(ow, oh);
  WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&pf);
  BYTE* base = static_cast<BYTE*>(m.pData) + size_t(oy) * m.RowPitch + size_t(ox) * 4;
  frame->WritePixels(oh, m.RowPitch, m.RowPitch * (oh - 1) + ow * 4, base);
  f.width = ow;
  f.height = oh;
  frame->Commit();
  enc->Commit();
  gpu().ctx->Unmap(stg.Get(), 0);
  std::printf("saved %ux%u\n", f.width, f.height);
  return 0;
}

int main(int argc, char** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (argc == 4 && std::string(argv[1]) == "--snap") return snapWindow(argv[2], toWide(argv[3]));
  if (argc == 4 && std::string(argv[1]) == "--snap-screen") return snapWindow(argv[2], toWide(argv[3]), true);
  if (argc == 3 && std::string(argv[1]) == "--demo-window") {
    // A stand-in "game": animated content for manual / scripted end-to-end runs of BlurLive.exe.
    TestWindow tw;
    tw.open(80, 80, 960, 540, TestWindow::Pattern::MovingBar, L"BlurLive Demo Game");
    pumpFor(std::atof(argv[2]));
    return 0;
  }
  std::string filter = argc > 1 ? argv[1] : "";
  int ran = 0;
  for (auto& t : registry()) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
    int before = g_failures;
    std::printf("[ RUN  ] %s\n", t.name);
    auto t0 = std::chrono::steady_clock::now();
    t.fn();
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[ %s ] %s (%.0f ms)\n", g_failures == before ? " OK " : "FAIL", t.name, ms);
    ++ran;
  }
  std::printf("\n%d tests, %d checks, %d failures\n", ran, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
