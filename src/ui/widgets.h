#pragma once
#include <imgui.h>

#include <array>
#include <string>

#include "pipeline/pipeline.h"

namespace bl::ui {

// Rounded card container (auto height). Use with EndCard().
void BeginCard(const char* id, float width = 0.0f, ImU32 bg = 0, float padding = 18.0f);
void EndCard();
void CardHeader(const char* iconGlyph, const char* title, const char* subtitle = nullptr);

bool ToggleSwitch(const char* id, bool* v);
// Label on the left, switch on the right, optional description underneath.
bool ToggleRow(const char* label, const char* description, bool* v);

bool SegmentedControl(const char* id, int* current, const char* const* items, int count, float width = 0.0f);

bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
bool SecondaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
bool DangerButton(const char* label, ImVec2 size = ImVec2(0, 0));
bool IconButton(const char* glyph, const char* tooltip);

void StatusPill(const char* text, ImU32 color);
void StatTile(const char* label, const char* value, const char* unit, ImU32 accent, float width,
              const char* tooltip = nullptr);

// Labelled slider (label + value on one line, slider underneath). Returns true while editing.
bool LabeledSlider(const char* label, float* v, float vmin, float vmax, const char* valueFmt,
                   const char* description = nullptr, float displayScale = 1.0f);
bool LabeledSliderInt(const char* label, int* v, int vmin, int vmax, const char* valueFmt,
                      const char* description = nullptr);

void Sparkline(const char* id, const GraphRing& g, ImVec2 size, ImU32 color, float fixedMax = 0.0f,
               const char* unit = "ms");
void WeightCurve(const std::array<float, 64>& lut, ImVec2 size);

// Hue picker: perceptual (OkLCh) hue gradient track. hue in turns [0, 1).
bool HueSlider(const char* label, float* hue, const char* description = nullptr);

// Collapsible category bar. Returns the open state; sets *resetClicked when its reset button is pressed.
bool CategoryHeader(const char* id, const char* title, const char* subtitle, int activeCount, bool* open,
                    bool* resetClicked);

bool RadioCard(const char* id, const char* glyph, const char* title, const char* description, bool selected,
               float width, const char* badge = nullptr);

void Banner(const char* glyph, ImU32 color, const char* text);
void Hint(const char* text);  // dim wrapped text
void HelpTooltip(const char* text);
void SectionLabel(const char* text);
void DrawLogo(ImVec2 pos, float size);

}  // namespace bl::ui
