#include "core/settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "core/util.h"
#include "filters/presets.h"

namespace bl {

const char* profileName(Profile p) {
  switch (p) {
    case Profile::Subtle: return "Subtle";
    case Profile::Cinematic: return "Cinematic";
    case Profile::Strong: return "Strong";
    case Profile::Responsive: return "Responsive";
    case Profile::Custom: return "Custom";
    default: return "?";
  }
}

const char* profileDescription(Profile p) {
  switch (p) {
    case Profile::Subtle: return "Half shutter (180 deg at 60 fps). Smooths motion without obvious streaks.";
    case Profile::Cinematic: return "Blur's defaults: full 1/60 s equal-weight shutter. The classic look.";
    case Profile::Strong: return "1.75x shutter with soft gaussian edges. Heavy, dreamy streaks.";
    case Profile::Responsive: return "Trail weighted towards the newest frame (Blur's gaussian). Least perceived lag.";
    case Profile::Custom: return "Your own settings.";
    default: return "";
  }
}

const char* outputModeName(OutputMode m) {
  switch (m) {
    case OutputMode::Overlay: return "Game overlay";
    case OutputMode::Window: return "Window";
    case OutputMode::Monitor: return "Fullscreen on monitor";
    default: return "?";
  }
}

int qualitySamples(int quality) {
  static const int kSamples[kQualityLevels] = {12, 24, 48, 96};
  return kSamples[std::clamp(quality, 0, kQualityLevels - 1)];
}

const char* qualityName(int quality) {
  static const char* kNames[kQualityLevels] = {"Low", "Medium", "High", "Ultra"};
  return kNames[std::clamp(quality, 0, kQualityLevels - 1)];
}

void applyProfile(Settings& s, Profile p) {
  s.profile = p;
  WeightingParams w;  // defaults = Blur's gaussian defaults (sd 1, mean 2, bound 0..2)
  switch (p) {
    case Profile::Subtle:
      s.blurAmount = 0.5f;
      s.referenceFps = 60.0f;
      w.type = Weighting::Equal;
      break;
    case Profile::Cinematic:
      s.blurAmount = 1.0f;
      s.referenceFps = 60.0f;
      w.type = Weighting::Equal;
      break;
    case Profile::Strong:
      s.blurAmount = 1.75f;
      s.referenceFps = 60.0f;
      w.type = Weighting::GaussianSym;
      w.gaussStdDev = 1.0f;
      break;
    case Profile::Responsive:
      s.blurAmount = 1.0f;
      s.referenceFps = 60.0f;
      w.type = Weighting::Gaussian;
      break;
    default: return;  // Custom keeps everything
  }
  w.custom = s.weighting.custom;
  s.weighting = w;
  s.shutterFollowsCapture = false;
  s.mode = BlurMode::MotionFlow;
}

void sanitize(Settings& s) {
  s.blurAmount = std::clamp(s.blurAmount, 0.0f, 3.0f);
  s.referenceFps = std::clamp(s.referenceFps, 10.0f, 1000.0f);
  s.quality = std::clamp(s.quality, 0, kQualityLevels - 1);
  s.blendFrames = std::clamp(s.blendFrames, 2, 16);
  s.gamma = std::clamp(s.gamma, 0.5f, 4.0f);
  s.sceneCutThreshold = std::clamp(s.sceneCutThreshold, 0.01f, 1.0f);
  s.confidenceLow = std::clamp(s.confidenceLow, 0.0f, 1.0f);
  s.confidenceHigh = std::clamp(s.confidenceHigh, s.confidenceLow + 0.005f, 1.0f);
  s.maxFrameGapMs = std::clamp(s.maxFrameGapMs, 20.0f, 2000.0f);
  s.framePoolBuffers = std::clamp(s.framePoolBuffers, 1, 4);
  s.outputMonitor = std::max(0, s.outputMonitor);
  s.compareSplit = std::clamp(s.compareSplit, 0.05f, 0.95f);
  sanitizeFilters(s.filters);
  s.weighting.gaussStdDev = std::clamp(s.weighting.gaussStdDev, 0.01f, 100.0f);
  if (int(s.weighting.type) < 0 || s.weighting.type >= Weighting::Count) s.weighting.type = Weighting::Equal;
  if (int(s.profile) < 0 || s.profile >= Profile::Count) s.profile = Profile::Custom;
  if (int(s.mode) < 0 || int(s.mode) > 1) s.mode = BlurMode::MotionFlow;
  if (int(s.outputMode) < 0 || int(s.outputMode) > 2) s.outputMode = OutputMode::Overlay;
}

std::wstring defaultSettingsPath() { return appDataDir() + L"\\settings.ini"; }

namespace {

std::string escape(const std::string& v) {
  std::string o;
  for (char c : v) {
    if (c == '\n' || c == '\r') o += ' ';
    else o += c;
  }
  return o;
}

}  // namespace

bool saveSettings(const Settings& s, const std::wstring& path) {
  std::ostringstream o;
  auto b = [&](const char* k, bool v) { o << k << " = " << (v ? 1 : 0) << "\n"; };
  auto i = [&](const char* k, int v) { o << k << " = " << v << "\n"; };
  auto f = [&](const char* k, float v) { o << k << " = " << v << "\n"; };
  auto str = [&](const char* k, const std::string& v) { o << k << " = " << escape(v) << "\n"; };

  o << "# Blur Live settings (written automatically)\n";
  b("blur_enabled", s.blurEnabled);
  i("mode", int(s.mode));
  i("profile", int(s.profile));
  f("blur_amount", s.blurAmount);
  f("reference_fps", s.referenceFps);
  b("shutter_follows_capture", s.shutterFollowsCapture);
  i("weighting", int(s.weighting.type));
  f("gauss_std_dev", s.weighting.gaussStdDev);
  f("gauss_mean", s.weighting.gaussMean);
  f("gauss_bound_lo", s.weighting.gaussBoundLo);
  f("gauss_bound_hi", s.weighting.gaussBoundHi);
  str("custom_weights", s.weighting.custom);
  i("quality", s.quality);
  i("blend_frames", s.blendFrames);
  f("gamma", s.gamma);
  f("scene_cut_threshold", s.sceneCutThreshold);
  f("confidence_low", s.confidenceLow);
  f("confidence_high", s.confidenceHigh);
  f("max_frame_gap_ms", s.maxFrameGapMs);
  b("capture_cursor", s.captureCursor);
  b("hide_capture_border", s.hideCaptureBorder);
  b("crop_to_client", s.cropToClient);
  i("frame_pool_buffers", s.framePoolBuffers);
  i("output_mode", int(s.outputMode));
  i("output_monitor", s.outputMonitor);
  b("vsync", s.vsync);
  b("auto_hide_overlay", s.autoHideOverlay);
  b("exclude_output_from_capture", s.excludeOutputFromCapture);
  b("show_all_windows", s.showAllWindows);
  str("last_target_exe", s.lastTargetExe);
  str("last_target_title", s.lastTargetTitle);
  b("hotkeys_enabled", s.hotkeysEnabled);
  b("effects_enabled", s.effectsEnabled);
  b("compare_enabled", s.compareEnabled);
  f("compare_split", s.compareSplit);
  str("active_preset", s.activePreset);
  b("live_preview", s.livePreview);
  writeFilterSettings(o, s.filters);

  // Write to a temp file and replace, so a crash mid-write never corrupts settings.
  std::wstring tmp = path + L".tmp";
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << o.str();
    if (!file.good()) return false;
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool loadSettings(Settings& s, const std::wstring& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::map<std::string, std::string> kv;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string v) {
      auto b = v.find_first_not_of(" \t");
      auto e = v.find_last_not_of(" \t");
      return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
    };
    kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }

  auto getB = [&](const char* k, bool& v) {
    if (auto it = kv.find(k); it != kv.end()) v = it->second == "1" || it->second == "true";
  };
  auto getI = [&](const char* k, int& v) {
    if (auto it = kv.find(k); it != kv.end()) try { v = std::stoi(it->second); } catch (...) {}
  };
  auto getF = [&](const char* k, float& v) {
    if (auto it = kv.find(k); it != kv.end()) try { v = std::stof(it->second); } catch (...) {}
  };
  auto getS = [&](const char* k, std::string& v) {
    if (auto it = kv.find(k); it != kv.end()) v = it->second;
  };
  int tmp = 0;
  getB("blur_enabled", s.blurEnabled);
  tmp = int(s.mode); getI("mode", tmp); s.mode = BlurMode(tmp);
  tmp = int(s.profile); getI("profile", tmp); s.profile = Profile(tmp);
  getF("blur_amount", s.blurAmount);
  getF("reference_fps", s.referenceFps);
  getB("shutter_follows_capture", s.shutterFollowsCapture);
  tmp = int(s.weighting.type); getI("weighting", tmp); s.weighting.type = Weighting(tmp);
  getF("gauss_std_dev", s.weighting.gaussStdDev);
  getF("gauss_mean", s.weighting.gaussMean);
  getF("gauss_bound_lo", s.weighting.gaussBoundLo);
  getF("gauss_bound_hi", s.weighting.gaussBoundHi);
  getS("custom_weights", s.weighting.custom);
  getI("quality", s.quality);
  getI("blend_frames", s.blendFrames);
  getF("gamma", s.gamma);
  getF("scene_cut_threshold", s.sceneCutThreshold);
  getF("confidence_low", s.confidenceLow);
  getF("confidence_high", s.confidenceHigh);
  getF("max_frame_gap_ms", s.maxFrameGapMs);
  getB("capture_cursor", s.captureCursor);
  getB("hide_capture_border", s.hideCaptureBorder);
  getB("crop_to_client", s.cropToClient);
  getI("frame_pool_buffers", s.framePoolBuffers);
  tmp = int(s.outputMode); getI("output_mode", tmp); s.outputMode = OutputMode(tmp);
  getI("output_monitor", s.outputMonitor);
  getB("vsync", s.vsync);
  getB("auto_hide_overlay", s.autoHideOverlay);
  getB("exclude_output_from_capture", s.excludeOutputFromCapture);
  getB("show_all_windows", s.showAllWindows);
  getS("last_target_exe", s.lastTargetExe);
  getS("last_target_title", s.lastTargetTitle);
  getB("hotkeys_enabled", s.hotkeysEnabled);
  getB("effects_enabled", s.effectsEnabled);
  getB("compare_enabled", s.compareEnabled);
  getF("compare_split", s.compareSplit);
  getS("active_preset", s.activePreset);
  getB("live_preview", s.livePreview);
  readFilterSettings(kv, s.filters);
  sanitize(s);
  return true;
}

}  // namespace bl
