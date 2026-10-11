#pragma once
#include <array>
#include <string>
#include <vector>

namespace bl {

// Every real-time filter of the suite. Order = order of the UI lists inside a category.
// Adding a filter: add an id here, a FilterDef in filter_registry.cpp, constant packing in
// FilterChain::packConstants() and its HLSL function in fx_uber_cs.hlsl (or its own pass).
enum class FilterId : int {
  // Detail & clarity
  Sharpen = 0,
  Detail,
  Clarity,
  LocalContrast,
  Denoise,
  // Colour
  Exposure,
  Brightness,
  Contrast,
  Gamma,
  Saturation,
  Vibrance,
  WhiteBalance,
  RgbChannels,
  SelectiveColor,
  // Tone
  ShadowsHighlights,
  Levels,
  LiftGammaGain,
  // Grading
  ColorGrading,
  Lut,
  // Effects
  Bloom,
  Vignette,
  FilmGrain,
  ChromaticAberration,
  // Accessibility
  ColorVision,
  ColorFocus,
  Count
};
constexpr int kFilterCount = int(FilterId::Count);

enum class FilterCategory : int { Detail = 0, Color, Tone, Grading, Effects, Accessibility, Count };
constexpr int kFilterCategoryCount = int(FilterCategory::Count);
const char* filterCategoryName(FilterCategory c);
const char* filterCategoryDescription(FilterCategory c);

enum class ParamKind { Slider, Hue, Choice };

struct ParamDef {
  const char* key;
  const char* label;
  const char* description;
  float min, max;
  float def;       // value when the user first enables / resets the filter
  float neutral;   // value at which the parameter has no effect on the image
  const char* format;  // printf format for the displayed value (applied to value * displayScale)
  float displayScale = 1.0f;
  ParamKind kind = ParamKind::Slider;
  std::vector<const char*> choices = {};
  bool advanced = false;  // hidden behind "Advanced" in the UI
  bool modifier = false;  // only shapes the effect (radius, threshold, ...): never changes the image on its own
};

enum class GpuCost { Minimal, Low, Medium };

struct FilterDef {
  FilterId id;
  const char* key;  // stable identifier used in settings / preset files
  const char* name;
  const char* description;
  FilterCategory category;
  std::vector<ParamDef> params;  // params[0] is the filter's main strength / intensity control
  GpuCost cost = GpuCost::Minimal;
  bool customUi = false;  // the UI draws a dedicated editor (selective colour, LUT, ...)
};

const std::vector<FilterDef>& filterRegistry();
const FilterDef& filterDef(FilterId id);
int findParam(FilterId id, const char* key);  // -1 if missing

// Selective colour has 8 hue bands x 3 parameters after its strength parameter.
constexpr int kSelectiveBands = 8;
const char* selectiveBandName(int band);
float selectiveBandHue(int band);  // 0..1 (OkLCh hue / 2pi)

// ---------------------------------------------------------------------------
// Per-filter state (what presets and settings store).
// ---------------------------------------------------------------------------
struct FilterState {
  bool enabled = false;
  std::vector<float> values;  // one per ParamDef, same order
  bool operator==(const FilterState&) const = default;
};

struct FilterSettings {
  std::array<FilterState, kFilterCount> filters;
  std::string lutPath;  // .cube file used by FilterId::Lut
  bool operator==(const FilterSettings&) const = default;

  FilterState& operator[](FilterId id) { return filters[size_t(id)]; }
  const FilterState& operator[](FilterId id) const { return filters[size_t(id)]; }
  float value(FilterId id, int param) const { return filters[size_t(id)].values[size_t(param)]; }
  float value(FilterId id, const char* key) const;
  void set(FilterId id, const char* key, float v);
};

FilterSettings defaultFilterSettings();          // every filter off, every parameter at its default
void resetFilter(FilterSettings& fs, FilterId id);  // parameters back to defaults (enabled flag untouched)
void neutralizeFilter(FilterSettings& fs, FilterId id);  // parameters to their no-op values
void resetCategory(FilterSettings& fs, FilterCategory c);
void disableAllFilters(FilterSettings& fs);      // turns every filter off, keeps their values
int enabledFilterCount(const FilterSettings& fs);
// True if the filter is enabled and at least one parameter differs from its neutral value.
bool filterAltersImage(const FilterSettings& fs, FilterId id);
void sanitizeFilters(FilterSettings& fs);        // clamp + repair vector sizes

}  // namespace bl
