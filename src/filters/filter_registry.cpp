#include "filters/filter_registry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bl {

const char* filterCategoryName(FilterCategory c) {
  switch (c) {
    case FilterCategory::Detail: return "Detail & clarity";
    case FilterCategory::Color: return "Colour";
    case FilterCategory::Tone: return "Tone";
    case FilterCategory::Grading: return "Grading & LUT";
    case FilterCategory::Effects: return "Effects";
    case FilterCategory::Accessibility: return "Accessibility";
    default: return "?";
  }
}

const char* filterCategoryDescription(FilterCategory c) {
  switch (c) {
    case FilterCategory::Detail: return "Sharpening, texture detail and local contrast";
    case FilterCategory::Color: return "Exposure, saturation, white balance and per-colour adjustments";
    case FilterCategory::Tone: return "Shadows, highlights, levels and lift / gamma / gain";
    case FilterCategory::Grading: return "Creative colour grading and 3D LUTs";
    case FilterCategory::Effects: return "Motion blur, bloom, vignette, grain and lens effects";
    case FilterCategory::Accessibility: return "Colour-vision assistance and colour separation";
    default: return "";
  }
}

namespace {

// Selective-colour band centres (OkLCh hue of the sRGB primaries / secondaries), as a fraction of 360 deg.
constexpr float kBandHueDeg[kSelectiveBands] = {29.2f, 62.0f, 109.8f, 142.5f, 194.8f, 264.1f, 296.0f, 328.4f};
constexpr const char* kBandNames[kSelectiveBands] = {"Reds",  "Oranges", "Yellows", "Greens",
                                                     "Cyans", "Blues",   "Purples", "Magentas"};

ParamDef pct(const char* key, const char* label, const char* desc, float mn, float mx, float def, float neutral,
             bool signedFmt = false) {
  return ParamDef{key, label, desc, mn, mx, def, neutral, signedFmt ? "%+.0f%%" : "%.0f%%", 100.0f};
}
ParamDef num(const char* key, const char* label, const char* desc, float mn, float mx, float def, float neutral,
             const char* fmt, float scale = 1.0f) {
  return ParamDef{key, label, desc, mn, mx, def, neutral, fmt, scale};
}
ParamDef hue(const char* key, const char* label, const char* desc, float def) {
  ParamDef p{key, label, desc, 0.0f, 1.0f, def, def, "%.0f deg", 360.0f};
  p.kind = ParamKind::Hue;
  p.modifier = true;
  return p;
}
ParamDef choice(const char* key, const char* label, const char* desc, std::vector<const char*> items, float def) {
  ParamDef p{key, label, desc, 0.0f, float(items.size() - 1), def, def, "%.0f", 1.0f};
  p.kind = ParamKind::Choice;
  p.choices = std::move(items);
  p.modifier = true;
  return p;
}
ParamDef adv(ParamDef p) {
  p.advanced = true;
  p.modifier = true;
  return p;
}
ParamDef mod(ParamDef p) {
  p.modifier = true;
  return p;
}

std::vector<FilterDef> buildRegistry() {
  std::vector<FilterDef> r;
  using C = FilterCategory;

  // ------------------------------------------------------------------ Detail & clarity
  r.push_back({FilterId::Sharpen, "sharpen", "Sharpen",
               "Contrast-adaptive sharpening (CAS-style) of luminance only: crisper textures, outlines and distant "
               "detail without colour fringes. Overshoot is clamped to the local range to prevent halos.",
               C::Detail,
               {pct("intensity", "Intensity", "Overall sharpening amount.", 0, 1, 0.5f, 0),
                pct("detail", "Fine detail", "Boost for the finest, 1-pixel texture detail.", 0, 1, 0.6f, 0),
                pct("edges", "Edge enhancement", "Crispness of outlines and silhouettes (wider kernel).", 0, 1, 0.3f, 0),
                mod(num("radius", "Radius", "Sampling distance of the sharpening kernel.", 0.5f, 2.0f, 1.0f, 1.0f, "%.2f px")),
                mod(num("threshold", "Noise threshold",
                        "Differences smaller than this are left alone, so noise and grain aren't amplified.", 0, 0.1f,
                        0.012f, 0, "%.1f", 255.0f)),
                adv(pct("halo", "Halo suppression", "How strictly results are clamped to the local min/max.", 0, 1,
                        0.8f, 0.8f))},
               GpuCost::Low});
  r.push_back({FilterId::Detail, "detail", "Detail enhancement",
               "Boosts medium-scale texture (surfaces, foliage, fabric) by amplifying the difference against a "
               "softly blurred copy. Complements Sharpen, which works on the finest pixels.",
               C::Detail,
               {pct("strength", "Strength", "Amount of medium-scale detail added.", 0, 1, 0.35f, 0)},
               GpuCost::Low});
  r.push_back({FilterId::Clarity, "clarity", "Clarity",
               "Midtone local contrast: gives objects more depth and punch without touching deep shadows or "
               "bright highlights.",
               C::Detail,
               {pct("strength", "Strength", "Positive adds punch, negative softens.", -1, 1, 0.3f, 0, true),
                mod(pct("radius", "Radius", "Small = finer structure, large = broader shapes.", 0, 1, 0.4f, 0.4f))},
               GpuCost::Low});
  r.push_back({FilterId::LocalContrast, "local_contrast", "Local contrast",
               "Large-radius contrast enhancement across all tones. Separates objects from flat, hazy backgrounds.",
               C::Detail,
               {pct("strength", "Strength", "Amount of large-scale local contrast.", 0, 1, 0.3f, 0)},
               GpuCost::Low});
  r.push_back({FilterId::Denoise, "denoise", "Denoise",
               "Subtle edge-preserving smoothing (bilateral) for noisy or grainy games. Runs before sharpening.",
               C::Detail,
               {pct("strength", "Strength", "Amount of smoothing.", 0, 1, 0.35f, 0),
                mod(pct("edges", "Edge preservation", "Higher keeps edges and small detail untouched.", 0, 1, 0.7f, 0.7f))},
               GpuCost::Medium});

  // ------------------------------------------------------------------ Colour
  r.push_back({FilterId::Exposure, "exposure", "Exposure",
               "Multiplies scene brightness in linear light, like changing a camera's exposure.",
               C::Color,
               {num("stops", "Exposure", "In photographic stops (each stop doubles the light).", -2, 2, 0.3f, 0,
                    "%+.2f EV"),
                mod(pct("protect", "Highlight protection", "Rolls off highlights instead of clipping them.", 0, 1, 0.6f,
                        0.6f))}});
  r.push_back({FilterId::Brightness, "brightness", "Brightness",
               "Brightens or darkens the image without clipping: it compresses towards white or black instead.",
               C::Color, {pct("amount", "Brightness", "", -1, 1, 0.1f, 0, true)}});
  r.push_back({FilterId::Contrast, "contrast", "Contrast",
               "S-curve around mid-grey applied to luminance (hue and saturation are kept). Never clips.", C::Color,
               {pct("amount", "Contrast", "", -1, 1, 0.15f, 0, true)}});
  r.push_back({FilterId::Gamma, "gamma", "Gamma",
               "Midtone brightness. Values above 1 brighten midtones and shadows, below 1 darken them; black and "
               "white stay fixed.",
               C::Color, {num("gamma", "Gamma", "", 0.5f, 2.0f, 1.1f, 1.0f, "%.2f")}});
  r.push_back({FilterId::Saturation, "saturation", "Saturation",
               "Scales colourfulness evenly (perceptual OkLCh chroma, luminance and hue preserved). Off by default.",
               C::Color, {pct("amount", "Saturation", "", -1, 1, 0.2f, 0, true)}});
  r.push_back({FilterId::Vibrance, "vibrance", "Vibrance",
               "Smart saturation: boosts muted colours much more than already-vivid ones, keeping skin and bright "
               "colours natural.",
               C::Color, {pct("amount", "Vibrance", "", -1, 1, 0.25f, 0, true)}});
  r.push_back({FilterId::WhiteBalance, "white_balance", "Temperature & tint",
               "White balance via chromatic adaptation along the black-body locus. Brightness is preserved.",
               C::Color,
               {pct("temperature", "Temperature", "Positive = warmer (orange), negative = cooler (blue).", -1, 1,
                    0.1f, 0, true),
                pct("tint", "Tint", "Positive = magenta, negative = green.", -1, 1, 0, 0, true)}});
  r.push_back({FilterId::RgbChannels, "rgb", "RGB channels",
               "Individual red, green and blue gains in linear light.", C::Color,
               {num("red", "Red", "", 0.5f, 1.5f, 1.0f, 1.0f, "%.2fx"),
                num("green", "Green", "", 0.5f, 1.5f, 1.0f, 1.0f, "%.2fx"),
                num("blue", "Blue", "", 0.5f, 1.5f, 1.0f, 1.0f, "%.2fx")}});
  {
    FilterDef sel{FilterId::SelectiveColor, "selective", "Selective colour",
                  "Adjust hue, saturation and lightness of one colour range at a time (reds, greens, blues, ...) "
                  "without affecting the rest. Smooth band transitions in OkLCh.",
                  C::Color,
                  {pct("strength", "Strength", "Overall amount of all band adjustments.", 0, 1, 1.0f, 1.0f)},
                  GpuCost::Minimal,
                  true};
    static std::string keys[kSelectiveBands * 3];
    static std::string labels[kSelectiveBands * 3];
    const char* comps[3] = {"hue", "sat", "light"};
    const char* compLabels[3] = {"Hue shift", "Saturation", "Lightness"};
    for (int b = 0; b < kSelectiveBands; ++b)
      for (int c = 0; c < 3; ++c) {
        int i = b * 3 + c;
        keys[i] = std::string(kBandNames[b]) + "_" + comps[c];
        for (auto& ch : keys[i]) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        labels[i] = std::string(kBandNames[b]) + " " + compLabels[c];
        sel.params.push_back(c == 0 ? num(keys[i].c_str(), labels[i].c_str(), "", -1, 1, 0, 0, "%+.0f deg", 30.0f)
                                    : pct(keys[i].c_str(), labels[i].c_str(), "", -1, 1, 0, 0, true));
      }
    r.push_back(sel);
  }

  // ------------------------------------------------------------------ Tone
  r.push_back({FilterId::ShadowsHighlights, "shadows_highlights", "Shadows & highlights",
               "Recovers detail in dark and bright areas using a smooth local brightness mask, so dark corners get "
               "easier to read without washing out the whole image.",
               C::Tone,
               {pct("shadows", "Shadows", "Positive lifts shadows, negative deepens them.", -1, 1, 0.3f, 0, true),
                pct("highlights", "Highlights", "Negative recovers bright areas, positive brightens them.", -1, 1,
                    -0.15f, 0, true),
                mod(pct("local", "Locality", "0 = per pixel, higher = based on the surrounding area (more natural).",
                        0, 1, 0.6f, 0.6f))},
               GpuCost::Low});
  r.push_back({FilterId::Levels, "levels", "Levels",
               "Input black / white points stretch the tonal range; output points limit it.", C::Tone,
               {num("in_black", "Black point", "Input values at or below this become black.", 0, 0.5f, 0.02f, 0,
                    "%.0f", 255),
                num("in_white", "White point", "Input values at or above this become white.", 0.5f, 1, 0.98f, 1,
                    "%.0f", 255),
                num("out_black", "Output black", "", 0, 0.5f, 0, 0, "%.0f", 255),
                num("out_white", "Output white", "", 0.5f, 1, 1, 1, "%.0f", 255)}});
  r.push_back({FilterId::LiftGammaGain, "lgg", "Lift / gamma / gain",
               "Classic three-way colour corrector: lift moves the shadows, gamma the midtones, gain the highlights. "
               "Each can be tinted towards a hue.",
               C::Tone,
               {num("lift", "Lift", "", -0.25f, 0.25f, 0.02f, 0, "%+.3f"),
                num("gamma", "Gamma", "", 0.5f, 2.0f, 1.0f, 1.0f, "%.2f"),
                num("gain", "Gain", "", 0.5f, 1.5f, 1.0f, 1.0f, "%.2f"),
                adv(hue("lift_hue", "Lift tint hue", "", 0.6f)),
                adv(pct("lift_tint", "Lift tint", "", 0, 1, 0, 0)),
                adv(hue("gamma_hue", "Gamma tint hue", "", 0.1f)),
                adv(pct("gamma_tint", "Gamma tint", "", 0, 1, 0, 0)),
                adv(hue("gain_hue", "Gain tint hue", "", 0.15f)),
                adv(pct("gain_tint", "Gain tint", "", 0, 1, 0, 0))}});

  // ------------------------------------------------------------------ Grading
  r.push_back({FilterId::ColorGrading, "grading", "Colour grading",
               "Split toning plus an ambient tint: colour the shadows and highlights separately (e.g. teal shadows, "
               "warm highlights). Luminance is preserved.",
               C::Grading,
               {pct("amount", "Amount", "Overall strength of the grade.", 0, 1, 1.0f, 0),
                hue("shadow_hue", "Shadow hue", "", 0.58f),
                pct("shadow_amount", "Shadow tint", "", 0, 1, 0.25f, 0),
                hue("highlight_hue", "Highlight hue", "", 0.17f),
                pct("highlight_amount", "Highlight tint", "", 0, 1, 0.2f, 0),
                pct("balance", "Balance", "Shifts the split towards shadows (-) or highlights (+).", -1, 1, 0, 0, true),
                hue("ambient_hue", "Ambient hue", "Overall colour wash for the whole image.", 0.6f),
                pct("ambient_amount", "Ambient tint", "", 0, 1, 0, 0)}});
  r.push_back({FilterId::Lut, "lut", "3D LUT",
               "Applies a .cube 3D look-up table (from DaVinci Resolve, Photoshop, ReShade packs, ...) with "
               "trilinear interpolation.",
               C::Grading, {pct("strength", "Strength", "", 0, 1, 1.0f, 0)}, GpuCost::Minimal, true});

  // ------------------------------------------------------------------ Effects
  r.push_back({FilterId::Bloom, "bloom", "Bloom",
               "Soft glow around bright light sources, computed in linear light on a small mip chain.", C::Effects,
               {pct("intensity", "Intensity", "", 0, 1, 0.25f, 0),
                mod(pct("threshold", "Threshold", "Only areas brighter than this glow.", 0, 1, 0.75f, 0.75f)),
                mod(pct("radius", "Radius", "Size of the glow.", 0, 1, 0.6f, 0.6f))},
               GpuCost::Medium});
  r.push_back({FilterId::Vignette, "vignette", "Vignette",
               "Darkens the corners to draw the eye to the centre of the screen.", C::Effects,
               {pct("amount", "Amount", "", 0, 1, 0.3f, 0), mod(pct("size", "Size", "", 0, 1, 0.6f, 0.6f)),
                mod(pct("softness", "Softness", "", 0, 1, 0.5f, 0.5f))}});
  r.push_back({FilterId::FilmGrain, "grain", "Film grain",
               "Subtle animated luminance grain, strongest in the midtones. Also hides banding.", C::Effects,
               {pct("amount", "Amount", "", 0, 1, 0.15f, 0), mod(num("size", "Grain size", "", 1, 3, 1.3f, 1.3f, "%.1f px"))}});
  r.push_back({FilterId::ChromaticAberration, "chromatic", "Chromatic aberration",
               "Lens-style colour fringing that grows towards the screen edges.", C::Effects,
               {pct("amount", "Amount", "", 0, 1, 0.2f, 0)}});

  // ------------------------------------------------------------------ Accessibility
  r.push_back({FilterId::ColorVision, "color_vision", "Colour-vision assistance",
               "Daltonization: information lost to a colour-vision deficiency is moved into colours you can "
               "distinguish (Machado 2009 simulation). 'Simulate' shows how the image looks with the deficiency.",
               C::Accessibility,
               {pct("strength", "Strength", "", 0, 1, 0.7f, 0),
                choice("type", "Type", "", {"Protan (red)", "Deutan (green)", "Tritan (blue)"}, 1),
                mod(pct("separation", "Colour separation", "How strongly confusable colours are pushed apart.", 0, 1,
                        0.5f, 0.5f)),
                choice("mode", "Mode", "", {"Correct", "Simulate"}, 0)}});
  r.push_back({FilterId::ColorFocus, "color_focus", "Colour focus",
               "Makes one colour range stand out (e.g. an enemy highlight colour) by boosting it and optionally "
               "muting everything else. Other hues are left alone unless you mute them.",
               C::Accessibility,
               {pct("boost", "Boost", "Extra saturation and brightness for the target colour.", 0, 1, 0.5f, 0),
                hue("hue", "Target hue", "", 0.081f),
                mod(pct("width", "Hue range", "How wide the selected colour range is.", 0, 1, 0.3f, 0.3f)),
                pct("mute", "Mute other colours", "Desaturates everything outside the range.", 0, 1, 0, 0)},
               GpuCost::Minimal,
               true});
  return r;
}

}  // namespace

const std::vector<FilterDef>& filterRegistry() {
  static const std::vector<FilterDef> reg = [] {
    auto r = buildRegistry();
    for (int i = 0; i < int(r.size()); ++i)
      if (int(r[size_t(i)].id) != i) std::abort();  // registry order must match FilterId
    return r;
  }();
  return reg;
}

const FilterDef& filterDef(FilterId id) { return filterRegistry()[size_t(id)]; }

int findParam(FilterId id, const char* key) {
  const auto& p = filterDef(id).params;
  for (size_t i = 0; i < p.size(); ++i)
    if (std::strcmp(p[i].key, key) == 0) return int(i);
  return -1;
}

const char* selectiveBandName(int band) { return kBandNames[std::clamp(band, 0, kSelectiveBands - 1)]; }
float selectiveBandHue(int band) { return kBandHueDeg[std::clamp(band, 0, kSelectiveBands - 1)] / 360.0f; }

float FilterSettings::value(FilterId id, const char* key) const {
  int i = findParam(id, key);
  return i < 0 ? 0.0f : value(id, i);
}

void FilterSettings::set(FilterId id, const char* key, float v) {
  int i = findParam(id, key);
  if (i >= 0) filters[size_t(id)].values[size_t(i)] = v;
}

FilterSettings defaultFilterSettings() {
  FilterSettings fs;
  for (const auto& d : filterRegistry()) {
    FilterState& s = fs.filters[size_t(d.id)];
    s.enabled = false;
    s.values.clear();
    for (const auto& p : d.params) s.values.push_back(p.def);
  }
  return fs;
}

void resetFilter(FilterSettings& fs, FilterId id) {
  const auto& d = filterDef(id);
  auto& v = fs.filters[size_t(id)].values;
  v.resize(d.params.size());
  for (size_t i = 0; i < d.params.size(); ++i) v[i] = d.params[i].def;
}

void neutralizeFilter(FilterSettings& fs, FilterId id) {
  const auto& d = filterDef(id);
  auto& v = fs.filters[size_t(id)].values;
  v.resize(d.params.size());
  for (size_t i = 0; i < d.params.size(); ++i) v[i] = d.params[i].neutral;
}

void resetCategory(FilterSettings& fs, FilterCategory c) {
  for (const auto& d : filterRegistry())
    if (d.category == c) {
      resetFilter(fs, d.id);
      fs.filters[size_t(d.id)].enabled = false;
    }
}

void disableAllFilters(FilterSettings& fs) {
  for (auto& f : fs.filters) f.enabled = false;
}

int enabledFilterCount(const FilterSettings& fs) {
  int n = 0;
  for (const auto& f : fs.filters) n += f.enabled ? 1 : 0;
  return n;
}

bool filterAltersImage(const FilterSettings& fs, FilterId id) {
  const FilterState& s = fs[id];
  if (!s.enabled) return false;
  if (id == FilterId::Lut && fs.lutPath.empty()) return false;
  const auto& d = filterDef(id);
  const auto& v = s.values;
  if (id == FilterId::SelectiveColor) {  // strength (neutral 1) only matters if some band is adjusted
    if (v[0] == 0.0f) return false;
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i] != 0.0f) return true;
    return false;
  }
  // Filters whose first parameter is a strength that gates everything else.
  const bool gated = id == FilterId::Sharpen || id == FilterId::Denoise || id == FilterId::ColorGrading ||
                     id == FilterId::Lut || id == FilterId::Bloom || id == FilterId::ColorVision;
  if (gated && v[0] == d.params[0].neutral) return false;
  for (size_t i = 0; i < d.params.size(); ++i)
    if (d.params[i].kind == ParamKind::Slider && !d.params[i].modifier && v[i] != d.params[i].neutral) return true;
  return false;
}
void sanitizeFilters(FilterSettings& fs) {
  for (const auto& d : filterRegistry()) {
    auto& v = fs.filters[size_t(d.id)].values;
    if (v.size() != d.params.size()) {
      size_t old = v.size();
      v.resize(d.params.size());
      for (size_t i = old; i < d.params.size(); ++i) v[i] = d.params[i].def;
    }
    for (size_t i = 0; i < d.params.size(); ++i) {
      float x = v[i];
      if (!std::isfinite(x)) x = d.params[i].def;
      if (d.params[i].kind == ParamKind::Choice) x = std::round(x);
      v[i] = std::clamp(x, d.params[i].min, d.params[i].max);
    }
  }
}

}  // namespace bl
