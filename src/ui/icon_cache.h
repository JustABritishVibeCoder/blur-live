#pragma once
#include <d3d11.h>
#include <imgui.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>

namespace bl::ui {

// Window icons as D3D11 textures for the source list. Icons are fetched lazily (a few per frame)
// with a hung-window-safe message timeout.
class IconCache {
 public:
  void init(ID3D11Device* device) { m_dev = device; }
  void beginFrame() { m_budget = 3; }
  ImTextureID get(HWND hwnd, const std::wstring& exePath);
  void clear() { m_map.clear(); }

 private:
  struct Entry {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  };
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> load(HWND hwnd, const std::wstring& exePath);
  Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
  std::unordered_map<HWND, Entry> m_map;
  int m_budget = 3;
};

}  // namespace bl::ui
