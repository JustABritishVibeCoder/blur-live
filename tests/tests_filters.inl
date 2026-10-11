// Filter-suite tests (included from tests.cpp).
#include <random>

#include "filters/color_math.h"
#include "filters/filter_chain.h"
#include "filters/lut.h"
#include "filters/presets.h"

// Colourful test image: smooth noise in three channels, a dark region, a bright highlight region.
static std::vector<uint32_t> colorFrame(UINT w, UINT h, int seed) {
  std::vector<uint32_t> px(size_t(w) * h);
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) {
      float r = valueNoise(float(x), float(y), 14.0f, seed);
      float g = valueNoise(float(x), float(y), 11.0f, seed + 3);
      float b = valueNoise(float(x), float(y), 17.0f, seed + 9);
      if (x < w / 5) { r *= 0.15f; g *= 0.15f; b *= 0.15f; }                     // dark band
      if (x > w * 4 / 5 && y < h / 3) { r = 0.9f + 0.1f * r; g = 0.9f + 0.1f * g; b = 0.85f + 0.15f * b; }  // highlights
      auto q = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
      px[size_t(y) * w + x] = 0xFF000000u | (q(r) << 16) | (q(g) << 8) | q(b);
    }
  return px;
}

struct FilterRig {
  FilterChain chain;
  UINT w = 0, h = 0;
  std::vector<uint32_t> input;
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11ShaderResourceView> srv;
  bool init(UINT W, UINT H, int seed = 5) {
    w = W;
    h = H;
    input = colorFrame(w, h, seed);
    tex = makeTexture(input, w, h);
    gpu().dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    std::string err;
    return chain.init(gpu().dev.Get(), gpu().ctx.Get(), &err);
  }
  // Runs the chain; returns false if it passed the source through without processing.
  bool run(const FilterSettings& fs, std::vector<uint8_t>& out, bool compare = false,
           ID3D11ShaderResourceView* original = nullptr) {
    chain.setSettings(fs);
    FilterFrameInput in{srv.Get(), w, h, original, original ? 0 : -1, compare, 0.5f};
    ID3D11ShaderResourceView* res = chain.process(in);
    if (res == srv.Get()) return false;
    chain.readbackOutput(out);
    return true;
  }
  // Max per-channel difference between the input image and an RGBA8 output.
  int maxDiff(const std::vector<uint8_t>& out, UINT x0 = 0, UINT x1 = ~0u) const {
    int m = 0;
    for (UINT y = 0; y < h; ++y)
      for (UINT x = x0; x < std::min(x1, w); ++x) {
        uint32_t p = input[size_t(y) * w + x];
        const uint8_t* o = &out[(size_t(y) * w + x) * 4];
        m = std::max({m, std::abs(int(o[0]) - int((p >> 16) & 0xFF)), std::abs(int(o[1]) - int((p >> 8) & 0xFF)),
                      std::abs(int(o[2]) - int(p & 0xFF))});
      }
    return m;
  }
};

TEST(filters_registry_defaults) {
  const auto& reg = filterRegistry();
  CHECK(int(reg.size()) == kFilterCount);
  FilterSettings fs = defaultFilterSettings();
  CHECK(enabledFilterCount(fs) == 0, "every filter must start disabled");
  CHECK(!fs[FilterId::Saturation].enabled);
  for (const auto& d : reg) {
    CHECK(!d.params.empty(), "%s has no parameters", d.name);
    for (const auto& p : d.params) {
      CHECK(p.def >= p.min && p.def <= p.max, "%s.%s default out of range", d.key, p.key);
      CHECK(p.neutral >= p.min && p.neutral <= p.max, "%s.%s neutral out of range", d.key, p.key);
    }
    for (const auto& e : reg) CHECK(&d == &e || std::strcmp(d.key, e.key) != 0, "duplicate key %s", d.key);
  }
  // No built-in preset may switch saturation on (it must only ever be enabled explicitly).
  for (const auto& p : builtinPresets()) CHECK(!p.filters[FilterId::Saturation].enabled, "%s", p.name.c_str());
  // The competitive preset must not add distractions.
  const Preset* comp = nullptr;
  auto builtins = builtinPresets();
  for (auto& p : builtins)
    if (p.name == "VALORANT Competitive") comp = &p;
  CHECK(comp != nullptr);
  if (comp) {
    CHECK(!comp->blurEnabled);
    for (FilterId id : {FilterId::Bloom, FilterId::FilmGrain, FilterId::ChromaticAberration, FilterId::Vignette})
      CHECK(!comp->filters[id].enabled);
  }
}

TEST(filters_disabled_is_zero_cost_passthrough) {
  REQUIRE_GPU();
  FilterRig rig;
  CHECK(rig.init(320, 180));
  std::vector<uint8_t> out;
  CHECK(!rig.run(defaultFilterSettings(), out), "with every filter off the source must be returned untouched");
  FilterSettings fs = defaultFilterSettings();
  fs[FilterId::Lut].enabled = true;  // LUT enabled but no file loaded: still nothing to do
  CHECK(!rig.run(fs, out));
}

TEST(filters_neutral_values_are_identity) {
  REQUIRE_GPU();
  FilterRig rig;
  CHECK(rig.init(320, 180));
  std::vector<uint8_t> out;
  FilterSettings all = defaultFilterSettings();
  for (const auto& d : filterRegistry()) {
    FilterSettings fs = defaultFilterSettings();
    neutralizeFilter(fs, d.id);
    fs[d.id].enabled = true;
    neutralizeFilter(all, d.id);
    all[d.id].enabled = true;
    if (!rig.run(fs, out)) continue;  // e.g. LUT without a file
    int md = rig.maxDiff(out);
    CHECK(md <= 1, "%s at neutral changed the image by %d/255", d.name, md);
    CHECK(!filterAltersImage(fs, d.id), "%s reports a change at neutral", d.name);
  }
  CHECK(rig.run(all, out));
  int md = rig.maxDiff(out);
  std::printf("    all %d filters enabled at neutral: max difference %d/255\n", kFilterCount, md);
  CHECK(md <= 1);
}

TEST(filters_toggle_independently) {
  REQUIRE_GPU();
  FilterRig rig;
  CHECK(rig.init(320, 180));
  std::vector<uint8_t> base, out;
  // Reference: a different filter enabled, to prove toggling X doesn't disturb Y.
  FilterSettings fs = defaultFilterSettings();
  fs[FilterId::Vignette].enabled = true;
  CHECK(rig.run(fs, base));
  for (const auto& d : filterRegistry()) {
    if (d.id == FilterId::Vignette || d.id == FilterId::Lut || d.id == FilterId::RgbChannels ||
        d.id == FilterId::SelectiveColor)
      continue;  // defaults of these are neutral by design (LUT needs a file)
    FilterSettings t = fs;
    t[d.id].enabled = true;
    rig.run(t, out);
    int changed = 0;
    for (size_t i = 0; i < out.size(); ++i) changed += out[i] != base[i];
    CHECK(changed > 0, "%s with its default values had no visible effect", d.name);
    // Settings of the other filters are untouched by toggling.
    for (const auto& e : filterRegistry())
      if (e.id != d.id) CHECK(t[e.id] == fs[e.id]);
    // Disabling again restores the reference. Both frames carry the +-1 LSB temporal output dither
    // (re-seeded every frame), so two independent frames can differ by up to 2 LSB.
    t[d.id].enabled = false;
    rig.run(t, out);
    int md = 0;
    for (size_t i = 0; i < out.size(); ++i) md = std::max(md, std::abs(int(out[i]) - int(base[i])));
    CHECK(md <= 2, "%s: disabling did not restore the image (diff %d)", d.name, md);
  }
}

TEST(filters_sharpen_quality) {
  REQUIRE_GPU();
  // A soft vertical edge plus faint noise in the flat areas.
  const UINT W = 256, H = 64;
  std::vector<uint32_t> img(W * H);
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      float t = std::clamp((float(x) - 126.0f) / 3.0f, 0.0f, 1.0f);  // a slightly soft 3 px edge
      float v = 0.25f + 0.5f * (t * t * (3 - 2 * t));
      if (hash2(int(x), int(y), 1) > 0.5f) v += 1.0f / 255.0f;  // 1-LSB noise
      img[y * W + x] = grey(v);
    }
  FilterRig rig;
  CHECK(rig.init(W, H));
  rig.input = img;
  rig.tex = makeTexture(img, W, H);
  rig.srv.Reset();
  gpu().dev->CreateShaderResourceView(rig.tex.Get(), nullptr, &rig.srv);
  FilterSettings fs = defaultFilterSettings();
  fs[FilterId::Sharpen].enabled = true;
  fs.set(FilterId::Sharpen, "intensity", 1.0f);
  std::vector<uint8_t> out;
  rig.run(fs, out);
  const UINT row = 32;
  auto in = [&](UINT x) { return float((img[row * W + x] >> 8) & 0xFF); };
  auto ou = [&](UINT x) { return float(out[(row * W + x) * 4 + 1]); };
  float gIn = 0, gOut = 0, lo = 1e9f, hi = -1e9f;
  for (UINT x = 100; x < 156; ++x) {
    gIn = std::max(gIn, std::abs(in(x + 1) - in(x)));
    gOut = std::max(gOut, std::abs(ou(x + 1) - ou(x)));
    lo = std::min(lo, ou(x));
    hi = std::max(hi, ou(x));
  }
  float flatChange = 0;
  for (UINT x = 20; x < 80; ++x) flatChange = std::max(flatChange, std::abs(ou(x) - in(x)));
  std::printf("    edge gradient %.1f -> %.1f per px, output range [%.0f, %.0f] vs input [64, 192], flat-area change "
              "%.0f LSB\n",
              gIn, gOut, lo, hi, flatChange);
  CHECK(gOut > gIn * 1.15f, "edge not sharper");
  CHECK(lo >= 64 - 6 && hi <= 192 + 6, "halo/overshoot too large");
  CHECK(flatChange <= 1.0f, "noise was amplified");
}

TEST(filters_color_correctness) {
  REQUIRE_GPU();
  // Grey ramp on top, saturated colour patches below.
  const UINT W = 256, H = 64;
  std::vector<uint32_t> img(W * H);
  const uint32_t patches[4] = {0xFFC83C3Cu, 0xFF3CB43Cu, 0xFF3C50C8u, 0xFFD2B43Cu};  // ARGB-ish: r, g, b, yellow
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) img[y * W + x] = y < 32 ? grey(float(x) / float(W - 1)) : patches[(x / 64) % 4];
  FilterRig rig;
  CHECK(rig.init(W, H));
  rig.input = img;
  rig.tex = makeTexture(img, W, H);
  rig.srv.Reset();
  gpu().dev->CreateShaderResourceView(rig.tex.Get(), nullptr, &rig.srv);
  std::vector<uint8_t> out;
  auto px = [&](UINT x, UINT y) { return &out[(size_t(y) * W + x) * 4]; };

  // Saturation +50%: greys stay grey, patches gain chroma, hue preserved, no clipping artefacts.
  FilterSettings fs = defaultFilterSettings();
  fs[FilterId::Saturation].enabled = true;
  fs.set(FilterId::Saturation, "amount", 0.5f);
  rig.run(fs, out);
  int greyErr = 0;
  for (UINT x = 0; x < W; ++x) {
    const uint8_t* p = px(x, 10);
    greyErr = std::max({greyErr, std::abs(p[0] - p[1]), std::abs(p[1] - p[2])});
  }
  CHECK(greyErr <= 1, "saturation tinted greys by %d", greyErr);
  auto chroma = [](float r, float g, float b) {
    auto lab = color::linearToOklab({color::srgbToLinear(r / 255), color::srgbToLinear(g / 255), color::srgbToLinear(b / 255)});
    return std::array<float, 2>{std::hypot(lab[1], lab[2]), std::atan2(lab[2], lab[1])};
  };
  for (int k = 0; k < 4; ++k) {
    uint32_t s = patches[k];
    const uint8_t* p = px(UINT(k * 64 + 32), 48);
    auto before = chroma(float((s >> 16) & 0xFF), float((s >> 8) & 0xFF), float(s & 0xFF));
    auto after = chroma(p[0], p[1], p[2]);
    float dh = std::abs(std::remainder(after[1] - before[1], 6.2831853f)) * 57.29578f;
    // In-gamut patches gain the full amount; colours at the gamut edge are compressed, not clipped.
    CHECK(after[0] > before[0] * 1.1f, "patch %d chroma %.3f -> %.3f", k, before[0], after[0]);
    CHECK(dh < 6.0f, "patch %d hue shifted %.1f deg", k, dh);
  }

  // Warm white balance: neutral greys become warmer (R > B) without changing brightness much.
  fs = defaultFilterSettings();
  fs[FilterId::WhiteBalance].enabled = true;
  fs.set(FilterId::WhiteBalance, "temperature", 0.6f);
  rig.run(fs, out);
  const uint8_t* mid = px(128, 10);
  int inGrey = int((img[10 * W + 128] >> 8) & 0xFF);
  CHECK(mid[0] > mid[2] + 8, "temperature did not warm the image (%d,%d,%d)", mid[0], mid[1], mid[2]);
  CHECK(std::abs((mid[0] + mid[1] + mid[2]) / 3 - inGrey) < 12, "white balance changed brightness too much");
  auto wb0 = color::whiteBalanceMatrix(0, 0);
  CHECK(wb0 == color::identity());

  // Contrast and brightness never clip the extremes of a ramp into flat runs.
  fs = defaultFilterSettings();
  fs[FilterId::Contrast].enabled = true;
  fs.set(FilterId::Contrast, "amount", 0.5f);
  fs[FilterId::Brightness].enabled = true;
  fs.set(FilterId::Brightness, "amount", 0.3f);
  rig.run(fs, out);
  int flatAtTop = 0;
  for (UINT x = W - 40; x < W - 1; ++x) flatAtTop += px(x, 10)[1] == 255 ? 1 : 0;
  CHECK(flatAtTop < 8, "highlights clipped over %d px", flatAtTop);
}

TEST(filters_no_dark_halo_around_bright_objects) {
  REQUIRE_GPU();
  // Dark flat background with a bright bar: shadow lift must be (almost) the same next to the bar
  // as far away from it.
  const UINT W = 320, H = 160;
  std::vector<uint32_t> img(W * H, grey(0.15f));
  for (UINT y = 40; y < 120; ++y)
    for (UINT x = 140; x < 180; ++x) img[y * W + x] = grey(0.97f);
  FilterRig rig;
  CHECK(rig.init(W, H));
  rig.input = img;
  rig.tex = makeTexture(img, W, H);
  rig.srv.Reset();
  gpu().dev->CreateShaderResourceView(rig.tex.Get(), nullptr, &rig.srv);
  FilterSettings fs = defaultFilterSettings();
  fs[FilterId::ShadowsHighlights].enabled = true;
  fs.set(FilterId::ShadowsHighlights, "shadows", 0.6f);
  fs.set(FilterId::ShadowsHighlights, "highlights", 0.0f);
  std::vector<uint8_t> out;
  rig.run(fs, out);
  auto g = [&](UINT x, UINT y) { return int(out[(size_t(y) * W + x) * 4 + 1]); };
  int away = g(20, 80), beside1 = g(134, 80), beside2 = g(186, 80);
  std::printf("    background lifted to %d far away, %d / %d beside the bar (input %d)\n", away, beside1, beside2,
              int(0.15f * 255 + 0.5f));
  CHECK(away > 45, "shadows were not lifted");
  CHECK(std::abs(beside1 - away) <= 3 && std::abs(beside2 - away) <= 3, "halo beside bright object");
}

TEST(filters_compare_view) {
  REQUIRE_GPU();
  FilterRig rig;
  CHECK(rig.init(320, 180));
  // "Original" = the same frame stored in a 1-slice texture array (like the engine's history ring).
  D3D11_TEXTURE2D_DESC d;
  rig.tex->GetDesc(&d);
  ComPtr<ID3D11ShaderResourceView> arr;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = d.Format;
  sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  sv.Texture2DArray.MipLevels = 1;
  sv.Texture2DArray.ArraySize = 1;
  gpu().dev->CreateShaderResourceView(rig.tex.Get(), &sv, &arr);
  FilterSettings fs = defaultFilterSettings();
  fs[FilterId::Contrast].enabled = true;
  fs.set(FilterId::Contrast, "amount", 0.8f);
  std::vector<uint8_t> out;
  CHECK(rig.run(fs, out, true, arr.Get()));
  int left = rig.maxDiff(out, 0, rig.w / 2 - 2);
  int right = rig.maxDiff(out, rig.w / 2 + 2, rig.w);
  CHECK(left == 0, "left half must be the untouched original (diff %d)", left);
  CHECK(right > 10, "right half must be processed");
  // Compare with no filters: still exact original on the left.
  CHECK(rig.run(defaultFilterSettings(), out, true, arr.Get()));
  CHECK(rig.maxDiff(out, 0, rig.w / 2 - 2) == 0);
}

TEST(filters_presets_roundtrip) {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring dir = std::wstring(tmp) + L"blurlive_preset_test";
  CreateDirectoryW(dir.c_str(), nullptr);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.blpreset").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  PresetStore store(dir);
  size_t builtins = store.all().size();

  // Random values for every parameter of every filter.
  std::mt19937 rng(42);
  Preset p;
  p.name = "My Look";
  p.description = "test";
  for (const auto& d : filterRegistry()) {
    p.filters[d.id].enabled = rng() % 2 == 0;
    for (size_t i = 0; i < d.params.size(); ++i) {
      std::uniform_real_distribution<float> u(d.params[i].min, d.params[i].max);
      float v = u(rng);
      if (d.params[i].kind == ParamKind::Choice) v = std::round(v);
      p.filters[d.id].values[i] = v;
    }
  }
  p.filters.lutPath = "C:\\luts\\look one.cube";
  p.blurEnabled = true;
  p.blurAmount = 1.37f;
  p.quality = 3;
  std::string err;
  CHECK(store.save(p, &err), "%s", err.c_str());
  PresetStore reread(dir);
  const Preset* q = reread.find("My Look");
  CHECK(q != nullptr);
  if (q) {
    CHECK(q->filters == p.filters, "filter settings changed in the round trip");
    CHECK(q->blurEnabled && q->blurAmount == p.blurAmount && q->quality == 3);
  }
  CHECK(store.duplicate("My Look", "", &err));
  CHECK(store.find("My Look copy") != nullptr);
  CHECK(store.rename("My Look copy", "Renamed", &err), "%s", err.c_str());
  CHECK(store.find("Renamed") && !store.find("My Look copy"));
  std::wstring exported = dir + L"\\exported.txt";
  CHECK(store.exportFile("Renamed", exported, &err));
  std::string imported;
  CHECK(store.importFile(exported, &imported, &err), "%s", err.c_str());
  CHECK(imported == "Renamed (2)", "import name %s", imported.c_str());
  CHECK(!store.remove("VALORANT Competitive", &err), "built-ins must not be deletable");
  CHECK(!store.save(Preset{"Original"}, &err), "built-ins must not be overwritten");
  CHECK(store.remove("Renamed", &err) && store.remove("Renamed (2)", &err) && store.remove("My Look", &err));
  CHECK(store.all().size() == builtins);
  DeleteFileW(exported.c_str());

  // Settings round trip including filters and the master switch.
  Settings s;
  s.filters = p.filters;
  s.effectsEnabled = false;
  s.activePreset = "My Look";
  std::wstring path = dir + L"\\settings.ini";
  CHECK(saveSettings(s, path));
  Settings r;
  CHECK(loadSettings(r, path));
  CHECK(r.filters == s.filters && !r.effectsEnabled && r.activePreset == "My Look");
  DeleteFileW(path.c_str());
  RemoveDirectoryW(dir.c_str());

  // Apply preset -> settings -> capture back gives the same preset content.
  Settings a;
  applyPresetToSettings(p, a);
  Preset back = presetFromSettings(a, p.name);
  CHECK(back.filters == p.filters && back.blurAmount == p.blurAmount);
}

static std::string identityCube(int n) {
  std::string s = "TITLE \"identity\"\nLUT_3D_SIZE " + std::to_string(n) + "\n";
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r) {
        char line[96];
        std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\n", r / double(n - 1), g / double(n - 1), b / double(n - 1));
        s += line;
      }
  return s;
}

TEST(filters_lut) {
  CubeLut lut;
  std::string err;
  CHECK(parseCubeLut(identityCube(17), lut, &err), "%s", err.c_str());
  CHECK(lut.size == 17 && lut.title == "identity");
  CHECK(!parseCubeLut("LUT_3D_SIZE 2\n0 0 0\n", lut, &err));  // too few entries
  CHECK(!parseCubeLut("LUT_1D_SIZE 16\n", lut, &err));
  CHECK(!parseCubeLut("garbage", lut, &err));
  REQUIRE_GPU();
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = std::wstring(tmp) + L"blurlive_identity.cube";
  {
    std::ofstream f(path, std::ios::binary);
    f << identityCube(33);
  }
  FilterRig rig;
  CHECK(rig.init(320, 180));
  FilterSettings fs = defaultFilterSettings();
  fs[FilterId::Lut].enabled = true;
  fs.lutPath = toUtf8(path);
  std::vector<uint8_t> out;
  CHECK(rig.run(fs, out));
  CHECK(rig.chain.lutLoaded(), "%s", rig.chain.lutMessage().c_str());
  int md = rig.maxDiff(out);
  CHECK(md <= 1, "identity LUT changed the image by %d", md);
  fs.lutPath = "C:\\does\\not\\exist.cube";
  CHECK(!rig.run(fs, out));  // missing LUT: filter is skipped, not an error
  CHECK(!rig.chain.lutMessage().empty());
  DeleteFileW(path.c_str());
}

TEST(filters_full_stack_and_presets_render_sanely) {
  REQUIRE_GPU();
  FilterRig rig;
  CHECK(rig.init(480, 270));
  std::vector<uint8_t> out;
  auto check = [&](const FilterSettings& fs, const char* what) {
    if (!rig.run(fs, out)) {
      std::printf("    %-32s pass-through (no filters enabled)\n", what);
      return;
    }
    double mean = 0;
    int bad = 0;
    for (size_t i = 0; i < out.size(); i += 4) {
      mean += (out[i] + out[i + 1] + out[i + 2]) / 3.0;
      bad += out[i + 3] != 255;
    }
    mean /= double(out.size() / 4);
    std::printf("    %-32s mean level %.0f/255\n", what, mean);
    CHECK(bad == 0, "%s: alpha not opaque", what);
    CHECK(mean > 20 && mean < 235, "%s: image blown out or black (mean %.0f)", what, mean);
  };
  FilterSettings all = defaultFilterSettings();
  for (const auto& d : filterRegistry()) all[d.id].enabled = true;
  check(all, "every filter at defaults");
  for (const auto& p : builtinPresets()) check(p.filters, p.name.c_str());
}

static void filterPerf(const char* label, const FilterSettings& fs, bool blur) {
  const UINT W = 1920, H = 1080;
  BlurEngine e;
  e.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr);
  FilterChain chain;
  chain.init(gpu().dev.Get(), gpu().ctx.Get(), nullptr);
  chain.setSettings(fs);
  std::vector<ComPtr<ID3D11Texture2D>> frames;
  for (int i = 0; i < 4; ++i) frames.push_back(makeTexture(colorFrame(W, H, 30 + i), W, H));
  EngineParams p;
  p.enabled = blur;
  std::vector<float> fx, en;
  for (int i = 0; i < 50; ++i) {
    e.submitFrame(frames[size_t(i % 4)].Get(), W, H, i / 240.0, p);
    float em = e.measureLastGpuMsBlocking();
    FilterFrameInput in{e.outputSRV(), W, H, e.frameHistorySRV(), e.newestSlice(), false, 0.5f};
    bool ran = chain.process(in) != e.outputSRV();
    float fm = ran ? chain.measureLastGpuMsBlocking() : 0.0f;
    if (i >= 10) {
      fx.push_back(fm);
      en.push_back(em);
    }
  }
  auto avg = [](const std::vector<float>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / double(v.size()); };
  std::sort(fx.begin(), fx.end());
  std::printf("    %-44s filters %.3f ms (p95 %.3f)  + engine %.3f ms  = %.3f ms GPU/frame\n", label, avg(fx),
              fx[fx.size() * 95 / 100], avg(en), avg(fx) + avg(en));
  CHECK(!fx.empty());
}

TEST(perf_filters_1080p) {
  REQUIRE_GPU();
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring lutPath = std::wstring(tmp) + L"blurlive_perf.cube";
  {
    std::ofstream f(lutPath, std::ios::binary);
    f << identityCube(33);
  }
  FilterSettings none = defaultFilterSettings();
  FilterSettings common = defaultFilterSettings();
  for (FilterId id : {FilterId::Sharpen, FilterId::Vibrance, FilterId::Contrast, FilterId::ShadowsHighlights})
    common[id].enabled = true;
  FilterSettings full = defaultFilterSettings();
  for (const auto& d : filterRegistry()) full[d.id].enabled = true;
  full.lutPath = toUtf8(lutPath);
  FilterSettings valorant;
  for (const auto& p : builtinPresets())
    if (p.name == "VALORANT Competitive") valorant = p.filters;
  filterPerf("no filters, no blur (pass-through)", none, false);
  filterPerf("VALORANT Competitive preset, no blur", valorant, false);
  filterPerf("common: sharpen+vibrance+contrast+shadows", common, false);
  filterPerf("full stack (all 25 filters), no blur", full, false);
  filterPerf("full stack + motion blur (High, 240 fps)", full, true);
  DeleteFileW(lutPath.c_str());
}
