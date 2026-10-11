#include "ui/widgets.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "filters/color_math.h"
#include "ui/theme.h"

namespace bl::ui {

namespace {
ImU32 withAlpha(ImU32 c, float a) {
  ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
  v.w *= a;
  return ImGui::ColorConvertFloat4ToU32(v);
}
ImU32 lerpCol(ImU32 a, ImU32 b, float t) {
  ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                               x.w + (y.w - x.w) * t));
}
// Smooth per-widget animation value (0..1) keyed by ImGuiID.
float animate(ImGuiID id, float target, float speed = 14.0f) {
  ImGuiStorage* st = ImGui::GetStateStorage();
  float* v = st->GetFloatRef(id ^ 0x5A17u, target);
  float dt = ImGui::GetIO().DeltaTime;
  *v += (target - *v) * std::min(1.0f, dt * speed);
  if (std::abs(*v - target) < 0.001f) *v = target;
  return *v;
}
}  // namespace

void BeginCard(const char* id, float width, ImU32 bg, float padding) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, bg ? bg : col::card);
  ImGui::PushStyleColor(ImGuiCol_Border, col::border);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(padding), S(padding)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14));
  ImGui::BeginChild(id, ImVec2(width, 0),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void EndCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

void CardHeader(const char* iconGlyph, const char* title, const char* subtitle) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float box = S(32);
  if (iconGlyph) {
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), withAlpha(col::accent, 0.18f), S(9));
    ImGui::PushFont(fonts().icon);
    ImVec2 ts = ImGui::CalcTextSize(iconGlyph);
    dl->AddText(ImVec2(p.x + (box - ts.x) * 0.5f, p.y + (box - ts.y) * 0.5f), col::accentHi, iconGlyph);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + box + S(12), p.y + (subtitle ? -S(1) : S(5))));
  }
  ImGui::BeginGroup();
  ImGui::PushFont(fonts().hero);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (subtitle) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(8));
    ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
    ImGui::PushFont(fonts().caption);
    ImGui::TextUnformatted(subtitle);
    ImGui::PopFont();
    ImGui::PopStyleColor();
  }
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(ImGui::GetCursorScreenPos().y, p.y + box) + S(10)));
}

bool ToggleSwitch(const char* id, bool* v) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return false;
  const ImGuiID gid = w->GetID(id);
  float h = S(24), wd = S(44);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImRect bb(p, ImVec2(p.x + wd, p.y + h));
  ImGui::ItemSize(bb);
  if (!ImGui::ItemAdd(bb, gid)) return false;
  bool hovered, held;
  bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
  if (pressed) {
    *v = !*v;
    ImGui::MarkItemEdited(gid);
  }
  float t = animate(gid, *v ? 1.0f : 0.0f);
  ImDrawList* dl = w->DrawList;
  ImU32 off = hovered ? col::rgba(70, 62, 96) : col::rgba(56, 49, 80);
  dl->AddRectFilled(bb.Min, bb.Max, lerpCol(off, col::accent, t), h * 0.5f);
  float r = h * 0.5f - S(3);
  ImVec2 c(bb.Min.x + h * 0.5f + t * (wd - h), bb.Min.y + h * 0.5f);
  dl->AddCircleFilled(c, r, col::rgba(255, 255, 255), 24);
  return pressed;
}

bool ToggleRow(const char* label, const char* description, bool* v) {
  ImGui::PushID(label);
  float avail = ImGui::GetContentRegionAvail().x;
  ImVec2 start = ImGui::GetCursorPos();
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(start.x + avail - S(60));
  ImGui::PushFont(fonts().semibold);
  ImGui::TextUnformatted(label);
  ImGui::PopFont();
  if (description) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));
    ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
    ImGui::PushFont(fonts().caption);
    ImGui::TextWrapped("%s", description);
    ImGui::PopFont();
    ImGui::PopStyleColor();
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  float endY = ImGui::GetCursorPosY();
  ImGui::SetCursorPos(ImVec2(start.x + avail - S(44), start.y + S(2)));
  bool changed = ToggleSwitch("##sw", v);
  ImGui::SetCursorPosY(std::max(endY, ImGui::GetCursorPosY()));
  ImGui::PopID();
  return changed;
}

bool SegmentedControl(const char* id, int* current, const char* const* items, int count, float width) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return false;
  ImGui::PushID(id);
  if (width <= 0) width = ImGui::GetContentRegionAvail().x;
  float h = S(36);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImRect bb(p, ImVec2(p.x + width, p.y + h));
  ImGui::ItemSize(bb);
  ImDrawList* dl = w->DrawList;
  dl->AddRectFilled(bb.Min, bb.Max, col::rgba(30, 26, 44), S(10));
  float segW = (width - S(8)) / float(count);
  const ImGuiID animId = w->GetID("anim");
  float t = animate(animId, float(*current), 18.0f);
  ImVec2 selMin(bb.Min.x + S(4) + t * segW, bb.Min.y + S(4));
  dl->AddRectFilled(selMin, ImVec2(selMin.x + segW, bb.Max.y - S(4)), col::accent, S(8));
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    ImRect sb(ImVec2(bb.Min.x + S(4) + i * segW, bb.Min.y), ImVec2(bb.Min.x + S(4) + (i + 1) * segW, bb.Max.y));
    ImGuiID sid = w->GetID(i);
    if (!ImGui::ItemAdd(sb, sid)) continue;
    bool hovered, held;
    if (ImGui::ButtonBehavior(sb, sid, &hovered, &held) && *current != i) {
      *current = i;
      changed = true;
    }
    ImU32 tc = i == *current ? col::rgba(255, 255, 255) : hovered ? col::text : col::textDim;
    ImGui::PushFont(fonts().semibold);
    ImVec2 ts = ImGui::CalcTextSize(items[i]);
    dl->AddText(ImVec2(sb.Min.x + (sb.GetWidth() - ts.x) * 0.5f, sb.Min.y + (h - ts.y) * 0.5f), tc, items[i]);
    ImGui::PopFont();
  }
  ImGui::PopID();
  return changed;
}

static bool styledButton(const char* label, ImVec2 size, ImU32 base, ImU32 hover, ImU32 active, ImU32 text,
                         bool enabled) {
  ImGui::PushStyleColor(ImGuiCol_Button, base);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
  ImGui::PushStyleColor(ImGuiCol_Text, text);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(10));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(18), S(10)));
  ImGui::PushFont(fonts().semibold);
  if (!enabled) ImGui::BeginDisabled();
  bool r = ImGui::Button(label, size);
  if (!enabled) ImGui::EndDisabled();
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(4);
  return r;
}

bool PrimaryButton(const char* label, ImVec2 size, bool enabled) {
  return styledButton(label, size, col::accent, col::accentHi, col::accentLo, col::rgba(255, 255, 255), enabled);
}
bool SecondaryButton(const char* label, ImVec2 size, bool enabled) {
  return styledButton(label, size, col::rgba(40, 35, 58), col::rgba(54, 47, 78), col::rgba(66, 57, 96), col::text,
                      enabled);
}
bool DangerButton(const char* label, ImVec2 size) {
  return styledButton(label, size, col::rgba(190, 60, 70), col::rgba(220, 80, 90), col::rgba(160, 45, 55),
                      col::rgba(255, 255, 255), true);
}

bool IconButton(const char* glyph, const char* tooltip) {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(8)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(10));
  bool r = ImGui::Button(glyph);
  ImGui::PopStyleVar(2);
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
  return r;
}

void StatusPill(const char* text, ImU32 color) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(fonts().semibold);
  ImVec2 ts = ImGui::CalcTextSize(text);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = S(30), padX = S(12), dot = S(4);
  ImVec2 size(ts.x + padX * 2 + dot * 2 + S(8), h);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + h), withAlpha(color, 0.16f), h * 0.5f);
  float pulse = 0.6f + 0.4f * std::sin(float(ImGui::GetTime()) * 4.0f);
  ImVec2 dc(p.x + padX + dot, p.y + h * 0.5f);
  dl->AddCircleFilled(dc, dot + S(3), withAlpha(color, 0.25f * pulse));
  dl->AddCircleFilled(dc, dot, color);
  dl->AddText(ImVec2(p.x + padX + dot * 2 + S(8), p.y + (h - ts.y) * 0.5f), color, text);
  ImGui::PopFont();
  ImGui::Dummy(size);
}

void StatTile(const char* label, const char* value, const char* unit, ImU32 accent, float width, const char* tooltip) {
  ImGui::PushID(label);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = S(86);
  dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), col::rgba(30, 26, 44), S(12));
  dl->AddRectFilled(ImVec2(p.x, p.y + S(14)), ImVec2(p.x + S(3), p.y + h - S(14)), accent, S(2));
  ImGui::PushFont(fonts().caption);
  dl->AddText(ImVec2(p.x + S(16), p.y + S(12)), col::textDim, label);
  ImGui::PopFont();
  ImGui::PushFont(fonts().big);
  ImVec2 vs = ImGui::CalcTextSize(value);
  dl->AddText(ImVec2(p.x + S(16), p.y + S(36)), col::text, value);
  ImGui::PopFont();
  if (unit && *unit) {
    ImGui::PushFont(fonts().regular);
    ImVec2 us = ImGui::CalcTextSize(unit);
    dl->AddText(ImVec2(p.x + S(20) + vs.x, p.y + S(36) + vs.y - us.y - S(4)), col::textFaint, unit);
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(width, h));
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetNextWindowSize(ImVec2(S(320), 0));
    ImGui::BeginTooltip();
    ImGui::TextWrapped("%s", tooltip);
    ImGui::EndTooltip();
  }
  ImGui::PopID();
}

bool LabeledSlider(const char* label, float* v, float vmin, float vmax, const char* valueFmt, const char* description,
                   float displayScale) {
  ImGui::PushID(label);
  float avail = ImGui::GetContentRegionAvail().x;
  ImGui::PushFont(fonts().semibold);
  ImGui::TextUnformatted(label);
  ImGui::PopFont();
  char buf[64];
  std::snprintf(buf, sizeof(buf), valueFmt, double(*v * displayScale));
  ImGui::SameLine(avail - ImGui::CalcTextSize(buf).x);
  ImGui::PushStyleColor(ImGuiCol_Text, col::accentHi);
  ImGui::TextUnformatted(buf);
  ImGui::PopStyleColor();
  if (description) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));
    ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
    ImGui::PushFont(fonts().caption);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail);
    ImGui::TextWrapped("%s", description);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::PopStyleColor();
  }
  // Custom-drawn track with a filled portion.
  ImGui::SetNextItemWidth(avail);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, S(18));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, S(9));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, col::rgba(255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, col::accentHi);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetFrameHeight();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float t = std::clamp((*v - vmin) / (vmax - vmin), 0.0f, 1.0f);
  float trackH = S(6);
  float cy = p.y + h * 0.5f;
  float grab = S(18);
  float x0 = p.x + grab * 0.5f, x1 = p.x + avail - grab * 0.5f;
  dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x1, cy + trackH * 0.5f), col::rgba(48, 42, 68), trackH);
  dl->AddRectFilledMultiColor(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x0 + (x1 - x0) * t, cy + trackH * 0.5f),
                              col::accentLo, col::accentHi, col::accentHi, col::accentLo);
  bool changed = ImGui::SliderFloat("##s", v, vmin, vmax, "", ImGuiSliderFlags_AlwaysClamp);
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(3);
  ImGui::PopID();
  return changed;
}

bool LabeledSliderInt(const char* label, int* v, int vmin, int vmax, const char* valueFmt, const char* description) {
  float f = float(*v);
  LabeledSlider(label, &f, float(vmin), float(vmax), valueFmt, description);
  const int nv = int(std::lround(f));
  const bool changed = nv != *v;
  *v = nv;
  return changed;
}

void Sparkline(const char* id, const GraphRing& g, ImVec2 size, ImU32 color, float fixedMax, const char* unit) {
  ImGui::PushID(id);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::rgba(20, 18, 30), S(10));
  float mx = fixedMax;
  float avg = 0;
  if (g.count > 0) {
    float m = 0;
    for (int i = 0; i < g.count; ++i) {
      m = std::max(m, g.at(i));
      avg += g.at(i);
    }
    avg /= float(g.count);
    if (mx <= 0) mx = std::max(0.1f, m * 1.2f);
  }
  if (mx <= 0) mx = 1;
  // grid lines
  for (int k = 1; k < 4; ++k) {
    float y = p.y + size.y * float(k) / 4.0f;
    dl->AddLine(ImVec2(p.x + S(8), y), ImVec2(p.x + size.x - S(8), y), col::rgba(255, 255, 255, 10));
  }
  if (g.count >= 2) {
    const int n = g.count;
    float pad = S(6);
    auto pt = [&](int i) {
      float x = p.x + pad + (size.x - pad * 2) * float(i) / float(GraphRing::kSize - 1) +
                (size.x - pad * 2) * float(GraphRing::kSize - n) / float(GraphRing::kSize - 1);
      float y = p.y + size.y - pad - (size.y - pad * 2) * std::clamp(g.at(i) / mx, 0.0f, 1.0f);
      return ImVec2(x, y);
    };
    for (int i = 1; i < n; ++i) {
      ImVec2 a = pt(i - 1), b = pt(i);
      dl->AddQuadFilled(a, b, ImVec2(b.x, p.y + size.y - pad), ImVec2(a.x, p.y + size.y - pad), withAlpha(color, 0.12f));
    }
    for (int i = 1; i < n; ++i) dl->AddLine(pt(i - 1), pt(i), color, S(1.6f));
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "avg %.2f %s   max scale %.1f", double(avg), unit, double(mx));
  ImGui::PushFont(fonts().caption);
  dl->AddText(ImVec2(p.x + S(10), p.y + S(6)), col::textFaint, buf);
  ImGui::PopFont();
  ImGui::Dummy(size);
  ImGui::PopID();
}

void WeightCurve(const std::array<float, 64>& lut, ImVec2 size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::rgba(20, 18, 30), S(10));
  float mx = 0;
  for (float v : lut) mx = std::max(mx, v);
  mx = std::max(mx * 1.15f, 1.2f);
  float pad = S(10);
  auto pt = [&](int i) {
    return ImVec2(p.x + pad + (size.x - pad * 2) * float(i) / 63.0f,
                  p.y + size.y - pad - S(14) - (size.y - pad * 2 - S(14)) * (lut[size_t(i)] / mx));
  };
  float base = p.y + size.y - pad - S(14);
  // Area fill with a vertical fade: one gradient quad per segment (top = curve, bottom = baseline).
  const ImU32 top = withAlpha(col::accent, 0.45f), bottom = withAlpha(col::accent, 0.04f);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  for (int i = 1; i < 64; ++i) {
    ImVec2 a = pt(i - 1), b = pt(i);
    dl->PrimReserve(6, 4);
    ImDrawIdx idx = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(a, uv, top);
    dl->PrimWriteVtx(b, uv, top);
    dl->PrimWriteVtx(ImVec2(b.x, base), uv, bottom);
    dl->PrimWriteVtx(ImVec2(a.x, base), uv, bottom);
    dl->PrimWriteIdx(idx); dl->PrimWriteIdx(ImDrawIdx(idx + 1)); dl->PrimWriteIdx(ImDrawIdx(idx + 2));
    dl->PrimWriteIdx(idx); dl->PrimWriteIdx(ImDrawIdx(idx + 2)); dl->PrimWriteIdx(ImDrawIdx(idx + 3));
  }
  for (int i = 1; i < 64; ++i) dl->AddLine(pt(i - 1), pt(i), col::accentHi, S(2));
  ImGui::PushFont(fonts().caption);
  dl->AddText(ImVec2(p.x + pad, base + S(1)), col::textFaint, "oldest");
  ImVec2 ns = ImGui::CalcTextSize("now");
  dl->AddText(ImVec2(p.x + size.x - pad - ns.x, base + S(1)), col::textFaint, "now");
  ImGui::PopFont();
  ImGui::Dummy(size);
}

bool HueSlider(const char* label, float* hue, const char* description) {
  ImGui::PushID(label);
  float avail = ImGui::GetContentRegionAvail().x;
  ImGui::PushFont(fonts().semibold);
  ImGui::TextUnformatted(label);
  ImGui::PopFont();
  auto swatch = color::oklchToSrgb(0.72f, 0.14f, *hue);
  ImU32 sc = IM_COL32(int(swatch[0] * 255), int(swatch[1] * 255), int(swatch[2] * 255), 255);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f deg", double(*hue * 360.0f));
  float tw = ImGui::CalcTextSize(buf).x;
  ImGui::SameLine(avail - tw - S(22));
  ImVec2 sp = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(sp.x + S(7), sp.y + ImGui::GetTextLineHeight() * 0.5f), S(6), sc);
  ImGui::SetCursorScreenPos(ImVec2(sp.x + S(20), sp.y));
  ImGui::PushStyleColor(ImGuiCol_Text, col::accentHi);
  ImGui::TextUnformatted(buf);
  ImGui::PopStyleColor();
  if (description && *description) Hint(description);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = S(26), grab = S(18);
  float x0 = p.x + grab * 0.5f, x1 = p.x + avail - grab * 0.5f, cy = p.y + h * 0.5f, th = S(8);
  const int segs = 36;
  for (int i = 0; i < segs; ++i) {
    auto a = color::oklchToSrgb(0.72f, 0.14f, float(i) / segs), b = color::oklchToSrgb(0.72f, 0.14f, float(i + 1) / segs);
    ImU32 ca = IM_COL32(int(a[0] * 255), int(a[1] * 255), int(a[2] * 255), 255);
    ImU32 cb = IM_COL32(int(b[0] * 255), int(b[1] * 255), int(b[2] * 255), 255);
    float xa = x0 + (x1 - x0) * float(i) / segs, xb = x0 + (x1 - x0) * float(i + 1) / segs;
    dl->AddRectFilledMultiColor(ImVec2(xa, cy - th * 0.5f), ImVec2(xb + 0.5f, cy + th * 0.5f), ca, cb, cb, ca);
  }
  ImGui::SetNextItemWidth(avail);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, grab);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, grab * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, sc);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, sc);
  bool changed = ImGui::SliderFloat("##hue", hue, 0.0f, 1.0f, "", ImGuiSliderFlags_AlwaysClamp);
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(3);
  ImGui::PopID();
  return changed;
}

bool CategoryHeader(const char* id, const char* title, const char* subtitle, int activeCount, bool* open,
                    bool* resetClicked) {
  ImGui::PushID(id);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float w = ImGui::GetContentRegionAvail().x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = S(56);
  const float resetW = S(84);
  ImGui::InvisibleButton("##hdr", ImVec2(w - resetW - S(8), h));
  if (ImGui::IsItemClicked()) *open = !*open;
  bool hovered = ImGui::IsItemHovered();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hovered ? col::cardHover : col::card, S(12));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), col::border, S(12));
  ImGui::PushFont(fonts().icon);
  const char* chevron = *open ? "\xEE\x9C\x8D" : "\xEE\x9D\xAA";  // E70D ChevronDown / E76C ChevronRight
  dl->AddText(ImVec2(p.x + S(16), p.y + (h - ImGui::GetFontSize()) * 0.5f), col::textDim, chevron);
  ImGui::PopFont();
  ImGui::PushFont(fonts().hero);
  dl->AddText(ImVec2(p.x + S(46), p.y + S(8)), col::text, title);
  float titleW = ImGui::CalcTextSize(title).x;
  ImGui::PopFont();
  ImGui::PushFont(fonts().caption);
  dl->AddText(ImVec2(p.x + S(46), p.y + S(32)), col::textDim, subtitle);
  if (activeCount > 0) {
    char b[32];
    std::snprintf(b, sizeof(b), "%d on", activeCount);
    ImVec2 bs = ImGui::CalcTextSize(b);
    ImVec2 bp(p.x + S(56) + titleW, p.y + S(11));
    dl->AddRectFilled(bp, ImVec2(bp.x + bs.x + S(14), bp.y + bs.y + S(4)), withAlpha(col::accent, 0.25f), S(8));
    dl->AddText(ImVec2(bp.x + S(7), bp.y + S(2)), col::accentHi, b);
  }
  ImGui::PopFont();
  ImGui::SameLine(0, S(8));
  ImGui::SetCursorScreenPos(ImVec2(p.x + w - resetW - S(10), p.y + (h - S(34)) * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(7)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
  ImGui::PushFont(fonts().caption);
  *resetClicked = ImGui::Button("Reset all", ImVec2(resetW, S(34)));
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    ImGui::SetTooltip("Turn off every filter in this category and restore their default values");
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + S(8)));
  ImGui::PopID();
  return *open;
}

bool RadioCard(const char* id, const char* glyph, const char* title, const char* description, bool selected,
               float width, const char* badge) {
  ImGui::PushID(id);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float pad = S(16);
  // Measure description height.
  ImGui::PushFont(fonts().caption);
  float textW = width - pad * 2 - S(44);
  ImVec2 ds = ImGui::CalcTextSize(description, nullptr, false, textW);
  ImGui::PopFont();
  float h = std::max(S(84), pad * 2 + S(24) + ds.y);
  bool clicked = ImGui::InvisibleButton("##rc", ImVec2(width, h));
  bool hovered = ImGui::IsItemHovered();
  float t = animate(ImGui::GetItemID(), selected ? 1.0f : 0.0f);
  ImU32 bg = lerpCol(hovered ? col::cardHover : col::rgba(30, 26, 44), withAlpha(col::accent, 0.14f), t);
  dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), bg, S(12));
  dl->AddRect(p, ImVec2(p.x + width, p.y + h), lerpCol(col::border, col::accent, t), S(12), 0, S(1.5f));
  ImGui::PushFont(fonts().icon);
  dl->AddText(ImVec2(p.x + pad, p.y + pad + S(2)), selected ? col::accentHi : col::textDim, glyph);
  ImGui::PopFont();
  ImGui::PushFont(fonts().semibold);
  dl->AddText(ImVec2(p.x + pad + S(34), p.y + pad), col::text, title);
  float titleW = ImGui::CalcTextSize(title).x;
  ImGui::PopFont();
  if (badge) {
    ImGui::PushFont(fonts().caption);
    ImVec2 bs = ImGui::CalcTextSize(badge);
    ImVec2 bp(p.x + pad + S(44) + titleW, p.y + pad + S(1));
    dl->AddRectFilled(bp, ImVec2(bp.x + bs.x + S(14), bp.y + bs.y + S(4)), withAlpha(col::success, 0.18f), S(8));
    dl->AddText(ImVec2(bp.x + S(7), bp.y + S(2)), col::success, badge);
    ImGui::PopFont();
  }
  ImGui::PushFont(fonts().caption);
  dl->AddText(nullptr, 0.0f, ImVec2(p.x + pad + S(34), p.y + pad + S(26)), col::textDim, description, nullptr, textW);
  ImGui::PopFont();
  // Selection dot
  ImVec2 dc(p.x + width - pad - S(8), p.y + pad + S(10));
  dl->AddCircle(dc, S(8), selected ? col::accent : col::textFaint, 20, S(1.5f));
  if (selected) dl->AddCircleFilled(dc, S(4.5f), col::accentHi);
  ImGui::PopID();
  return clicked;
}

void Banner(const char* glyph, ImU32 color, const char* text) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float w = ImGui::GetContentRegionAvail().x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float pad = S(14);
  ImGui::PushFont(fonts().regular);
  ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, w - pad * 2 - S(32));
  ImGui::PopFont();
  float h = ts.y + pad * 2;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), withAlpha(color, 0.12f), S(12));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), withAlpha(color, 0.45f), S(12));
  ImGui::PushFont(fonts().icon);
  dl->AddText(ImVec2(p.x + pad, p.y + pad), color, glyph);
  ImGui::PopFont();
  ImGui::PushFont(fonts().regular);
  dl->AddText(nullptr, 0.0f, ImVec2(p.x + pad + S(30), p.y + pad), col::text, text, nullptr, w - pad * 2 - S(32));
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(w, h));
}

void Hint(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, col::textDim);
  ImGui::PushFont(fonts().caption);
  ImGui::TextWrapped("%s", text);
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

void HelpTooltip(const char* text) {
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, col::textFaint);
  ImGui::TextUnformatted(icon::info);
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetNextWindowSize(ImVec2(S(340), 0));
    ImGui::BeginTooltip();
    ImGui::TextWrapped("%s", text);
    ImGui::EndTooltip();
  }
}

void SectionLabel(const char* text) {
  ImGui::PushFont(fonts().caption);
  ImGui::PushStyleColor(ImGuiCol_Text, col::textFaint);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void DrawLogo(ImVec2 pos, float size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float r = size * 0.34f;
  ImVec2 c(pos.x + size * 0.72f, pos.y + size * 0.5f);
  for (int i = 5; i >= 1; --i) {
    float off = float(i) * size * 0.056f;
    dl->AddCircleFilled(ImVec2(c.x - off, c.y), r, withAlpha(col::accentHi, 0.12f + 0.05f * float(5 - i)), 32);
  }
  dl->AddCircleFilled(c, r, col::accent, 32);
  dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.45f, withAlpha(col::rgba(255, 255, 255), 0.18f), 24);
}

}  // namespace bl::ui
