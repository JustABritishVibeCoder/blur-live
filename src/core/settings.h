#pragma once
#include <string>

#include "core/weighting.h"
#include "filters/filter_registry.h"

namespace bl {

enum class BlurMode { MotionFlow = 0, FrameBlend = 1 };
enum class OutputMode { Overlay = 0, Window = 1, Monitor = 2 };
enum class Profile { Subtle = 0, Cinematic, Strong, Responsive, Custom, Count };

const char* profileName(Profile p);
const char* profileDescription(Profile p);
const char* outputModeName(OutputMode m);

// Temporal samples per fully-covered shutter window for each quality level.
int qualitySamples(int quality);
const char* qualityName(int quality);
constexpr int kQualityLevels = 4;

struct Settings {
  // ---- Effect ----
  bool blurEnabled = true;
  BlurMode mode = BlurMode::MotionFlow;
  Profile profile = Profile::Cinematic;
  float blurAmount = 1.0f;        // Blur's "blur amount": shutter = amount / referenceFps
  float referenceFps = 60.0f;     // Blur's "blur output fps"
  bool shutterFollowsCapture = false;  // shutter = amount * measured capture interval
  WeightingParams weighting;
  int quality = 2;                // 0..3 -> 12/24/48/96 samples per window
  int blendFrames = 4;            // FrameBlend mode history length
  float gamma = 1.0f;             // 1 = blend in display space (Blur default); >1 = brighter highlights

  // ---- Robustness ----
  float sceneCutThreshold = 0.06f;  // mean block error (0..1 luma) above which a frame pair is a cut
  float confidenceLow = 0.035f;     // per-block error where motion starts being distrusted
  float confidenceHigh = 0.09f;      // per-block error where motion is fully distrusted (pixel stays sharp)
  float maxFrameGapMs = 100.0f;     // longer gaps between frames break the history (pause / alt-tab)

  // ---- Capture ----
  bool captureCursor = false;
  bool hideCaptureBorder = true;
  bool cropToClient = true;       // drop the title bar / borders of windowed games
  int framePoolBuffers = 2;

  // ---- Output ----
  OutputMode outputMode = OutputMode::Overlay;
  int outputMonitor = 0;            // index into the monitor list for OutputMode::Monitor
  bool vsync = false;
  bool autoHideOverlay = true;      // hide the overlay whenever the game is not the foreground window
  bool excludeOutputFromCapture = false;

  // ---- Filters ----
  bool effectsEnabled = true;     // master switch: off = untouched capture (motion blur + filters bypassed)
  FilterSettings filters = defaultFilterSettings();  // every filter starts disabled
  bool compareEnabled = false;    // split view: original left, processed right
  float compareSplit = 0.5f;
  std::string activePreset;       // last applied preset name ("" = none)
  bool livePreview = true;        // processed-output thumbnail in the control panel

  // ---- UI / session ----
  bool showAllWindows = false;
  std::string lastTargetExe;
  std::string lastTargetTitle;
  bool hotkeysEnabled = true;

  bool operator==(const Settings&) const = default;
};

void applyProfile(Settings& s, Profile p);

// Simple "key = value" text format in %APPDATA%\BlurLive\settings.ini.
bool saveSettings(const Settings& s, const std::wstring& path);
bool loadSettings(Settings& s, const std::wstring& path);  // missing keys keep their current values
std::wstring defaultSettingsPath();

// Clamp every field into its valid range (call after loading or editing).
void sanitize(Settings& s);

}  // namespace bl
