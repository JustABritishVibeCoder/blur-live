#pragma once
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "filters/filter_registry.h"

namespace bl {

struct Settings;

// A saved look: every filter's state plus the motion-blur look.
struct Preset {
  std::string name;
  std::string description;
  bool builtIn = false;
  FilterSettings filters = defaultFilterSettings();
  bool blurEnabled = false;
  float blurAmount = 1.0f;
  float referenceFps = 60.0f;
  int weighting = 0;
  int quality = 2;
  bool operator==(const Preset&) const = default;
};

// Generic "key = value" (de)serialisation of filter settings, driven by the registry:
//   filter.<key>.enabled = 1
//   filter.<key>.<param> = 0.5
//   filter.lut_path = C:\...\look.cube
void writeFilterSettings(std::ostream& o, const FilterSettings& fs);
void readFilterSettings(const std::map<std::string, std::string>& kv, FilterSettings& fs);
std::map<std::string, std::string> parseKeyValues(const std::string& text);

std::string serializePreset(const Preset& p);
bool parsePreset(const std::string& text, Preset& out, std::string* error);

Preset presetFromSettings(const Settings& s, const std::string& name);
void applyPresetToSettings(const Preset& p, Settings& s);

std::vector<Preset> builtinPresets();

// Built-in presets + user presets stored as one .blpreset file each.
class PresetStore {
 public:
  explicit PresetStore(std::wstring directory = {});  // default: %APPDATA%\BlurLive\presets
  void reload();
  const std::vector<Preset>& all() const { return m_presets; }
  const Preset* find(const std::string& name) const;
  std::string uniqueName(const std::string& base) const;

  bool save(const Preset& p, std::string* error);  // create or overwrite a user preset
  bool rename(const std::string& from, const std::string& to, std::string* error);
  bool duplicate(const std::string& from, const std::string& to, std::string* error);
  bool remove(const std::string& name, std::string* error);
  bool importFile(const std::wstring& path, std::string* importedName, std::string* error);
  bool exportFile(const std::string& name, const std::wstring& path, std::string* error) const;
  const std::wstring& directory() const { return m_dir; }

 private:
  std::wstring fileFor(const std::string& name) const;
  std::wstring m_dir;
  std::vector<Preset> m_presets;
};

}  // namespace bl
