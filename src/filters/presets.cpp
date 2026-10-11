#include "filters/presets.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "core/settings.h"
#include "core/util.h"

namespace bl {

std::map<std::string, std::string> parseKeyValues(const std::string& text) {
  std::map<std::string, std::string> kv;
  std::istringstream in(text);
  std::string line;
  auto trim = [](const std::string& v) {
    auto b = v.find_first_not_of(" \t\r");
    auto e = v.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
  };
  while (std::getline(in, line)) {
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);  // UTF-8 BOM
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return kv;
}

void writeFilterSettings(std::ostream& o, const FilterSettings& fs) {
  o << std::setprecision(9);  // exact float round-trip
  for (const auto& d : filterRegistry()) {
    const FilterState& s = fs[d.id];
    o << "filter." << d.key << ".enabled = " << (s.enabled ? 1 : 0) << "\n";
    for (size_t i = 0; i < d.params.size(); ++i) o << "filter." << d.key << "." << d.params[i].key << " = " << s.values[i] << "\n";
  }
  o << "filter.lut_path = " << fs.lutPath << "\n";
}

void readFilterSettings(const std::map<std::string, std::string>& kv, FilterSettings& fs) {
  for (const auto& d : filterRegistry()) {
    FilterState& s = fs[d.id];
    std::string base = std::string("filter.") + d.key + ".";
    if (auto it = kv.find(base + "enabled"); it != kv.end()) s.enabled = it->second == "1" || it->second == "true";
    s.values.resize(d.params.size(), 0.0f);
    for (size_t i = 0; i < d.params.size(); ++i)
      if (auto it = kv.find(base + d.params[i].key); it != kv.end()) {
        try {
          s.values[i] = std::stof(it->second);
        } catch (...) {
        }
      }
  }
  if (auto it = kv.find("filter.lut_path"); it != kv.end()) fs.lutPath = it->second;
  sanitizeFilters(fs);
}

std::string serializePreset(const Preset& p) {
  std::ostringstream o;
  o << std::setprecision(9);
  o << "# Blur Live preset\n";
  o << "name = " << p.name << "\n";
  o << "description = " << p.description << "\n";
  o << "blur.enabled = " << (p.blurEnabled ? 1 : 0) << "\n";
  o << "blur.amount = " << p.blurAmount << "\n";
  o << "blur.reference_fps = " << p.referenceFps << "\n";
  o << "blur.weighting = " << p.weighting << "\n";
  o << "blur.quality = " << p.quality << "\n";
  writeFilterSettings(o, p.filters);
  return o.str();
}

bool parsePreset(const std::string& text, Preset& out, std::string* error) {
  auto kv = parseKeyValues(text);
  auto it = kv.find("name");
  if (it == kv.end() || it->second.empty()) {
    if (error) *error = "Not a Blur Live preset (no name).";
    return false;
  }
  Preset p;
  p.name = it->second;
  if (auto d = kv.find("description"); d != kv.end()) p.description = d->second;
  auto getF = [&](const char* k, float& v) {
    if (auto f = kv.find(k); f != kv.end()) try { v = std::stof(f->second); } catch (...) {}
  };
  auto getI = [&](const char* k, int& v) {
    if (auto f = kv.find(k); f != kv.end()) try { v = std::stoi(f->second); } catch (...) {}
  };
  if (auto b = kv.find("blur.enabled"); b != kv.end()) p.blurEnabled = b->second == "1";
  getF("blur.amount", p.blurAmount);
  getF("blur.reference_fps", p.referenceFps);
  getI("blur.weighting", p.weighting);
  getI("blur.quality", p.quality);
  p.blurAmount = std::clamp(p.blurAmount, 0.0f, 3.0f);
  p.referenceFps = std::clamp(p.referenceFps, 10.0f, 1000.0f);
  p.weighting = std::clamp(p.weighting, 0, int(Weighting::Count) - 1);
  p.quality = std::clamp(p.quality, 0, kQualityLevels - 1);
  p.filters = defaultFilterSettings();
  readFilterSettings(kv, p.filters);
  out = std::move(p);
  return true;
}

Preset presetFromSettings(const Settings& s, const std::string& name) {
  Preset p;
  p.name = name;
  p.filters = s.filters;
  p.blurEnabled = s.blurEnabled;
  p.blurAmount = s.blurAmount;
  p.referenceFps = s.referenceFps;
  p.weighting = int(s.weighting.type);
  p.quality = s.quality;
  return p;
}

void applyPresetToSettings(const Preset& p, Settings& s) {
  s.filters = p.filters;
  s.blurEnabled = p.blurEnabled;
  s.blurAmount = p.blurAmount;
  s.referenceFps = p.referenceFps;
  s.weighting.type = Weighting(p.weighting);
  s.quality = p.quality;
  s.profile = Profile::Custom;
  s.activePreset = p.name;
}

std::vector<Preset> builtinPresets() {
  std::vector<Preset> out;
  auto make = [&](const char* name, const char* desc) -> Preset& {
    Preset p;
    p.name = name;
    p.description = desc;
    p.builtIn = true;
    out.push_back(p);
    return out.back();
  };
  auto enable = [](Preset& p, FilterId id, std::initializer_list<std::pair<const char*, float>> values) {
    p.filters[id].enabled = true;
    for (auto& [k, v] : values) p.filters.set(id, k, v);
  };

  make("Original", "Everything off: the untouched captured image.");

  {
    Preset& p = make("VALORANT Competitive",
                     "Readable silhouettes and textures, easier-to-read dark corners, natural colour. No motion blur, "
                     "bloom, grain or lens effects; low GPU cost. A visual preference, not an aim advantage.");
    enable(p, FilterId::Sharpen, {{"intensity", 0.45f}, {"detail", 0.6f}, {"edges", 0.35f}, {"threshold", 0.012f}});
    enable(p, FilterId::ShadowsHighlights, {{"shadows", 0.28f}, {"highlights", -0.08f}, {"local", 0.6f}});
    enable(p, FilterId::Clarity, {{"strength", 0.12f}, {"radius", 0.3f}});
    enable(p, FilterId::Vibrance, {{"amount", 0.12f}});
  }
  {
    Preset& p = make("Cinematic Motion",
                     "Blur's classic 1/60 s motion blur with a gentle teal/orange grade and a soft vignette.");
    p.blurEnabled = true;
    p.blurAmount = 1.0f;
    enable(p, FilterId::ColorGrading, {{"amount", 0.6f}});
    enable(p, FilterId::Vignette, {{"amount", 0.22f}});
    enable(p, FilterId::Contrast, {{"amount", 0.08f}});
  }
  {
    Preset& p = make("Vivid & Crisp", "Punchier colour via vibrance (saturation stays off), extra contrast and detail.");
    enable(p, FilterId::Vibrance, {{"amount", 0.3f}});
    enable(p, FilterId::Contrast, {{"amount", 0.12f}});
    enable(p, FilterId::Sharpen, {{"intensity", 0.35f}});
    enable(p, FilterId::Clarity, {{"strength", 0.2f}});
  }
  {
    Preset& p = make("Bright & Clear", "Lifts dark scenes: shadow recovery, slight exposure boost and local contrast.");
    enable(p, FilterId::Exposure, {{"stops", 0.25f}});
    enable(p, FilterId::ShadowsHighlights, {{"shadows", 0.45f}, {"highlights", -0.15f}});
    enable(p, FilterId::LocalContrast, {{"strength", 0.25f}});
  }
  {
    Preset& p = make("Colour-blind assist (deutan)", "Daltonization for green-weak / deuteranopic vision.");
    enable(p, FilterId::ColorVision, {{"strength", 0.8f}, {"type", 1.0f}});
  }
  return out;
}

namespace {
std::string sanitizeFileName(const std::string& name) {
  std::string s;
  for (char ch : name) s += std::strchr("<>:\"/\\|?*", ch) || static_cast<unsigned char>(ch) < 32 ? '_' : ch;
  while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
  return s.empty() ? "preset" : s;
}
bool readFile(const std::wstring& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
bool writeFile(const std::wstring& path, const std::string& text) {
  std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    if (!f.good()) return false;
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
}  // namespace

PresetStore::PresetStore(std::wstring directory) : m_dir(std::move(directory)) {
  if (m_dir.empty()) m_dir = appDataDir() + L"\\presets";
  CreateDirectoryW(m_dir.c_str(), nullptr);
  reload();
}

void PresetStore::reload() {
  m_presets = builtinPresets();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((m_dir + L"\\*.blpreset").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  std::vector<Preset> user;
  do {
    std::string text;
    Preset p;
    if (readFile(m_dir + L"\\" + fd.cFileName, text) && parsePreset(text, p, nullptr) && !find(p.name)) {
      p.builtIn = false;
      user.push_back(std::move(p));
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(user.begin(), user.end(), [](const Preset& a, const Preset& b) { return a.name < b.name; });
  m_presets.insert(m_presets.end(), user.begin(), user.end());
}

const Preset* PresetStore::find(const std::string& name) const {
  for (auto& p : m_presets)
    if (p.name == name) return &p;
  return nullptr;
}

std::string PresetStore::uniqueName(const std::string& base) const {
  if (!find(base)) return base;
  for (int i = 2;; ++i) {
    std::string n = base + " (" + std::to_string(i) + ")";
    if (!find(n)) return n;
  }
}

std::wstring PresetStore::fileFor(const std::string& name) const {
  return m_dir + L"\\" + toWide(sanitizeFileName(name)) + L".blpreset";
}

bool PresetStore::save(const Preset& p, std::string* error) {
  if (p.name.empty()) {
    if (error) *error = "Preset name can't be empty.";
    return false;
  }
  if (const Preset* e = find(p.name); e && e->builtIn) {
    if (error) *error = "Built-in presets can't be overwritten. Save it under a new name.";
    return false;
  }
  Preset copy = p;
  copy.builtIn = false;
  if (!writeFile(fileFor(copy.name), serializePreset(copy))) {
    if (error) *error = "Could not write the preset file.";
    return false;
  }
  reload();
  return true;
}

bool PresetStore::rename(const std::string& from, const std::string& to, std::string* error) {
  const Preset* p = find(from);
  if (!p || p->builtIn) {
    if (error) *error = "Only your own presets can be renamed.";
    return false;
  }
  if (to.empty() || (find(to) && to != from)) {
    if (error) *error = "That name is empty or already in use.";
    return false;
  }
  Preset copy = *p;
  copy.name = to;
  std::wstring oldFile = fileFor(from);
  if (!writeFile(fileFor(to), serializePreset(copy))) {
    if (error) *error = "Could not write the preset file.";
    return false;
  }
  if (fileFor(to) != oldFile) DeleteFileW(oldFile.c_str());
  reload();
  return true;
}

bool PresetStore::duplicate(const std::string& from, const std::string& to, std::string* error) {
  const Preset* p = find(from);
  if (!p) {
    if (error) *error = "Preset not found.";
    return false;
  }
  Preset copy = *p;
  copy.name = to.empty() ? uniqueName(from + " copy") : to;
  copy.builtIn = false;
  return save(copy, error);
}

bool PresetStore::remove(const std::string& name, std::string* error) {
  const Preset* p = find(name);
  if (!p || p->builtIn) {
    if (error) *error = "Built-in presets can't be deleted.";
    return false;
  }
  if (!DeleteFileW(fileFor(name).c_str())) {
    if (error) *error = "Could not delete the preset file.";
    return false;
  }
  reload();
  return true;
}

bool PresetStore::importFile(const std::wstring& path, std::string* importedName, std::string* error) {
  std::string text;
  if (!readFile(path, text)) {
    if (error) *error = "Could not read the file.";
    return false;
  }
  Preset p;
  if (!parsePreset(text, p, error)) return false;
  p.name = uniqueName(p.name);
  p.builtIn = false;
  if (!save(p, error)) return false;
  if (importedName) *importedName = p.name;
  return true;
}

bool PresetStore::exportFile(const std::string& name, const std::wstring& path, std::string* error) const {
  const Preset* p = find(name);
  if (!p) {
    if (error) *error = "Preset not found.";
    return false;
  }
  Preset copy = *p;
  copy.builtIn = false;
  if (!writeFile(path, serializePreset(copy))) {
    if (error) *error = "Could not write the file.";
    return false;
  }
  return true;
}

}  // namespace bl
