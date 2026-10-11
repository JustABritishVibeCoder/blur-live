#include "ui/theme.h"

#include <windows.h>

#include <string>

namespace bl::ui {

namespace {
Fonts g_fonts;
float g_dpi = 1.0f;

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string fontPath(const wchar_t* file) {
  wchar_t win[MAX_PATH];
  GetWindowsDirectoryW(win, MAX_PATH);
  std::wstring p = std::wstring(win) + L"\\Fonts\\" + file;
  if (!fileExists(p)) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}
}  // namespace

Fonts& fonts() { return g_fonts; }
float dpi() { return g_dpi; }

void applyTheme(float dpiScale) {
  g_dpi = dpiScale;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  g_fonts = {};

  const std::string regular = fontPath(L"segoeui.ttf");
  const std::string semibold = fontPath(L"seguisb.ttf");
  std::string icons = fontPath(L"SegoeIcons.ttf");  // Windows 11 (Segoe Fluent Icons)
  if (icons.empty()) icons = fontPath(L"segmdl2.ttf");  // Windows 10 (Segoe MDL2 Assets)

  static const ImWchar kIconRange[] = {0xE700, 0xF8FF, 0};
  static const ImWchar kTextRange[] = {0x0020, 0x00FF, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x25A0, 0x25FF, 0};

  auto load = [&](const std::string& path, float px, bool mergeIcons) -> ImFont* {
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;
    ImFont* f = path.empty() ? io.Fonts->AddFontDefault(&cfg)
                             : io.Fonts->AddFontFromFileTTF(path.c_str(), px * dpiScale, &cfg, kTextRange);
    if (mergeIcons && !icons.empty()) {
      ImFontConfig ic;
      ic.MergeMode = true;
      ic.GlyphOffset = ImVec2(0, 2.0f * dpiScale);
      ic.GlyphMinAdvanceX = px * dpiScale;
      io.Fonts->AddFontFromFileTTF(icons.c_str(), px * dpiScale * 0.95f, &ic, kIconRange);
    }
    return f;
  };

  g_fonts.regular = load(regular, 16.0f, true);
  g_fonts.semibold = load(semibold.empty() ? regular : semibold, 16.0f, true);
  g_fonts.caption = load(regular, 13.5f, true);
  g_fonts.title = load(semibold.empty() ? regular : semibold, 24.0f, true);
  g_fonts.big = load(semibold.empty() ? regular : semibold, 30.0f, false);
  g_fonts.hero = load(semibold.empty() ? regular : semibold, 19.0f, true);
  if (!icons.empty()) {
    ImFontConfig ic;
    g_fonts.icon = io.Fonts->AddFontFromFileTTF(icons.c_str(), 18.0f * dpiScale, &ic, kIconRange);
    g_fonts.haveIcons = true;
  } else {
    g_fonts.icon = g_fonts.regular;
  }
  io.FontDefault = g_fonts.regular;

  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  s.WindowPadding = ImVec2(0, 0);
  s.FramePadding = ImVec2(12, 8);
  s.ItemSpacing = ImVec2(10, 10);
  s.ItemInnerSpacing = ImVec2(8, 6);
  s.CellPadding = ImVec2(8, 6);
  s.ScrollbarSize = 10;
  s.GrabMinSize = 14;
  s.WindowRounding = 0;
  s.ChildRounding = 12;
  s.FrameRounding = 8;
  s.PopupRounding = 10;
  s.ScrollbarRounding = 8;
  s.GrabRounding = 8;
  s.TabRounding = 8;
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 1;
  s.PopupBorderSize = 1;
  s.FrameBorderSize = 0;
  s.SelectableTextAlign = ImVec2(0, 0.5f);
  s.ScaleAllSizes(dpiScale);

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = v4(col::text);
  c[ImGuiCol_TextDisabled] = v4(col::textFaint);
  c[ImGuiCol_WindowBg] = v4(col::bg);
  c[ImGuiCol_ChildBg] = v4(col::card);
  c[ImGuiCol_PopupBg] = v4(col::cardRaised);
  c[ImGuiCol_Border] = v4(col::border);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = v4(col::rgba(34, 30, 50));
  c[ImGuiCol_FrameBgHovered] = v4(col::rgba(44, 38, 64));
  c[ImGuiCol_FrameBgActive] = v4(col::rgba(52, 44, 76));
  c[ImGuiCol_TitleBg] = v4(col::sidebar);
  c[ImGuiCol_TitleBgActive] = v4(col::sidebar);
  c[ImGuiCol_MenuBarBg] = v4(col::sidebar);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = v4(col::rgba(60, 52, 86));
  c[ImGuiCol_ScrollbarGrabHovered] = v4(col::rgba(80, 70, 112));
  c[ImGuiCol_ScrollbarGrabActive] = v4(col::accent);
  c[ImGuiCol_CheckMark] = v4(col::accentHi);
  c[ImGuiCol_SliderGrab] = v4(col::accentHi);
  c[ImGuiCol_SliderGrabActive] = v4(col::rgba(196, 181, 253));
  c[ImGuiCol_Button] = v4(col::rgba(40, 35, 58));
  c[ImGuiCol_ButtonHovered] = v4(col::rgba(54, 47, 78));
  c[ImGuiCol_ButtonActive] = v4(col::rgba(66, 57, 96));
  c[ImGuiCol_Header] = v4(col::rgba(139, 92, 246, 60));
  c[ImGuiCol_HeaderHovered] = v4(col::rgba(139, 92, 246, 40));
  c[ImGuiCol_HeaderActive] = v4(col::rgba(139, 92, 246, 80));
  c[ImGuiCol_Separator] = v4(col::border);
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_Tab] = v4(col::card);
  c[ImGuiCol_PlotLines] = v4(col::accentHi);
  c[ImGuiCol_PlotHistogram] = v4(col::accent);
  c[ImGuiCol_TextSelectedBg] = v4(col::rgba(139, 92, 246, 90));
  c[ImGuiCol_NavHighlight] = v4(col::accent);
  c[ImGuiCol_ModalWindowDimBg] = v4(col::rgba(0, 0, 0, 140));
}

}  // namespace bl::ui
