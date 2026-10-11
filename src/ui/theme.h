#pragma once
#include <imgui.h>

namespace bl::ui {

// Design tokens.
namespace col {
constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
constexpr ImU32 bg = rgba(11, 10, 16);
constexpr ImU32 sidebar = rgba(16, 14, 23);
constexpr ImU32 card = rgba(24, 21, 35);
constexpr ImU32 cardHover = rgba(32, 28, 46);
constexpr ImU32 cardRaised = rgba(36, 31, 52);
constexpr ImU32 border = rgba(44, 38, 64);
constexpr ImU32 text = rgba(236, 234, 244);
constexpr ImU32 textDim = rgba(160, 153, 182);
constexpr ImU32 textFaint = rgba(108, 101, 130);
constexpr ImU32 accent = rgba(139, 92, 246);
constexpr ImU32 accentHi = rgba(167, 139, 250);
constexpr ImU32 accentLo = rgba(109, 40, 217);
constexpr ImU32 accentGlow = rgba(139, 92, 246, 50);
constexpr ImU32 success = rgba(52, 211, 153);
constexpr ImU32 warning = rgba(251, 191, 36);
constexpr ImU32 danger = rgba(248, 113, 113);
constexpr ImU32 info = rgba(96, 165, 250);
}  // namespace col

// Segoe Fluent Icons / Segoe MDL2 Assets code points (UTF-8).
namespace icon {
constexpr const char* home = "\xEE\xA0\x8F";        // E80F
constexpr const char* video = "\xEE\x9C\x94";       // E714
constexpr const char* blur = "\xEE\x9E\x90";        // E790 (Color)
constexpr const char* monitor = "\xEE\x9F\xB4";     // E7F4
constexpr const char* speed = "\xEE\xB1\x8A";       // EC4A
constexpr const char* settings = "\xEE\x9C\x93";    // E713
constexpr const char* help = "\xEE\xA2\x97";        // E897
constexpr const char* play = "\xEE\x9D\xA8";        // E768
constexpr const char* stop = "\xEE\x9C\x9A";        // E71A
constexpr const char* refresh = "\xEE\x9C\xAC";     // E72C
constexpr const char* search = "\xEE\x9C\xA1";      // E721
constexpr const char* warn = "\xEE\x9E\xBA";        // E7BA
constexpr const char* error = "\xEE\x9E\x83";       // E783
constexpr const char* check = "\xEE\x9C\xBE";       // E73E
constexpr const char* game = "\xEE\x9F\xBC";        // E7FC
constexpr const char* bolt = "\xEE\xA5\x85";        // E945
constexpr const char* folder = "\xEE\xA2\xB7";      // E8B7
constexpr const char* keyboard = "\xEE\x9D\xA5";    // E765
constexpr const char* info = "\xEE\xA5\x86";        // E946
constexpr const char* close = "\xEE\xA2\xBB";       // E8BB
constexpr const char* sliders = "\xEE\xA7\xA9";     // E9E9
constexpr const char* window = "\xEE\x9C\xB7";      // E737
constexpr const char* chip = "\xEE\xA5\x90";        // E950
}  // namespace icon

struct Fonts {
  ImFont* regular = nullptr;
  ImFont* semibold = nullptr;
  ImFont* caption = nullptr;
  ImFont* title = nullptr;
  ImFont* big = nullptr;
  ImFont* hero = nullptr;
  ImFont* icon = nullptr;     // nav-size icons
  bool haveIcons = false;
};

Fonts& fonts();
// (Re)build fonts + style for a DPI scale. Must be called outside of a frame; the renderer's font
// texture has to be recreated afterwards.
void applyTheme(float dpiScale);
float dpi();  // current scale factor
inline float S(float px) { return px * dpi(); }

}  // namespace bl::ui
