// Filters page, presets and live preview (part of bl::ui::App).
#include <windows.h>
#include <commdlg.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include "core/log.h"
#include "core/util.h"
#include "filters/color_math.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/widgets.h"

using Microsoft::WRL::ComPtr;

namespace bl::ui {

namespace {

std::wstring fileDialog(HWND owner, bool save, const wchar_t* filter, const wchar_t* defExt,
                        const std::wstring& defName = {}) {
  wchar_t buf[MAX_PATH] = {};
  if (!defName.empty()) wcsncpy_s(buf, defName.c_str(), _TRUNCATE);
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = defExt;
  ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
  BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
  return ok ? std::wstring(buf) : std::wstring();
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool matches(const FilterDef& d, const std::string& q) {
  if (q.empty()) return true;
  return lower(d.name).find(q) != std::string::npos || lower(d.description).find(q) != std::string::npos ||
         lower(filterCategoryName(d.category)).find(q) != std::string::npos;
}

const char* costLabel(GpuCost c) {
  switch (c) {
    case GpuCost::Minimal: return "minimal cost";
    case GpuCost::Low: return "low cost";
    case GpuCost::Medium: return "extra pass";
  }
  return "";
}

ImU32 hueColor(float h) {
  auto c = color::oklchToSrgb(0.72f, 0.14f, h);
  return IM_COL32(int(c[0] * 255), int(c[1] * 255), int(c[2] * 255), 255);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------------------------

void App::applyPreset(const std::string& name) {
  const Preset* p = m_presets.find(name);
  if (!p) return;
  applyPresetToSettings(*p, m_settings);
  m_presetMessage = "Applied \"" + name + "\"";
  m_presetMessageIsError = false;
}

bool App::presetModified() const {
  const Preset* p = m_presets.find(m_settings.activePreset);
  if (!p) return false;
  if (!(p->filters == m_settings.filters) || p->blurEnabled != m_settings.blurEnabled) return true;
  return p->blurEnabled && (p->blurAmount != m_settings.blurAmount || p->referenceFps != m_settings.referenceFps);
}

void App::drawPresetBar() {
  BeginCard("##presets", 0);
  CardHeader(icon::folder, "Presets", "Save, share and switch complete looks");
  const Preset* active = m_presets.find(m_settings.activePreset);
  const bool modified = presetModified();
  std::string current = active ? active->name + (modified ? "  (modified)" : "") : std::string("No preset");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##presetcombo", current.c_str(), ImGuiComboFlags_HeightLarge)) {
    bool userHeader = false;
    SectionLabel("BUILT-IN");
    for (const auto& p : m_presets.all()) {
      if (!p.builtIn && !userHeader) {
        ImGui::Separator();
        SectionLabel("YOUR PRESETS");
        userHeader = true;
      }
      if (ImGui::Selectable(p.name.c_str(), p.name == m_settings.activePreset)) applyPreset(p.name);
      if (!p.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetNextWindowSize(ImVec2(S(320), 0));
        ImGui::BeginTooltip();
        ImGui::TextWrapped("%s", p.description.c_str());
        ImGui::EndTooltip();
      }
    }
    ImGui::EndCombo();
  }
  if (active && !active->description.empty()) Hint(active->description.c_str());

  float bw = (ImGui::GetContentRegionAvail().x - S(10) * 2) / 3.0f;
  const bool canOverwrite = active && !active->builtIn;
  if (SecondaryButton("Save", ImVec2(bw, 0), canOverwrite && modified)) {
    std::string err;
    Preset p = presetFromSettings(m_settings, active->name);
    p.description = active->description;
    if (m_presets.save(p, &err)) m_presetMessage = "Saved \"" + p.name + "\"";
    else m_presetMessage = err;
    m_presetMessageIsError = !err.empty();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && active && active->builtIn)
    ImGui::SetTooltip("Built-in presets are templates. Use \"Save as\" to keep your changes.");
  ImGui::SameLine();
  if (SecondaryButton("Save as...", ImVec2(bw, 0))) {
    std::string base = active ? active->name + (active->builtIn ? " (custom)" : " copy") : std::string("My preset");
    std::snprintf(m_presetNameBuf, sizeof(m_presetNameBuf), "%s", m_presets.uniqueName(base).c_str());
    ImGui::OpenPopup("Save preset as");
  }
  ImGui::SameLine();
  if (SecondaryButton("Rename...", ImVec2(bw, 0), canOverwrite)) {
    std::snprintf(m_presetNameBuf, sizeof(m_presetNameBuf), "%s", active->name.c_str());
    ImGui::OpenPopup("Rename preset");
  }
  if (SecondaryButton("Duplicate", ImVec2(bw, 0), active != nullptr)) {
    std::string err, name = m_presets.uniqueName(active->name + " copy");
    if (m_presets.duplicate(active->name, name, &err)) {
      m_settings.activePreset = name;
      m_presetMessage = "Created \"" + name + "\"";
    } else {
      m_presetMessage = err;
    }
    m_presetMessageIsError = !err.empty();
  }
  ImGui::SameLine();
  if (SecondaryButton("Import...", ImVec2(bw, 0))) {
    std::wstring path = fileDialog(m_hwnd, false, L"Blur Live preset (*.blpreset)\0*.blpreset\0All files\0*.*\0", L"blpreset");
    if (!path.empty()) {
      std::string err, name;
      if (m_presets.importFile(path, &name, &err)) {
        applyPreset(name);
        m_presetMessage = "Imported \"" + name + "\"";
      } else {
        m_presetMessage = "Import failed: " + err;
      }
      m_presetMessageIsError = !err.empty();
    }
  }
  ImGui::SameLine();
  if (SecondaryButton("Export...", ImVec2(bw, 0), active != nullptr)) {
    // Export what is on screen (including unsaved tweaks) under the preset's name.
    Preset p = presetFromSettings(m_settings, active->name);
    p.description = active->description;
    std::wstring path = fileDialog(m_hwnd, true, L"Blur Live preset (*.blpreset)\0*.blpreset\0", L"blpreset",
                                   toWide(active->name) + L".blpreset");
    if (!path.empty()) {
      std::string text = serializePreset(p);
      FILE* f = nullptr;
      if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        m_presetMessage = "Exported to " + toUtf8(path);
        m_presetMessageIsError = false;
      } else {
        m_presetMessage = "Could not write the file.";
        m_presetMessageIsError = true;
      }
    }
  }
  if (canOverwrite) {
    if (!m_confirmDeletePreset) {
      if (SecondaryButton("Delete...", ImVec2(-1, 0))) m_confirmDeletePreset = true;
    } else {
      float hw = (ImGui::GetContentRegionAvail().x - S(10)) * 0.5f;
      if (DangerButton("Delete preset", ImVec2(hw, 0))) {
        std::string err, name = active->name;
        if (m_presets.remove(name, &err)) {
          m_presetMessage = "Deleted \"" + name + "\"";
          m_settings.activePreset.clear();
        } else {
          m_presetMessage = err;
        }
        m_presetMessageIsError = !err.empty();
        m_confirmDeletePreset = false;
      }
      ImGui::SameLine();
      if (SecondaryButton("Cancel", ImVec2(hw, 0))) m_confirmDeletePreset = false;
    }
  }
  if (!m_presetMessage.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, m_presetMessageIsError ? col::danger : col::success);
    ImGui::PushFont(fonts().caption);
    ImGui::TextWrapped("%s", m_presetMessage.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
  }

  // Name prompts.
  auto namePopup = [&](const char* id, const char* button, auto&& onOk) {
    ImGui::SetNextWindowSize(ImVec2(S(380), 0));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Dummy(ImVec2(S(340), 0));
      ImGui::TextUnformatted("Preset name");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      bool enter = ImGui::InputText("##name", m_presetNameBuf, sizeof(m_presetNameBuf),
                                    ImGuiInputTextFlags_EnterReturnsTrue);
      if (PrimaryButton(button, ImVec2(S(160), 0)) || enter) {
        onOk(std::string(m_presetNameBuf));
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (SecondaryButton("Cancel", ImVec2(S(160), 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  };
  namePopup("Save preset as", "Save", [&](const std::string& name) {
    std::string err;
    Preset p = presetFromSettings(m_settings, name);
    if (m_presets.find(name) && m_presets.find(name)->builtIn) err = "That name belongs to a built-in preset.";
    else if (m_presets.save(p, &err)) m_settings.activePreset = name;
    m_presetMessage = err.empty() ? "Saved \"" + name + "\"" : err;
    m_presetMessageIsError = !err.empty();
  });
  namePopup("Rename preset", "Rename", [&](const std::string& name) {
    std::string err;
    std::string old = m_settings.activePreset;
    if (m_presets.rename(old, name, &err)) m_settings.activePreset = name;
    m_presetMessage = err.empty() ? "Renamed to \"" + name + "\"" : err;
    m_presetMessageIsError = !err.empty();
  });
  EndCard();
}

// ---------------------------------------------------------------------------------------------
// Live preview
// ---------------------------------------------------------------------------------------------

void App::openPreviewTexture() {
  HANDLE h = m_snap.previewHandle;
  if (h == m_previewHandle) return;
  unlockPreview();
  m_previewSrv.Reset();
  m_previewMutex.Reset();
  m_previewTex.Reset();
  m_previewHandle = h;
  if (!h || !m_dev) return;
  if (FAILED(m_dev->OpenSharedResource(h, IID_PPV_ARGS(&m_previewTex)))) {
    logWarn("Live preview unavailable: the shared texture could not be opened on the UI device");
    return;
  }
  m_previewTex.As(&m_previewMutex);
  m_dev->CreateShaderResourceView(m_previewTex.Get(), nullptr, &m_previewSrv);
}

void App::lockPreview() {
  if (m_previewMutex && !m_previewLocked) m_previewLocked = m_previewMutex->AcquireSync(0, 2) == S_OK;
}

void App::unlockPreview() {
  if (m_previewMutex && m_previewLocked) m_previewMutex->ReleaseSync(0);
  m_previewLocked = false;
}

void App::drawPreviewPanel(float width) {
  BeginCard("##preview", width);
  CardHeader(icon::video, "Live preview", m_snap.state == PipelineState::Running ? "Processed output, 30 fps" : nullptr);
  float w = ImGui::GetContentRegionAvail().x;
  float h = w * 9.0f / 16.0f;
  if (m_snap.previewWidth > 0) h = w * float(m_snap.previewHeight) / float(m_snap.previewWidth);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::rgba(14, 12, 20), S(10));
  if (m_snap.state == PipelineState::Running && m_settings.livePreview && m_previewSrv && m_previewLocked) {
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(m_previewSrv.Get()), p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0),
                        ImVec2(1, 1), IM_COL32_WHITE, S(10));
  } else {
    const char* msg = m_snap.state != PipelineState::Running ? "Start capture to see the processed image here"
                      : !m_settings.livePreview              ? "Preview paused"
                                                             : "Waiting for frames...";
    ImGui::PushFont(fonts().caption);
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), col::textFaint, msg);
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(w, h));
  ToggleRow("Compare with original", "Split view: untouched capture on the left, processed on the right. Ctrl+Alt+C.",
            &m_settings.compareEnabled);
  if (m_settings.compareEnabled)
    LabeledSlider("Split position", &m_settings.compareSplit, 0.05f, 0.95f, "%.0f%%", nullptr, 100.0f);
  ToggleRow("Live preview", "Uses a small 640 px copy updated 30 times per second (negligible cost).",
            &m_settings.livePreview);
  char buf[160];
  std::snprintf(buf, sizeof(buf), "GPU per frame: filters %.3f ms  ·  motion blur %.3f ms", m_snap.gpuFilterMs,
                m_snap.gpuMs);
  if (m_snap.state == PipelineState::Running) Hint(buf);
  EndCard();
}

// ---------------------------------------------------------------------------------------------
// Toolbar and filter cards
// ---------------------------------------------------------------------------------------------

void App::drawFilterToolbar() {
  BeginCard("##fxbar", 0);
  float avail = ImGui::GetContentRegionAvail().x;
  ImVec2 start = ImGui::GetCursorPos();
  ImGui::BeginGroup();
  ImGui::PushFont(fonts().hero);
  ImGui::TextUnformatted(m_settings.effectsEnabled ? "Effects on" : "Effects off");
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));
  Hint(m_settings.effectsEnabled ? "Master switch (Ctrl+Alt+F). Off shows the untouched capture."
                                 : "Every effect is bypassed, including motion blur. Your settings are kept.");
  ImGui::EndGroup();
  ImGui::SetCursorPos(ImVec2(start.x + avail - S(44), start.y + S(6)));
  ToggleSwitch("##master", &m_settings.effectsEnabled);
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), start.y + S(52)));

  ImGui::SetNextItemWidth(avail - S(310));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(9)));
  std::string hint = std::string(icon::search) + "  Search filters";
  ImGui::InputTextWithHint("##fxsearch", hint.c_str(), m_filterSearch, sizeof(m_filterSearch));
  ImGui::PopStyleVar();
  ImGui::SameLine();
  if (SecondaryButton("All filters off", ImVec2(S(150), 0))) disableAllFilters(m_settings.filters);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    ImGui::SetTooltip("Turns every filter off but keeps their values (motion blur is controlled separately)");
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6));
  ToggleSwitch("##adv", &m_showAdvanced);
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2));
  ImGui::TextUnformatted("Advanced");
  EndCard();
}

void App::drawMotionBlurFilterCard() {
  ImGui::PushID("motionblur");
  BeginCard("##mb", 0, m_settings.blurEnabled ? col::rgba(30, 26, 44) : col::card, 16);
  float avail = ImGui::GetContentRegionAvail().x;
  ImVec2 start = ImGui::GetCursorPos();
  ToggleSwitch("##on", &m_settings.blurEnabled);
  ImGui::SetCursorPos(ImVec2(start.x + S(56), start.y));
  ImGui::BeginGroup();
  ImGui::PushFont(fonts().semibold);
  ImGui::TextUnformatted("Motion blur");
  ImGui::PopFont();
  ImGui::PushFont(fonts().caption);
  ImGui::PushStyleColor(ImGuiCol_Text, col::textFaint);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));
  ImGui::TextUnformatted("motion-compensated engine  ·  ~0.4 ms at 1080p");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::EndGroup();
  ImGui::SetCursorPos(ImVec2(start.x + avail - S(40), start.y - S(2)));
  if (IconButton(icon::refresh, "Reset motion blur to Blur's defaults")) {
    bool en = m_settings.blurEnabled;
    applyProfile(m_settings, Profile::Cinematic);
    m_settings.blurEnabled = en;
  }
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), start.y + S(40)));
  if (m_settings.blurEnabled) {
    Hint("Blur-style motion blur: motion is estimated on the GPU and the shutter is integrated along it.");
    if (LabeledSlider("Strength", &m_settings.blurAmount, 0.0f, 2.0f, "%.0f%%", nullptr, 100.0f))
      m_settings.profile = Profile::Custom;
    if (SecondaryButton("More motion blur options...")) m_page = Page::Blur;
  }
  EndCard();
  ImGui::PopID();
}

void App::drawFilterCard(const FilterDef& def) {
  FilterState& st = m_settings.filters[def.id];
  const int idx = int(def.id);
  ImGui::PushID(def.key);
  BeginCard("##card", 0, st.enabled ? col::rgba(30, 26, 44) : col::card, 16);
  float avail = ImGui::GetContentRegionAvail().x;
  ImVec2 start = ImGui::GetCursorPos();
  if (ToggleSwitch("##on", &st.enabled) && st.enabled) m_filterExpanded[size_t(idx)] = true;

  // Title (click to expand / collapse without enabling).
  ImGui::SetCursorPos(ImVec2(start.x + S(56), start.y));
  ImVec2 tp = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##title", ImVec2(avail - S(56) - S(90), S(38)));
  if (ImGui::IsItemClicked()) m_filterExpanded[size_t(idx)] = !m_filterExpanded[size_t(idx)];
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(fonts().semibold);
  dl->AddText(tp, st.enabled ? col::text : col::textDim, def.name);
  ImGui::PopFont();
  ImGui::PushFont(fonts().caption);
  std::string sub = std::string(costLabel(def.cost));
  if (st.enabled && !filterAltersImage(m_settings.filters, def.id)) sub += "  ·  at neutral (no change)";
  dl->AddText(ImVec2(tp.x, tp.y + S(20)), col::textFaint, sub.c_str());
  ImGui::PopFont();

  ImGui::SetCursorPos(ImVec2(start.x + avail - S(84), start.y - S(2)));
  if (IconButton(icon::refresh, "Reset this filter to its default values")) resetFilter(m_settings.filters, def.id);
  ImGui::SameLine(0, S(4));
  const bool expanded = st.enabled || m_filterExpanded[size_t(idx)] || m_filterSearch[0] != 0;
  if (IconButton(expanded ? "\xEE\x9C\x8E" : "\xEE\x9C\x8D", expanded ? "Collapse" : "Show settings"))  // E70E / E70D
    m_filterExpanded[size_t(idx)] = !expanded;
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), start.y + S(42)));

  if (expanded) {
    Hint(def.description);
    ImGui::Dummy(ImVec2(0, S(2)));
    if (!st.enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);
    auto& v = st.values;
    auto drawParam = [&](size_t i) {
      const ParamDef& p = def.params[i];
      const char* desc = p.description && *p.description ? p.description : nullptr;
      switch (p.kind) {
        case ParamKind::Slider: LabeledSlider(p.label, &v[i], p.min, p.max, p.format, desc, p.displayScale); break;
        case ParamKind::Hue: HueSlider(p.label, &v[i], desc); break;
        case ParamKind::Choice: {
          ImGui::PushFont(fonts().semibold);
          ImGui::TextUnformatted(p.label);
          ImGui::PopFont();
          int cur = int(std::lround(v[i]));
          if (SegmentedControl(p.key, &cur, p.choices.data(), int(p.choices.size()))) v[i] = float(cur);
          break;
        }
      }
    };

    if (def.id == FilterId::SelectiveColor) {
      drawParam(0);
      SectionLabel("COLOUR RANGE");
      float chip = S(30);
      for (int b = 0; b < kSelectiveBands; ++b) {
        if (b) ImGui::SameLine(0, S(8));
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImGui::PushID(b);
        if (ImGui::InvisibleButton("##band", ImVec2(chip, chip))) m_selectiveBand = b;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", selectiveBandName(b));
        ImGui::PopID();
        ImVec2 c(cp.x + chip * 0.5f, cp.y + chip * 0.5f);
        dl->AddCircleFilled(c, chip * 0.38f, hueColor(selectiveBandHue(b)), 24);
        if (b == m_selectiveBand) dl->AddCircle(c, chip * 0.48f, col::text, 24, S(2));
        bool adjusted = v[size_t(1 + b * 3)] != 0 || v[size_t(2 + b * 3)] != 0 || v[size_t(3 + b * 3)] != 0;
        if (adjusted) dl->AddCircleFilled(ImVec2(cp.x + chip - S(4), cp.y + S(4)), S(3.5f), col::accentHi);
      }
      ImGui::PushFont(fonts().semibold);
      ImGui::Text("%s", selectiveBandName(m_selectiveBand));
      ImGui::PopFont();
      for (int k = 0; k < 3; ++k) {
        size_t i = size_t(1 + m_selectiveBand * 3 + k);
        static const char* labels[3] = {"Hue shift", "Saturation", "Lightness"};
        const ParamDef& p = def.params[i];
        LabeledSlider(labels[k], &v[i], p.min, p.max, p.format, nullptr, p.displayScale);
      }
    } else if (def.id == FilterId::Lut) {
      drawParam(0);
      std::string file = m_settings.filters.lutPath.empty()
                             ? std::string("No LUT selected")
                             : toUtf8(std::filesystem::path(toWide(m_settings.filters.lutPath)).filename().wstring());
      ImGui::PushFont(fonts().semibold);
      ImGui::TextUnformatted(file.c_str());
      ImGui::PopFont();
      if (!m_snap.lutMessage.empty() && !m_settings.filters.lutPath.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, m_snap.lutLoaded ? col::success : col::warning);
        ImGui::PushFont(fonts().caption);
        ImGui::TextWrapped("%s", m_snap.lutMessage.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
      } else if (!m_settings.filters.lutPath.empty() && m_snap.state != PipelineState::Running) {
        Hint("The LUT is loaded on the GPU when capture starts.");
      }
      std::string load = std::string(icon::folder) + "  Load .cube...";
      if (SecondaryButton(load.c_str())) {
        std::wstring path = fileDialog(m_hwnd, false, L"3D LUT (*.cube)\0*.cube\0All files\0*.*\0", L"cube");
        if (!path.empty()) m_settings.filters.lutPath = toUtf8(path);
      }
      ImGui::SameLine();
      if (SecondaryButton("Clear", ImVec2(0, 0), !m_settings.filters.lutPath.empty())) m_settings.filters.lutPath.clear();
    } else {
      if (def.id == FilterId::ColorFocus) {
        SectionLabel("QUICK TARGETS (VALORANT ENEMY HIGHLIGHT COLOURS)");
        struct Q {
          const char* name;
          float hue;
        } qs[] = {{"Red", 0.081f}, {"Yellow", 0.305f}, {"Purple", 0.83f}};
        for (int k = 0; k < 3; ++k) {
          if (k) ImGui::SameLine();
          if (SecondaryButton(qs[k].name)) v[size_t(findParam(def.id, "hue"))] = qs[k].hue;
        }
      }
      for (size_t i = 0; i < def.params.size(); ++i) {
        if (def.params[i].advanced && !m_showAdvanced) continue;
        drawParam(i);
      }
      bool hasAdvanced = std::any_of(def.params.begin(), def.params.end(), [](const ParamDef& p) { return p.advanced; });
      if (hasAdvanced && !m_showAdvanced) Hint("More parameters are available with \"Advanced\" turned on.");
    }
    if (!st.enabled) {
      ImGui::PopStyleVar();
      Hint("This filter is off. Its settings are kept and apply as soon as you turn it on.");
    }
  }
  EndCard();
  ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------------------------

void App::pageFilters() {
  float avail = ImGui::GetContentRegionAvail().x;
  const bool wide = avail > S(1000);
  const float sideW = wide ? std::min(S(440), avail * 0.4f) : avail;
  const float listW = wide ? avail - sideW - S(16) : avail;

  if (!wide) {
    drawPreviewPanel(0);
    drawPresetBar();
  }
  ImGui::BeginChild("##fxlist", ImVec2(listW, 0), 0, ImGuiWindowFlags_NoBackground);
  drawFilterToolbar();
  const std::string q = lower(m_filterSearch);
  for (int c = 0; c < kFilterCategoryCount; ++c) {
    FilterCategory cat = FilterCategory(c);
    int count = 0, active = 0;
    for (const auto& d : filterRegistry())
      if (d.category == cat && matches(d, q)) {
        ++count;
        active += m_settings.filters[d.id].enabled ? 1 : 0;
      }
    if (cat == FilterCategory::Effects) {
      active += m_settings.blurEnabled ? 1 : 0;
      if (q.empty() || std::string("motion blur").find(q) != std::string::npos) ++count;
    }
    if (count == 0) continue;
    bool open = m_categoryOpen[size_t(c)] || !q.empty();
    bool reset = false;
    CategoryHeader(filterCategoryName(cat), filterCategoryName(cat), filterCategoryDescription(cat), active, &open,
                   &reset);
    if (q.empty()) m_categoryOpen[size_t(c)] = open;
    if (reset) resetCategory(m_settings.filters, cat);
    if (!open) continue;
    ImGui::Indent(S(12));
    if (cat == FilterCategory::Effects && (q.empty() || std::string("motion blur").find(q) != std::string::npos))
      drawMotionBlurFilterCard();
    for (const auto& d : filterRegistry())
      if (d.category == cat && matches(d, q)) drawFilterCard(d);
    ImGui::Unindent(S(12));
    ImGui::Dummy(ImVec2(0, S(4)));
  }
  if (!q.empty()) {
    bool any = false;
    for (const auto& d : filterRegistry()) any |= matches(d, q);
    if (!any) Hint("No filter matches your search.");
  }
  ImGui::Dummy(ImVec2(0, S(20)));
  ImGui::EndChild();

  if (wide) {
    ImGui::SameLine(0, S(16));
    ImGui::BeginChild("##fxside", ImVec2(sideW, 0), 0, ImGuiWindowFlags_NoBackground);
    drawPreviewPanel(0);
    drawPresetBar();
    ImGui::EndChild();
  }
}

void App::drawEffectsQuickCard() {
  BeginCard("##fxquick", 0);
  CardHeader(icon::sliders, "Filters & effects",
             (std::to_string(enabledFilterCount(m_settings.filters)) + " filters on").c_str());
  ToggleRow("All effects", "Master switch (Ctrl+Alt+F). Off shows the untouched capture; settings are kept.",
            &m_settings.effectsEnabled);
  ImGui::Dummy(ImVec2(0, S(2)));
  SectionLabel("PRESET");
  const Preset* active = m_presets.find(m_settings.activePreset);
  std::string current = active ? active->name + (presetModified() ? "  (modified)" : "") : std::string("No preset");
  float bw = S(150);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - S(10));
  if (ImGui::BeginCombo("##quickpreset", current.c_str(), ImGuiComboFlags_HeightLarge)) {
    for (const auto& p : m_presets.all())
      if (ImGui::Selectable(p.name.c_str(), p.name == m_settings.activePreset)) applyPreset(p.name);
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  std::string open = std::string(icon::sliders) + "  Open filters";
  if (SecondaryButton(open.c_str(), ImVec2(bw, 0))) m_page = Page::Filters;
  ImGui::Dummy(ImVec2(0, S(2)));
  ToggleRow("Compare with original", "Split view in the output: original left, processed right (Ctrl+Alt+C).",
            &m_settings.compareEnabled);
  EndCard();
}

}  // namespace bl::ui
