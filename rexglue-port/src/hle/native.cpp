// q4hle - native D3D11 renderer: device, swap chain and presentation.

#include "hle/native.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include "hle/log.h"

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, q4hle_vsync);  // q4_graphics.cpp (GRAPHICS menu)

namespace q4hle::native {
namespace {

std::atomic<bool> g_enabled{false};
bool g_offscreen = false;            // shadow mode: render next to Xenia's GPU, no swap chain
ID3D11Texture2D* g_offscreen_tex = nullptr;
std::function<void*()> g_window_provider;
std::mutex g_mu;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain1* g_swap = nullptr;
ID3D11RenderTargetView* g_bb_rtv = nullptr;
HWND g_hwnd = nullptr;
uint32_t g_bb_w = 0, g_bb_h = 0;
bool g_failed = false;

void ReleaseBackBuffer() {
  if (g_bb_rtv) {
    g_bb_rtv->Release();
    g_bb_rtv = nullptr;
  }
}

bool CreateBackBufferView() {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
  HRESULT hr = g_device->CreateRenderTargetView(bb, nullptr, &g_bb_rtv);
  bb->Release();
  return SUCCEEDED(hr);
}

void FollowWindowSize() {
  RECT rc{};
  if (!GetClientRect(g_hwnd, &rc)) return;
  uint32_t w = static_cast<uint32_t>(rc.right - rc.left), h = static_cast<uint32_t>(rc.bottom - rc.top);
  if (w == 0 || h == 0 || (w == g_bb_w && h == g_bb_h)) return;
  ReleaseBackBuffer();
  g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  if (SUCCEEDED(g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
    g_bb_w = w;
    g_bb_h = h;
  }
  CreateBackBufferView();
}

}  // namespace

void SetEnabled(bool on) { g_enabled = on; }
bool Enabled() { return g_enabled.load(); }
void SetWindowProvider(std::function<void*()> provider) { g_window_provider = std::move(provider); }

bool EnsureDevice() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_swap || g_offscreen_tex) return true;
  if (g_failed || !g_window_provider) return false;
  g_hwnd = static_cast<HWND>(g_window_provider());
  if (!g_hwnd && !g_offscreen) return false;

  UINT flags = 0;
  if (const char* e = std::getenv("Q4_HLE_DEBUG_LAYER"); e && *e == '1') flags |= D3D11_CREATE_DEVICE_DEBUG;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                 D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx);
  if (FAILED(hr)) {
    Log("native: D3D11CreateDevice failed %08X", static_cast<unsigned>(hr));
    g_failed = true;
    return false;
  }
  if (g_offscreen) {
    g_bb_w = 1280;
    g_bb_h = 720;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_bb_w;
    td.Height = g_bb_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    g_device->CreateTexture2D(&td, nullptr, &g_offscreen_tex);
    g_device->CreateRenderTargetView(g_offscreen_tex, nullptr, &g_bb_rtv);
    Log("native: D3D11 device created (shadow mode, offscreen %ux%u)", g_bb_w, g_bb_h);
    return true;
  }
  IDXGIDevice* dxgi_dev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* factory = nullptr;
  g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev));
  dxgi_dev->GetAdapter(&adapter);
  adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));

  RECT rc{};
  GetClientRect(g_hwnd, &rc);
  g_bb_w = static_cast<uint32_t>(rc.right - rc.left);
  g_bb_h = static_cast<uint32_t>(rc.bottom - rc.top);
  if (!g_bb_w || !g_bb_h) g_bb_w = 1280, g_bb_h = 720;

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = g_bb_w;
  sd.Height = g_bb_h;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Scaling = DXGI_SCALING_STRETCH;
  hr = factory->CreateSwapChainForHwnd(g_device, g_hwnd, &sd, nullptr, nullptr, &g_swap);
  if (SUCCEEDED(hr)) factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
  factory->Release();
  adapter->Release();
  dxgi_dev->Release();
  if (FAILED(hr)) {
    Log("native: CreateSwapChainForHwnd failed %08X", static_cast<unsigned>(hr));
    g_failed = true;
    return false;
  }
  CreateBackBufferView();
  Log("native: D3D11 device and %ux%u swap chain created", g_bb_w, g_bb_h);
  return true;
}

ID3D11Device* Device() { return g_device; }
ID3D11DeviceContext* Context() { return g_ctx; }
uint32_t BackBufferWidth() { return g_bb_w; }
uint32_t BackBufferHeight() { return g_bb_h; }

void ClearBackBuffer(float r, float g, float b) {
  if (!g_bb_rtv) return;
  const float c[4] = {r, g, b, 1.0f};
  g_ctx->ClearRenderTargetView(g_bb_rtv, c);
}

ID3D11RenderTargetView* BackBufferRTV() { return g_bb_rtv; }

// Writes a 24-bit BMP from 32-bit RGBA (or BGRA) pixels.
void WriteBmp(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch, bool bgra) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int o, uint32_t v) { std::memcpy(hdr + o, &v, 4); };
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, w);
  put32(22, h);
  hdr[26] = 1;
  hdr[28] = 24;
  put32(34, row * h);
  std::fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> line(row, 0);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* src = rgba + size_t(h - 1 - y) * pitch;
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = src + 4 * x;
      line[3 * x + 0] = bgra ? p[0] : p[2];
      line[3 * x + 1] = p[1];
      line[3 * x + 2] = bgra ? p[2] : p[0];
    }
    std::fwrite(line.data(), 1, row, f);
  }
  std::fclose(f);
}

namespace {
// Writes the offscreen image (shadow mode) and the window's on-screen contents (Xenia's output)
// as BMP files for side-by-side comparison.
std::vector<double> g_dump_times;
size_t g_dump_next = 0;
auto g_t0 = std::chrono::steady_clock::now();

void DumpIfDue() {
  if (g_dump_times.empty() && g_dump_next == 0) {
    if (const char* e = std::getenv("Q4_HLE_DUMP_TIMES")) {
      std::string s = e;
      size_t p = 0;
      while (p < s.size()) {
        const size_t q = s.find(',', p);
        g_dump_times.push_back(std::atof(s.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str()));
        if (q == std::string::npos) break;
        p = q + 1;
      }
    }
    g_dump_next = 1;  // parsed
  }
  std::string dir = ".";
  if (const char* d = std::getenv("Q4_TRACE_DIR")) dir = d;
  char name[64];
  // Q4_HLE_DUMP_BURST=n: after each dump time, also write the next n presented frames
  // (native_tNNN_fKK.bmp) from the swap chain or offscreen image, to catch flicker.
  static int burst_left = 0, burst_index = 0, burst_t = 0;
  if (burst_left > 0) {
    ID3D11Texture2D* src = g_offscreen_tex;
    ID3D11Texture2D* back = nullptr;
    if (!src && g_swap && SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
      src = back;
    if (src) {
      D3D11_TEXTURE2D_DESC td;
      src->GetDesc(&td);
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.MiscFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Texture2D* st = nullptr;
      if (td.SampleDesc.Count == 1 && SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &st))) {
        g_ctx->CopyResource(st, src);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
          std::snprintf(name, sizeof(name), "/native_t%03d_f%02d.bmp", burst_t, burst_index);
          WriteBmp(dir + name, static_cast<const uint8_t*>(ms.pData), td.Width, td.Height, ms.RowPitch, false);
          g_ctx->Unmap(st, 0);
        }
        st->Release();
      }
    }
    if (back) back->Release();
    --burst_left;
    ++burst_index;
  }
  const size_t idx = g_dump_next - 1;
  if (idx >= g_dump_times.size()) return;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  if (t < g_dump_times[idx]) return;
  ++g_dump_next;
  if (const char* b = std::getenv("Q4_HLE_DUMP_BURST")) {
    burst_left = std::atoi(b);
    burst_index = 0;
    burst_t = int(g_dump_times[idx]);
  }
  // native image
  if (g_offscreen_tex) {
    D3D11_TEXTURE2D_DESC td;
    g_offscreen_tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &st))) {
      g_ctx->CopyResource(st, g_offscreen_tex);
      D3D11_MAPPED_SUBRESOURCE ms;
      if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
        std::snprintf(name, sizeof(name), "/native_t%03d.bmp", int(g_dump_times[idx]));
        WriteBmp(dir + name, static_cast<const uint8_t*>(ms.pData), td.Width, td.Height, ms.RowPitch, false);
        g_ctx->Unmap(st, 0);
      }
      st->Release();
    }
  }
  // what the window shows right now (Xenia's presenter in shadow mode)
  if (g_hwnd) {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    POINT tl{0, 0};
    ClientToScreen(g_hwnd, &tl);
    const int w = rc.right, h = rc.bottom;
    if (w > 0 && h > 0) {
      HDC screen = GetDC(nullptr);
      HDC mem = CreateCompatibleDC(screen);
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = w;
      bi.bmiHeader.biHeight = -h;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      void* bits = nullptr;
      HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
      HGDIOBJ old = SelectObject(mem, bmp);
      // PrintWindow with PW_RENDERFULLCONTENT (2) captures the window's own (DirectX) content even
      // when other windows cover it; fall back to the screen if it fails.
      if (!PrintWindow(g_hwnd, mem, 0x1 | 0x2)) BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY);
      GdiFlush();
      std::snprintf(name, sizeof(name), "/window_t%03d.bmp", int(g_dump_times[idx]));
      WriteBmp(dir + name, static_cast<const uint8_t*>(bits), w, h, w * 4, true);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      ReleaseDC(nullptr, screen);
    }
  }
  Log("dump: frame images written for t=%.1f", g_dump_times[idx]);
}
}  // namespace

void SetOffscreen(bool on) { g_offscreen = on; }
bool Offscreen() { return g_offscreen; }

void Present() {
  DumpIfDue();
  if (!g_swap) return;
  // The SDK's VdSwap paces the game to its vblank; V-Sync additionally waits for the display's
  // (GRAPHICS menu / q4hle_vsync; Q4_HLE_VSYNC=0/1 overrides).
  const char* e = std::getenv("Q4_HLE_VSYNC");
  const UINT interval = (e && *e) ? (*e == '1' ? 1 : 0) : (REXCVAR_GET(q4hle_vsync) ? 1 : 0);
  g_swap->Present(interval, 0);
  FollowWindowSize();
}

}  // namespace q4hle::native
