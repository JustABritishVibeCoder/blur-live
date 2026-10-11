#include "ui/icon_cache.h"

#include <shellapi.h>

#include <algorithm>
#include <vector>

namespace bl::ui {

namespace {

HICON fetchIcon(HWND hwnd, const std::wstring& exePath, bool& owned) {
  owned = false;
  DWORD_PTR r = 0;
  if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &r) && r)
    return reinterpret_cast<HICON>(r);
  if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &r) && r)
    return reinterpret_cast<HICON>(r);
  if (HICON h = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON))) return h;
  if (!exePath.empty()) {
    HICON large = nullptr;
    if (ExtractIconExW(exePath.c_str(), 0, &large, nullptr, 1) > 0 && large) {
      owned = true;
      return large;
    }
  }
  return nullptr;
}

// Renders the icon on black and on white to recover straight alpha for any icon format.
bool rasterize(HICON icon, int size, std::vector<uint32_t>& rgba) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  void* bitsB = nullptr;
  void* bitsW = nullptr;
  HBITMAP bmB = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bitsB, nullptr, 0);
  HBITMAP bmW = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bitsW, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!bmB || !bmW) {
    if (bmB) DeleteObject(bmB);
    if (bmW) DeleteObject(bmW);
    DeleteDC(dc);
    return false;
  }
  const size_t n = size_t(size) * size_t(size);
  HGDIOBJ old = SelectObject(dc, bmB);
  std::fill_n(static_cast<uint32_t*>(bitsB), n, 0xFF000000u);
  DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
  SelectObject(dc, bmW);
  std::fill_n(static_cast<uint32_t*>(bitsW), n, 0xFFFFFFFFu);
  DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
  GdiFlush();
  SelectObject(dc, old);

  rgba.resize(n);
  const uint32_t* b = static_cast<const uint32_t*>(bitsB);
  const uint32_t* w = static_cast<const uint32_t*>(bitsW);
  for (size_t i = 0; i < n; ++i) {
    int bg = int((b[i] >> 8) & 0xFF), wg = int((w[i] >> 8) & 0xFF);
    int a = std::clamp(255 - (wg - bg), 0, 255);
    auto ch = [&](int shift) {
      int v = int((b[i] >> shift) & 0xFF);
      return a > 0 ? std::min(255, v * 255 / a) : 0;
    };
    // BGRA source -> RGBA8 texture
    rgba[i] = uint32_t(ch(16)) | (uint32_t(ch(8)) << 8) | (uint32_t(ch(0)) << 16) | (uint32_t(a) << 24);
  }
  DeleteObject(bmB);
  DeleteObject(bmW);
  DeleteDC(dc);
  return true;
}

}  // namespace

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> IconCache::load(HWND hwnd, const std::wstring& exePath) {
  bool owned = false;
  HICON icon = fetchIcon(hwnd, exePath, owned);
  if (!icon) return nullptr;
  const int size = 48;
  std::vector<uint32_t> px;
  bool ok = rasterize(icon, size, px);
  if (owned) DestroyIcon(icon);
  if (!ok) return nullptr;

  D3D11_TEXTURE2D_DESC d{};
  d.Width = d.Height = UINT(size);
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{px.data(), UINT(size * 4), 0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  if (FAILED(m_dev->CreateTexture2D(&d, &init, &tex))) return nullptr;
  if (FAILED(m_dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
  return srv;
}

ImTextureID IconCache::get(HWND hwnd, const std::wstring& exePath) {
  auto it = m_map.find(hwnd);
  if (it == m_map.end()) {
    if (m_budget <= 0 || !m_dev) return ImTextureID{};
    --m_budget;
    it = m_map.emplace(hwnd, Entry{load(hwnd, exePath)}).first;
  }
  return reinterpret_cast<ImTextureID>(it->second.srv.Get());
}

}  // namespace bl::ui
